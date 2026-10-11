//test_kv_store.cc

#include <cassert>
#include <iostream>
#include <string>

#include "kv/kv_store.h"
#include "common/command.h"

// ==================== 基础操作 ====================

void TestEmpty() {
    KVStore store;
    assert(!store.Get("anything").has_value());
    std::cout << "[PASS] TestEmpty\n";
}

void TestPutGet() {
    KVStore store;
    store.ApplyPut("a", "1");
    assert(store.Get("a").value() == "1");
    assert(!store.Get("b").has_value());
    std::cout << "[PASS] TestPutGet\n";
}

void TestOverwrite() {
    KVStore store;
    store.ApplyPut("a", "1");
    store.ApplyPut("a", "2");
    assert(store.Get("a").value() == "2");
    std::cout << "[PASS] TestOverwrite\n";
}

void TestDelete() {
    KVStore store;
    store.ApplyPut("a", "1");
    assert(store.ApplyDelete("a"));
    assert(!store.Get("a").has_value());
    // 删除不存在的 key
    assert(!store.ApplyDelete("a"));
    std::cout << "[PASS] TestDelete\n";
}

void TestApplyCommand() {
    KVStore store;
    store.Apply(Command{Command::Type::PUT, "x", "10"});
    assert(store.Get("x").value() == "10");
    store.Apply(Command{Command::Type::DEL, "x", ""});
    assert(!store.Get("x").has_value());
    std::cout << "[PASS] TestApplyCommand\n";
}

// ==================== 序列化 ====================

void TestSerializeEmpty() {
    KVStore store;
    std::string snap = store.Serialize();
    assert(snap.empty());
    std::cout << "[PASS] TestSerializeEmpty\n";
}

void TestSerializeRoundTrip() {
    KVStore store;
    store.ApplyPut("a", "1");
    store.ApplyPut("b", "2");
    store.ApplyPut("c", "3");

    std::string snap = store.Serialize();

    KVStore restored;
    restored.Deserialize(snap);

    assert(restored.Get("a").value() == "1");
    assert(restored.Get("b").value() == "2");
    assert(restored.Get("c").value() == "3");
    std::cout << "[PASS] TestSerializeRoundTrip\n";
}

void TestSerializeEmptyValue() {
    // ★ 关键：空 value 必须正确处理
    KVStore store;
    store.ApplyPut("empty", "");
    store.ApplyPut("after", "value");

    std::string snap = store.Serialize();
    KVStore restored;
    restored.Deserialize(snap);

    assert(restored.Get("empty").has_value());
    assert(restored.Get("empty").value() == "");
    assert(restored.Get("after").value() == "value");   // 不能被跳过
    std::cout << "[PASS] TestSerializeEmptyValue\n";
}

void TestSerializeValueWithSpace() {
    // ★ 关键：value 含空格必须完整保留
    KVStore store;
    store.ApplyPut("key", "hello world");

    std::string snap = store.Serialize();
    KVStore restored;
    restored.Deserialize(snap);

    assert(restored.Get("key").value() == "hello world");
    std::cout << "[PASS] TestSerializeValueWithSpace\n";
}

void TestDeserializeOverwrite() {
    // 反序列化应该是"替换"，不是"合并"
    KVStore store;
    store.ApplyPut("a", "1");

    std::string snap = store.Serialize();

    KVStore restored;
    restored.ApplyPut("x", "old");   // 预先放脏数据
    restored.Deserialize(snap);       // 应该被覆盖

    assert(!restored.Get("x").has_value());
    assert(restored.Get("a").value() == "1");
    std::cout << "[PASS] TestDeserializeOverwrite\n";
}

void TestSerializeMultiple() {
    KVStore store;
    for (int i = 0; i < 100; i++) {
        store.ApplyPut("k" + std::to_string(i), "v" + std::to_string(i));
    }

    std::string snap = store.Serialize();

    KVStore restored;
    restored.Deserialize(snap);

    for (int i = 0; i < 100; i++) {
        assert(restored.Get("k" + std::to_string(i)).value()
               == "v" + std::to_string(i));
    }
    std::cout << "[PASS] TestSerializeMultiple\n";
}

// ==================== main ====================

int main() {
    std::cout << "===== KVStore Tests =====\n";

    TestEmpty();
    TestPutGet();
    TestOverwrite();
    TestDelete();
    TestApplyCommand();

    TestSerializeEmpty();
    TestSerializeRoundTrip();
    TestSerializeEmptyValue();
    TestSerializeValueWithSpace();
    TestDeserializeOverwrite();
    TestSerializeMultiple();

    std::cout << "\nAll KVStore tests passed!\n";
    return 0;
}
/*
空 value（TestSerializeEmptyValue）—— 验证 getline 格式不会漏

含空格的 value（TestSerializeValueWithSpace）—— 验证不会截断

反序列化覆盖（TestDeserializeOverwrite）—— 验证是"替换"而非"合并"

*/