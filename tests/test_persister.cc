//test_persister.cc

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "persist/persister.h"

// ==================== 辅助 ====================

static int g_counter = 0;

std::string MakeTestDir(const std::string& name) {
    std::string dir = "test_data/persister_" + name + "_"
                    + std::to_string(getpid()) + "_"
                    + std::to_string(g_counter++);
    std::filesystem::create_directories(dir);
    return dir;
}

void CleanupDir(const std::string& dir) {
    std::filesystem::remove_all(dir);
}

int64_t FileSize(const std::string& path) {
    return std::filesystem::file_size(path);
}

// ==================== 基础 ====================

void TestAppendAndLoad() {
    std::string dir = MakeTestDir("append_load");
    std::string path = dir + "/test.log";
    {
        Persister p(path);
        auto o1 = p.AppendToFile("line1");
        assert(o1.has_value());
        assert(*o1 == 0);           // 第一次写从 0 开始

        auto o2 = p.AppendToFile("line2");
        assert(o2.has_value());
        assert(*o2 == 6);           // "line1\n" = 6 字节

        assert(p.Size() == 12);     // "line1\nline2\n"
    }
    {
        Persister p(path);
        auto lines = p.LoadFromFile();
        assert(lines.size() == 2);
        assert(lines[0] == "line1");
        assert(lines[1] == "line2");
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestAppendAndLoad\n";
}

void TestAppendReturnsOffset() {
    std::string dir = MakeTestDir("offset");
    std::string path = dir + "/test.log";
    Persister p(path);

    auto o1 = p.AppendToFile("aaaa");   // 5 bytes (incl \n)
    auto o2 = p.AppendToFile("bb");     // 3 bytes
    auto o3 = p.AppendToFile("");       // 1 byte (only '\n')

    assert(o1.has_value() && *o1 == 0);
    assert(o2.has_value() && *o2 == 5);
    assert(o3.has_value() && *o3 == 8);
    assert(p.Size() == 9);

    CleanupDir(dir);
    std::cout << "[PASS] TestAppendReturnsOffset\n";
}

// ==================== Truncate（尾部截断）====================

void TestTruncate() {
    std::string dir = MakeTestDir("truncate");
    std::string path = dir + "/test.log";
    {
        Persister p(path);
        p.AppendToFile("aaa");   // 4 bytes
        p.AppendToFile("bbb");   // 4 bytes
        p.AppendToFile("ccc");   // 4 bytes
        assert(p.Size() == 12);
    }
    {
        Persister p(path);
        // 截断到 8 字节 → 保留前两条
        assert(p.Truncate(8));
        assert(p.Size() == 8);
        assert(FileSize(path) == 8);

        auto lines = p.LoadFromFile();
        assert(lines.size() == 2);
        assert(lines[0] == "aaa");
        assert(lines[1] == "bbb");
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestTruncate\n";
}

void TestTruncateToZero() {
    std::string dir = MakeTestDir("truncate_zero");
    std::string path = dir + "/test.log";
    {
        Persister p(path);
        p.AppendToFile("aaa");
        p.AppendToFile("bbb");
        assert(p.Size() == 8);
    }
    {
        Persister p(path);
        assert(p.Truncate(0));
        assert(p.Size() == 0);
        assert(FileSize(path) == 0);
        assert(p.LoadFromFile().empty());
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestTruncateToZero\n";
}

// ==================== TruncateFront（头部截断）====================

void TestTruncateFrontPartial() {
    std::string dir = MakeTestDir("tf_partial");
    std::string path = dir + "/test.log";
    {
        Persister p(path);
        p.AppendToFile("aaa");   // [0, 4)
        p.AppendToFile("bbb");   // [4, 8)
        p.AppendToFile("ccc");   // [8, 12)
        assert(p.Size() == 12);
    }
    {
        Persister p(path);
        // 删前 4 字节 → 保留 "bbb" "ccc"
        assert(p.TruncateFront(4));           // ★ 会因 fsync bug 挂在这
        assert(p.Size() == 8);
        assert(FileSize(path) == 8);

        auto lines = p.LoadFromFile();
        assert(lines.size() == 2);
        assert(lines[0] == "bbb");
        assert(lines[1] == "ccc");

        // 追加：偏移从 8 开始
        auto o = p.AppendToFile("ddd");
        assert(o.has_value() && *o == 8);
        assert(p.Size() == 12);
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestTruncateFrontPartial\n";
}

void TestTruncateFrontFull() {
    std::string dir = MakeTestDir("tf_full");
    std::string path = dir + "/test.log";
    {
        Persister p(path);
        p.AppendToFile("aaa");
        p.AppendToFile("bbb");
    }
    {
        Persister p(path);
        // bytesToRemove >= size_ → 全清
        assert(p.TruncateFront(100));
        assert(p.Size() == 0);
        assert(FileSize(path) == 0);
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestTruncateFrontFull\n";
}

void TestTruncateFrontZero() {
    std::string dir = MakeTestDir("tf_zero");
    std::string path = dir + "/test.log";
    {
        Persister p(path);
        p.AppendToFile("aaa");
        p.AppendToFile("bbb");
    }
    {
        Persister p(path);
        assert(p.TruncateFront(0));           // 无操作
        assert(p.Size() == 8);

        assert(p.TruncateFront(-5));          // 负数 → 无操作
        assert(p.Size() == 8);

        auto lines = p.LoadFromFile();
        assert(lines.size() == 2);
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestTruncateFrontZero\n";
}

void TestTruncateFrontThenAppend() {
    // 反复截断 + 追加，验证 size_ 和 offset 一致
    std::string dir = MakeTestDir("tf_cycle");
    std::string path = dir + "/test.log";
    Persister p(path);

    for (int i = 0; i < 10; i++) {
        p.AppendToFile("aaaaaaaaaa");         // 11 bytes each
    }
    assert(p.Size() == 110);

    // 每轮删 11 字节
    for (int i = 0; i < 5; i++) {
        assert(p.TruncateFront(11));
    }
    assert(p.Size() == 55);

    auto lines = p.LoadFromFile();
    assert(lines.size() == 5);
    for (int i = 0; i < 5; i++) {
        assert(lines[i] == "aaaaaaaaaa");
    }

    // 再追加
    auto o = p.AppendToFile("bbbb");
    assert(o.has_value() && *o == 55);
    assert(p.Size() == 60);

    CleanupDir(dir);
    std::cout << "[PASS] TestTruncateFrontThenAppend\n";
}

// ==================== main ====================

int main() {
    std::cout << "===== Persister Tests =====\n";

    TestAppendAndLoad();
    TestAppendReturnsOffset();

    TestTruncate();
    TestTruncateToZero();

    TestTruncateFrontPartial();
    TestTruncateFrontFull();
    TestTruncateFrontZero();
    TestTruncateFrontThenAppend();

    std::cout << "\nAll Persister tests passed!\n";
    return 0;
}