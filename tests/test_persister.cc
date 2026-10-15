//test_persister.cc
// 重写版，覆盖 persist/persister：
//   老接口：AppendToFile / LoadFromFile / Truncate / Size / GetFileName
//   新接口：OverwriteFile / ReadwholeFile / Exists / TruncateFront
//   并发：多线程 Append 不丢数据

#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "persist/persister.h"
#include "test_util.h"

static std::string MakeFilePath(const std::string& tag) {
    return testutil::MakeTempDir("persister_" + tag) + "/f.txt";
}

static std::string DirOf(const std::string& path) {
    return path.substr(0, path.find_last_of('/'));
}

// ==================== AppendToFile / LoadFromFile ====================

void TestAppendAndLoad() {
    std::string path = MakeFilePath("append");
    {
        Persister p(path);
        auto off1 = p.AppendToFile("hello");
        auto off2 = p.AppendToFile("world");
        CHECK(off1.has_value());
        CHECK(off2.has_value());
        CHECK_EQ(off1.value(), 0);
        CHECK_EQ(off2.value(), 6);   // "hello\n" = 6
    }
    {
        Persister p(path);
        auto lines = p.LoadFromFile();
        CHECK_EQ(lines.size(), 2u);
        CHECK_STR_EQ(lines[0], "hello");
        CHECK_STR_EQ(lines[1], "world");
    }
    testutil::CleanupDir(DirOf(path));
}

void TestAppendEmptyLines() {
    std::string path = MakeFilePath("empty_line");
    {
        Persister p(path);
        p.AppendToFile("a");
        p.AppendToFile("");       // 空行
        p.AppendToFile("b");
    }
    {
        Persister p(path);
        auto lines = p.LoadFromFile();
        CHECK_EQ(lines.size(), 3u);
        CHECK_STR_EQ(lines[0], "a");
        CHECK_STR_EQ(lines[1], "");
        CHECK_STR_EQ(lines[2], "b");
    }
    testutil::CleanupDir(DirOf(path));
}

void TestAppendPersistsAcrossInstances() {
    std::string path = MakeFilePath("reopen");
    {
        Persister p(path);
        p.AppendToFile("first");
    }
    {
        // 重新打开，Size 应从磁盘继承，追加偏移要接在后面
        Persister p(path);
        CHECK_EQ(p.Size(), 6);
        auto off = p.AppendToFile("second");
        CHECK(off.has_value());
        CHECK_EQ(off.value(), 6);
    }
    {
        Persister p(path);
        auto lines = p.LoadFromFile();
        CHECK_EQ(lines.size(), 2u);
        CHECK_STR_EQ(lines[1], "second");
    }
    testutil::CleanupDir(DirOf(path));
}

// ==================== Truncate ====================

void TestTruncate() {
    std::string path = MakeFilePath("truncate");
    {
        Persister p(path);
        p.AppendToFile("aaaa");   // 5 字节
        p.AppendToFile("bbbb");   // 5 字节
        CHECK_EQ(p.Size(), 10);
        CHECK(p.Truncate(5));
        CHECK_EQ(p.Size(), 5);
        auto lines = p.LoadFromFile();
        CHECK_EQ(lines.size(), 1u);
        CHECK_STR_EQ(lines[0], "aaaa");
        // 截断后继续追加
        auto off = p.AppendToFile("cccc");
        CHECK_EQ(off.value(), 5);
    }
    {
        Persister p(path);
        auto lines = p.LoadFromFile();
        CHECK_EQ(lines.size(), 2u);
        CHECK_STR_EQ(lines[0], "aaaa");
        CHECK_STR_EQ(lines[1], "cccc");
    }
    testutil::CleanupDir(DirOf(path));
}

void TestTruncateToZero() {
    std::string path = MakeFilePath("truncate0");
    {
        Persister p(path);
        p.AppendToFile("xxx");
        p.AppendToFile("yyy");
        CHECK(p.Truncate(0));
        CHECK_EQ(p.Size(), 0);
        CHECK(p.LoadFromFile().empty());
        auto off = p.AppendToFile("fresh");
        CHECK_EQ(off.value(), 0);
    }
    {
        Persister p(path);
        auto lines = p.LoadFromFile();
        CHECK_EQ(lines.size(), 1u);
        CHECK_STR_EQ(lines[0], "fresh");
    }
    testutil::CleanupDir(DirOf(path));
}

// ==================== Exists ====================

void TestExistsBeforeCreate() {
    std::string dir = testutil::MakeTempDir("persister_exists");
    std::string path = dir + "/snap.bin";
    {
        // needCreate=false 且文件不存在：fd 保持 -1，但 Exists 应为 false
        Persister p(path, false);
        CHECK(!p.Exists());
        // 这种状态下 ReadwholeFile 失败
        CHECK(!p.ReadwholeFile().has_value());
    }
    testutil::CleanupDir(dir);
}

void TestExistsAfterOverwrite() {
    std::string path = MakeFilePath("exists_after");
    {
        Persister p(path, false);
        CHECK(!p.Exists());
        CHECK(p.OverwriteFile("data"));
        CHECK(p.Exists());
    }
    testutil::CleanupDir(DirOf(path));
}

// ==================== OverwriteFile / ReadwholeFile ====================

void TestOverwriteRoundTrip() {
    std::string path = MakeFilePath("overwrite");
    {
        Persister p(path);
        p.AppendToFile("old content line");
        p.AppendToFile("another old line");
        CHECK(p.OverwriteFile("new"));
        CHECK_EQ(p.Size(), 3);
        auto raw = p.ReadwholeFile();
        CHECK(raw.has_value());
        CHECK_STR_EQ(raw.value(), "new");
    }
    {
        // 落盘后重新打开
        Persister p(path);
        auto raw = p.ReadwholeFile();
        CHECK(raw.has_value());
        CHECK_STR_EQ(raw.value(), "new");
    }
    testutil::CleanupDir(DirOf(path));
}

