//test_kv_store.cc
// 重写版，覆盖 kv/kv_store：
//   基础操作（Put/Get/Delete/Apply）、覆盖与删除幂等
//   快照序列化（空表、空 value、含空格、中文、大数据、替换语义）
//   shared_mutex 下的并发读写

#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "kv/kv_store.h"
#include "common/command.h"
#include "test_util.h"

// ==================== 基础操作 ====================

void TestEmpty() {
    KVStore store;
    CHECK(!store.Get("anything").has_value());
}

void TestPutGet() {
    KVStore store;
    store.ApplyPut("a", "1");
    CHECK(store.Get("a").has_value());
    CHECK_STR_EQ(store.Get("a").value(), "1");
    CHECK(!store.Get("b").has_value());
}

void TestOverwrite() {
    KVStore store;
    store.ApplyPut("a", "1");
    store.ApplyPut("a", "2");
    store.ApplyPut("a", "3");
    CHECK_STR_EQ(store.Get("a").value(), "3");
}

void TestEmptyValue() {
    KVStore store;
    store.ApplyPut("k", "");
    auto v = store.Get("k");
    CHECK(v.has_value());
    CHECK_STR_EQ(v.value(), "");
}

void TestDelete() {
    KVStore store;
    store.ApplyPut("a", "1");
    CHECK(store.ApplyDelete("a"));
    CHECK(!store.Get("a").has_value());
    // 幂等：删除不存在的 key 返回 false，不崩溃
    CHECK(!store.ApplyDelete("a"));
    // 删了再写
    store.ApplyPut("a", "2");
    CHECK_STR_EQ(store.Get("a").value(), "2");
}

void TestApplyCommand() {
    KVStore store;
    store.Apply(Command{Command::Type::PUT, "x", "10"});
    CHECK_STR_EQ(store.Get("x").value(), "10");
    store.Apply(Command{Command::Type::DEL, "x", ""});
    CHECK(!store.Get("x").has_value());
    // GET 不改变状态机
    store.Apply(Command{Command::Type::GET, "x", ""});
    CHECK(!store.Get("x").has_value());
}

void TestManyKeys() {
    KVStore store;
    for (int i = 0; i < 1000; ++i) {
        store.ApplyPut("k" + std::to_string(i), "v" + std::to_string(i));
    }
    for (int i = 0; i < 1000; ++i) {
        CHECK_STR_EQ(store.Get("k" + std::to_string(i)).value(),
                     "v" + std::to_string(i));
    }
}

// ==================== 序列化 ====================

void TestSerializeEmpty() {
    KVStore store;
    CHECK_STR_EQ(store.Serialize(), "");
}

void TestSerializeRoundTrip() {
    KVStore store;
    store.ApplyPut("a", "1");
    store.ApplyPut("b", "2");
    store.ApplyPut("c", "3");

    KVStore restored;
    restored.Deserialize(store.Serialize());
    CHECK_STR_EQ(restored.Get("a").value(), "1");
    CHECK_STR_EQ(restored.Get("b").value(), "2");
    CHECK_STR_EQ(restored.Get("c").value(), "3");
}

void TestSerializeEmptyValueRoundTrip() {
    KVStore store;
    store.ApplyPut("empty", "");
    store.ApplyPut("after", "value");
    KVStore restored;
    restored.Deserialize(store.Serialize());
    CHECK(restored.Get("empty").has_value());
    CHECK_STR_EQ(restored.Get("empty").value(), "");
    CHECK_STR_EQ(restored.Get("after").value(), "value");
}

void TestSerializeValueWithSpace() {
    // 快照格式 key\\nvalue\\n，value 允许空格
    KVStore store;
    store.ApplyPut("greeting", "hello world foo bar");
    KVStore restored;
    restored.Deserialize(store.Serialize());
    CHECK_STR_EQ(restored.Get("greeting").value(), "hello world foo bar");
}

