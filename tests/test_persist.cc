#include <cassert>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <filesystem>

#include "../src/kv/kv_store.h"
#include "../src/persist/persister.h"

// 生成一个唯一的测试文件路径，避免并发测试冲突
std::string MakeTestFile(const std::string& name) {
    std::string dir = "../tests/test_data";
    std::filesystem::create_directories(dir);
    return dir + "/test_" + name + "_" + std::to_string(getpid()) + ".log";
}

// ==================== 测试 1：基本持久化与恢复 ====================
void TestBasicPersistAndRecover() {
    std::string logFile = MakeTestFile("basic");
    std::remove(logFile.c_str());

    // 阶段 1：写入数据
    {
        auto persister = std::make_shared<Persister>(logFile);
        KVStore store(persister);

        store.Put("name", "alice");
        store.Put("age", "20");
        store.Put("city", "guangzhou");
        store.Delete("age");
        store.Put("name", "bob");   // 覆盖写
    }
    // 析构，日志已落盘

    // 阶段 2：从日志恢复
    {
        auto persister = std::make_shared<Persister>(logFile);
        KVStore store(persister);
        store.Load();

        auto v1 = store.Get("name");
        assert(v1.has_value() && v1.value() == "bob");

        auto v2 = store.Get("age");
        assert(!v2.has_value());   // 被 DELETE 了

        auto v3 = store.Get("city");
        assert(v3.has_value() && v3.value() == "guangzhou");

        std::cout << "[PASS] TestBasicPersistAndRecover" << std::endl;
    }

    std::remove(logFile.c_str());
}

// ==================== 测试 2：模拟崩溃后恢复 ====================
void TestCrashRecovery() {
    std::string logFile = MakeTestFile("crash");
    std::remove(logFile.c_str());

    constexpr int kCount = 1000;

    // 阶段 1：写入大量数据，然后模拟崩溃
    // 注意：Persister 每次 Append 都会 fsync，
    // 所以即使不析构，数据也已经落盘
    {
        auto persister = std::make_shared<Persister>(logFile);
        KVStore store(persister);

        for (int i = 0; i < kCount; i++) {
            store.Put("key_" + std::to_string(i), "value_" + std::to_string(i));
        }
        // 不主动清理，直接离开作用域
    }

    // 阶段 2：恢复并验证
    {
        auto persister = std::make_shared<Persister>(logFile);
        KVStore store(persister);
        store.Load();

        for (int i = 0; i < kCount; i++) {
            auto v = store.Get("key_" + std::to_string(i));
            assert(v.has_value());
            assert(v.value() == "value_" + std::to_string(i));
        }

        std::cout << "[PASS] TestCrashRecovery (" << kCount << " keys)" << std::endl;
    }

    std::remove(logFile.c_str());
}

// ==================== 测试 3：多线程写入后恢复 ====================
void TestMultiThreadPersist() {
    std::string logFile = MakeTestFile("mt");
    std::remove(logFile.c_str());

    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;

    {
        auto persister = std::make_shared<Persister>(logFile);
        KVStore store(persister);

        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; t++) {
            threads.emplace_back([&store, t]() {
                for (int i = 0; i < kPerThread; i++) {
                    std::string key = "t" + std::to_string(t) + "_k" + std::to_string(i);
                    store.Put(key, "v" + std::to_string(i));
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    // 恢复验证
    {
        auto persister = std::make_shared<Persister>(logFile);
        KVStore store(persister);
        store.Load();

        for (int t = 0; t < kThreads; t++) {
            for (int i = 0; i < kPerThread; i++) {
                std::string key = "t" + std::to_string(t) + "_k" + std::to_string(i);
                auto v = store.Get(key);
                assert(v.has_value());
                assert(v.value() == "v" + std::to_string(i));
            }
        }

        std::cout << "[PASS] TestMultiThreadPersist ("
                  << kThreads * kPerThread << " keys)" << std::endl;
    }

    std::remove(logFile.c_str());
}

// ==================== 测试 4：空日志文件 ====================
void TestEmptyLog() {
    std::string logFile = MakeTestFile("empty");
    std::remove(logFile.c_str());

    auto persister = std::make_shared<Persister>(logFile);
    KVStore store(persister);
    store.Load();   // 不应该崩溃

    auto v = store.Get("anything");
    assert(!v.has_value());

    std::cout << "[PASS] TestEmptyLog" << std::endl;

    std::remove(logFile.c_str());
}

// ==================== 测试 5：恢复后继续写入 ====================
void TestWriteAfterRecover() {
    std::string logFile = MakeTestFile("continue");
    std::remove(logFile.c_str());

    // 第一次写入
    {
        auto persister = std::make_shared<Persister>(logFile);
        KVStore store(persister);
        store.Put("a", "1");
        store.Put("b", "2");
    }

    // 恢复后继续写入
    {
        auto persister = std::make_shared<Persister>(logFile);
        KVStore store(persister);
        store.Load();

        assert(store.Get("a").value() == "1");
        assert(store.Get("b").value() == "2");

        store.Put("c", "3");
        store.Delete("a");
    }

    // 再次恢复，验证所有操作都在
    {
        auto persister = std::make_shared<Persister>(logFile);
        KVStore store(persister);
        store.Load();

        assert(!store.Get("a").has_value());
        assert(store.Get("b").value() == "2");
        assert(store.Get("c").value() == "3");

        std::cout << "[PASS] TestWriteAfterRecover" << std::endl;
    }

    //std::remove(logFile.c_str());
}

// ==================== main ====================
int main() {
    std::cout << "===== Persist Tests =====" << std::endl;

    TestEmptyLog();
    TestBasicPersistAndRecover();
    TestCrashRecovery();
    TestMultiThreadPersist();
    TestWriteAfterRecover();

    std::cout << "\nAll persistence tests passed!" << std::endl;
    return 0;
}