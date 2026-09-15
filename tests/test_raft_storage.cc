#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#include "../src/raft/raft_storage.h"

// ==================== 辅助 ====================

// 生成唯一测试目录，避免并发冲突
std::string MakeTestDir(const std::string& name) {
    std::string dir = "../tests/test_data/raft_storage_" + name + "_" + std::to_string(getpid());
    std::filesystem::create_directories(dir);
    return dir;
}

void CleanupDir(const std::string& dir) {
    std::filesystem::remove_all(dir);
}

LogEntry MakePut(int64_t index, int64_t term,
                 const std::string& key, const std::string& value) {
    LogEntry e;
    e.index = index;
    e.term = term;
    e.command = Command{Command::Type::PUT, key, value};
    return e;
}

// ==================== 测试 ====================

void TestEmptyLoad() {
    std::string dir = MakeTestDir("empty");
    {
        RaftStorage storage(dir);
        auto meta = storage.LoadMeta();
        assert(meta.first == 0);
        assert(meta.second == -1);

        auto entries = storage.LoadLogEntries();
        assert(entries.empty());
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestEmptyLoad\n";
}

void TestMetaRoundTrip() {
    std::string dir = MakeTestDir("meta");
    {
        RaftStorage storage(dir);
        assert(storage.AppendMeta(5, 1));
    }
    {
        // 模拟重启：析构后重新构造
        RaftStorage storage(dir);
        auto meta = storage.LoadMeta();
        assert(meta.first == 5);
        assert(meta.second == 1);
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestMetaRoundTrip\n";
}

void TestMetaMultipleWrites() {
    std::string dir = MakeTestDir("meta_multi");
    {
        RaftStorage storage(dir);
        storage.AppendMeta(1, -1);
        storage.AppendMeta(2, 0);
        storage.AppendMeta(5, 1);
        storage.AppendMeta(6, 2);
    }
    {
        // 重启后读到的应该是最后一条
        RaftStorage storage(dir);
        auto meta = storage.LoadMeta();
        assert(meta.first == 6);
        assert(meta.second == 2);
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestMetaMultipleWrites\n";
}

void TestLogRoundTrip() {
    std::string dir = MakeTestDir("log");
    {
        RaftStorage storage(dir);
        assert(storage.AppendLogEntry(MakePut(1, 1, "a", "1")));
        assert(storage.AppendLogEntry(MakePut(2, 1, "b", "2")));
        assert(storage.AppendLogEntry(MakePut(3, 2, "c", "3")));
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        assert(entries.size() == 3);

        assert(entries[0].index == 1);
        assert(entries[0].term == 1);
        assert(entries[0].command.key == "a");
        assert(entries[0].command.value == "1");

        assert(entries[1].index == 2);
        assert(entries[1].command.key == "b");

        assert(entries[2].index == 3);
        assert(entries[2].term == 2);
        assert(entries[2].command.key == "c");
        assert(entries[2].command.value == "3");
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestLogRoundTrip\n";
}

void TestFullRestart() {
    std::string dir = MakeTestDir("restart");

    // 第一次运行：写 meta 和 log
    {
        RaftStorage storage(dir);
        storage.AppendMeta(5, 1);
        storage.AppendLogEntry(MakePut(1, 1, "x", "1"));
        storage.AppendLogEntry(MakePut(2, 5, "y", "2"));
    }

    // 模拟重启
    {
        RaftStorage storage(dir);
        auto meta = storage.LoadMeta();
        assert(meta.first == 5);
        assert(meta.second == 1);

        auto entries = storage.LoadLogEntries();
        assert(entries.size() == 2);
        assert(entries[0].index == 1);
        assert(entries[1].index == 2);
        assert(entries[1].term == 5);
        assert(entries[1].command.key == "y");
    }

    // 继续追加
    {
        RaftStorage storage(dir);
        storage.AppendMeta(6, 2);
        storage.AppendLogEntry(MakePut(3, 6, "z", "3"));
    }

    // 再次重启，验证累积效果
    {
        RaftStorage storage(dir);
        auto meta = storage.LoadMeta();
        assert(meta.first == 6);
        assert(meta.second == 2);

        auto entries = storage.LoadLogEntries();
        assert(entries.size() == 3);
        assert(entries[2].index == 3);
        assert(entries[2].term == 6);
        assert(entries[2].command.key == "z");
    }

    CleanupDir(dir);
    std::cout << "[PASS] TestFullRestart\n";
}

void TestDelEntryRoundTrip() {
    std::string dir = MakeTestDir("del");
    {
        RaftStorage storage(dir);
        LogEntry e;
        e.index = 1;
        e.term = 3;
        e.command = Command{Command::Type::DEL, "oldkey", ""};
        storage.AppendLogEntry(e);
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        assert(entries.size() == 1);
        assert(entries[0].command.type == Command::Type::DEL);
        assert(entries[0].command.key == "oldkey");
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestDelEntryRoundTrip\n";
}

// ==================== main ====================

int main() {
    std::cout << "===== RaftStorage Tests =====\n";

    TestEmptyLoad();
    TestMetaRoundTrip();
    TestMetaMultipleWrites();
    TestLogRoundTrip();
    TestDelEntryRoundTrip();
    TestFullRestart();

    std::cout << "\nAll RaftStorage tests passed!\n";
    return 0;
}