void TestSerializeChinese() {
    KVStore store;
    store.ApplyPut("名字", "你好，世界");
    store.ApplyPut("emoji", "value");
    KVStore restored;
    restored.Deserialize(store.Serialize());
    CHECK_STR_EQ(restored.Get("名字").value(), "你好，世界");
    CHECK_STR_EQ(restored.Get("emoji").value(), "value");
}

void TestSerializeLarge() {
    KVStore store;
    std::string big(100000, 'x');
    store.ApplyPut("big", big);
    for (int i = 0; i < 500; ++i) {
        store.ApplyPut("k" + std::to_string(i), "v" + std::to_string(i));
    }
    KVStore restored;
    restored.Deserialize(store.Serialize());
    CHECK_STR_EQ(restored.Get("big").value(), big);
    CHECK_STR_EQ(restored.Get("k499").value(), "v499");
}

void TestDeserializeIsReplace() {
    // 反序列化是"替换"而非"合并"
    KVStore restored;
    restored.ApplyPut("old", "x");
    KVStore store;
    store.ApplyPut("a", "1");
    restored.Deserialize(store.Serialize());
    CHECK(!restored.Get("old").has_value());
    CHECK_STR_EQ(restored.Get("a").value(), "1");
}

void TestDeserializeEmptyClears() {
    KVStore store;
    store.ApplyPut("a", "1");
    store.Deserialize("");   // 空快照 = 清空
    CHECK(!store.Get("a").has_value());
}

// ==================== 并发 ====================

void TestConcurrentWriteDistinctKeys() {
    KVStore store;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    std::vector<std::thread> ths;
    for (int t = 0; t < kThreads; ++t) {
        ths.emplace_back([&store, t] {
            for (int i = 0; i < kPerThread; ++i) {
                std::string key = "t" + std::to_string(t)
                                + "_" + std::to_string(i);
                store.ApplyPut(key, std::to_string(t * 1000 + i));
            }
        });
    }
    for (auto& th : ths) th.join();
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; ++i) {
            std::string key = "t" + std::to_string(t) + "_" + std::to_string(i);
            CHECK(store.Get(key).has_value());
            CHECK_STR_EQ(store.Get(key).value(),
                         std::to_string(t * 1000 + i));
        }
    }
}

void TestConcurrentReadWhileWrite() {
    // 读多写少，主要验证读写锁不崩溃、不读到撕裂
    KVStore store;
    store.ApplyPut("hot", "initial");
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                auto v = store.Get("hot");
                CHECK(v.has_value());
                // 只可能是初值或两个新值之一（写操作是整串覆盖）
                CHECK(*v == "initial" || *v == "v1" || *v == "v2");
                reads.fetch_add(1);
            }
        });
    }
    std::thread writer([&] {
        for (int i = 0; i < 1000; ++i) {
            store.ApplyPut("hot", (i % 2 == 0) ? "v1" : "v2");
        }
    });
    writer.join();
    stop.store(true);
    for (auto& r : readers) r.join();
    CHECK(reads.load() > 0);
}

int main() {
    std::cout << "===== KVStore Tests =====\n";

    RUN_TEST(TestEmpty);
    RUN_TEST(TestPutGet);
    RUN_TEST(TestOverwrite);
    RUN_TEST(TestEmptyValue);
    RUN_TEST(TestDelete);
    RUN_TEST(TestApplyCommand);
    RUN_TEST(TestManyKeys);
    RUN_TEST(TestSerializeEmpty);
    RUN_TEST(TestSerializeRoundTrip);
    RUN_TEST(TestSerializeEmptyValueRoundTrip);
    RUN_TEST(TestSerializeValueWithSpace);
    RUN_TEST(TestSerializeChinese);
    RUN_TEST(TestSerializeLarge);
    RUN_TEST(TestDeserializeIsReplace);
    RUN_TEST(TestDeserializeEmptyClears);
    RUN_TEST(TestConcurrentWriteDistinctKeys);
    RUN_TEST(TestConcurrentReadWhileWrite);

    TEST_SUMMARY_RETURN();
}
