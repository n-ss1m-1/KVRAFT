#include "../src/kv/kv_store.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

void TestSingleThread() {
    KVStore store;
    
    // put 然后 get
    store.put("hello", "world");
    auto val = store.get("hello");
    assert(val.has_value() && val.value() == "world");
    
    // 覆盖写
    store.put("hello", "world2");
    val = store.get("hello");
    assert(val.has_value() && val.value() == "world2");
    
    // remove 存在的 key 返回 true
    assert(store.remove("hello") == true);
    
    // remove 不存在的 key 返回 false
    assert(store.remove("hello") == false);
    
    // get 不存在的 key
    val = store.get("nonexistent");
    assert(!val.has_value());
    
    std::cout << "[PASS] TestSingleThread" << std::endl;
}

void TestMultiThread() {
    KVStore store;
    constexpr int kThreadNum = 8;
    constexpr int kKeysPerThread = 1000;
    
    std::vector<std::thread> threads;
    
    for (int t = 0; t < kThreadNum; t++) {
        threads.emplace_back([&store, t]() {
            for (int i = 0; i < kKeysPerThread; i++) {
                std::string key = "thread_" + std::to_string(t) + "_key_" + std::to_string(i);
                store.put(key, "value_" + std::to_string(i));
            }
        });
    }
    
    for (auto& th : threads) th.join();
    
    // 验证
    for (int t = 0; t < kThreadNum; t++) {
        for (int i = 0; i < kKeysPerThread; i++) {
            std::string key = "thread_" + std::to_string(t) + "_key_" + std::to_string(i);
            auto val = store.get(key);
            assert(val.has_value());
            assert(val.value() == "value_" + std::to_string(i));
        }
    }
    
    std::cout << "[PASS] TestMultiThread" << std::endl;
}

void TestConcurrentReadWrite() {
    KVStore store;
    
    for (int i = 0; i < 100; i++) {
        store.put("key_" + std::to_string(i), "initial");
    }
    
    std::vector<std::thread> threads;
    
    // 4 个写线程
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&store, t]() {
            for (int round = 0; round < 50; round++) {
                for (int i = 0; i < 50; i++) {
                    store.put("key_" + std::to_string(i),
                              "writer_" + std::to_string(t));
                }
            }
        });
    }
    
    // 4 个读线程
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&store]() {
            for (int round = 0; round < 200; round++) {
                for (int i = 0; i < 100; i++) {
                    auto val = store.get("key_" + std::to_string(i));
                    assert(val.has_value());
                    assert(!val.value().empty());
                }
            }
        });
    }
    
    for (auto& th : threads) th.join();
    
    std::cout << "[PASS] TestConcurrentReadWrite" << std::endl;
}

int main() {
    TestSingleThread();
    TestMultiThread();
    TestConcurrentReadWrite();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}