void TestOverwriteRepeatedlyNoStaleBytes() {
    // 关键：长内容覆盖成短内容后，文件不能残留旧尾巴
    std::string path = MakeFilePath("overwrite_no_stale");
    Persister p(path);
    CHECK(p.OverwriteFile("0123456789ABCDEF"));   // 16 字节
    CHECK(p.OverwriteFile("abc"));                 // 3 字节
    CHECK_EQ(p.Size(), 3);
    auto raw = p.ReadwholeFile();
    CHECK(raw.has_value());
    CHECK_STR_EQ(raw.value(), "abc");
    // 再覆盖成更长的
    CHECK(p.OverwriteFile("xyz-1234567890"));
    raw = p.ReadwholeFile();
    CHECK(raw.value() == "xyz-1234567890");
    testutil::CleanupDir(DirOf(path));
}

void TestReadwholeFileBinary() {
    // 二进制内容（含 '\0'）也要完整往返
    std::string path = MakeFilePath("binary");
    std::string bin;
    bin.push_back('A');
    bin.push_back('\0');
    bin.push_back('B');
    bin.push_back('\n');
    bin.push_back(static_cast<char>(0xFF));
    bin.push_back('C');
    {
        Persister p(path, false);
        CHECK(p.OverwriteFile(bin));
        auto raw = p.ReadwholeFile();
        CHECK(raw.has_value());
        CHECK(raw.value() == bin);
        CHECK_EQ(raw.value().size(), bin.size());
    }
    testutil::CleanupDir(DirOf(path));
}

// ==================== TruncateFront ====================

void TestTruncateFrontZero() {
    std::string path = MakeFilePath("tf_zero");
    Persister p(path);
    p.AppendToFile("hello");
    CHECK(p.TruncateFront(0));   // 切 0 字节，no-op
    CHECK_EQ(p.Size(), 6);
    auto raw = p.ReadwholeFile();
    CHECK(raw.has_value());
    CHECK_STR_EQ(raw.value(), "hello\n");
    testutil::CleanupDir(DirOf(path));
}

void TestTruncateFrontPartial() {
    std::string path = MakeFilePath("tf_partial");
    {
        Persister p(path);
        p.AppendToFile("0123456789");   // 11 字节
        p.AppendToFile("abcdefghij");   // 11 字节，共 22
        // 砍掉前 11 字节（第一行）
        CHECK(p.TruncateFront(11));
        CHECK_EQ(p.Size(), 11);
        auto raw = p.ReadwholeFile();
        CHECK(raw.has_value());
        CHECK_STR_EQ(raw.value(), "abcdefghij\n");
        // 截断后重新打开 fd 是 O_APPEND，追加在尾部
        auto off = p.AppendToFile("zz");
        CHECK_EQ(off.value(), 11);
    }
    {
        Persister p(path);
        auto lines = p.LoadFromFile();
        CHECK_EQ(lines.size(), 2u);
        CHECK_STR_EQ(lines[0], "abcdefghij");
        CHECK_STR_EQ(lines[1], "zz");
    }
    testutil::CleanupDir(DirOf(path));
}

void TestTruncateFrontAll() {
    std::string path = MakeFilePath("tf_all");
    {
        Persister p(path);
        p.AppendToFile("aa");
        p.AppendToFile("bb");
        // bytesToRemove >= size：全删
        CHECK(p.TruncateFront(100));
        CHECK_EQ(p.Size(), 0);
        CHECK(p.ReadwholeFile().value().empty());
        // 重新追加
        auto off = p.AppendToFile("new");
        CHECK_EQ(off.value(), 0);
    }
    {
        Persister p(path);
        auto lines = p.LoadFromFile();
        CHECK_EQ(lines.size(), 1u);
        CHECK_STR_EQ(lines[0], "new");
    }
    testutil::CleanupDir(DirOf(path));
}

// ==================== 并发 ====================

void TestConcurrentAppend() {
    std::string path = MakeFilePath("concurrent");
    constexpr int kThreads = 8;
    constexpr int kPerThread = 100;
    {
        Persister p(path);
        std::vector<std::thread> ths;
        for (int t = 0; t < kThreads; ++t) {
            ths.emplace_back([&p, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    std::string line = "t" + std::to_string(t)
                                     + "_" + std::to_string(i);
                    auto off = p.AppendToFile(line);
                    CHECK(off.has_value());
                }
            });
        }
        for (auto& th : ths) th.join();
        // 总行数必须精确（锁保护 + O_APPEND）
        auto lines = p.LoadFromFile();
        CHECK_EQ(lines.size(),
                 static_cast<size_t>(kThreads * kPerThread));
    }
    testutil::CleanupDir(DirOf(path));
}

int main() {
    std::cout << "===== Persister Tests =====\n";

    RUN_TEST(TestAppendAndLoad);
    RUN_TEST(TestAppendEmptyLines);
    RUN_TEST(TestAppendPersistsAcrossInstances);
    RUN_TEST(TestTruncate);
    RUN_TEST(TestTruncateToZero);
    RUN_TEST(TestExistsBeforeCreate);
    RUN_TEST(TestExistsAfterOverwrite);
    RUN_TEST(TestOverwriteRoundTrip);
    RUN_TEST(TestOverwriteRepeatedlyNoStaleBytes);
    RUN_TEST(TestReadwholeFileBinary);
    RUN_TEST(TestTruncateFrontZero);
    RUN_TEST(TestTruncateFrontPartial);
    RUN_TEST(TestTruncateFrontAll);
    RUN_TEST(TestConcurrentAppend);

    TEST_SUMMARY_RETURN();
}
