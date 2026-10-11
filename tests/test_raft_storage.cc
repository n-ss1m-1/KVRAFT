//test_raft_storage.cc

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

#include "raft/raft_storage.h"
#include "common/command.h"

// ==================== 辅助 ====================

static int g_counter = 0;

std::string MakeTestDir(const std::string& name) {
    std::string dir = "test_data/raft_storage_" + name + "_"
                    + std::to_string(getpid()) + "_"
                    + std::to_string(g_counter++);
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

LogEntry MakeDel(int64_t index, int64_t term, const std::string& key) {
    LogEntry e;
    e.index = index;
    e.term = term;
    e.command = Command{Command::Type::DEL, key, ""};
    return e;
}

// ==================== Meta 测试 ====================

void TestEmptyMeta() {
    std::string dir = MakeTestDir("meta_empty");
    RaftStorage storage(dir);
    auto [term, votedFor] = storage.LoadMeta();
    assert(term == 0);
    assert(votedFor == -1);
    CleanupDir(dir);
    std::cout << "[PASS] TestEmptyMeta\n";
}

void TestMetaRoundTrip() {
    std::string dir = MakeTestDir("meta_rt");
    {
        RaftStorage storage(dir);
        storage.AppendMeta(5, 1);
    }
    {
        RaftStorage storage(dir);
        auto [term, votedFor] = storage.LoadMeta();
        assert(term == 5);
        assert(votedFor == 1);
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestMetaRoundTrip\n";
}

void TestMetaMultipleWrites() {
    // 读最后一行
    std::string dir = MakeTestDir("meta_multi");
    {
        RaftStorage storage(dir);
        storage.AppendMeta(1, -1);
        storage.AppendMeta(2, 0);
        storage.AppendMeta(5, 1);
        storage.AppendMeta(7, 2);
    }
    {
        RaftStorage storage(dir);
        auto [term, votedFor] = storage.LoadMeta();
        assert(term == 7);
        assert(votedFor == 2);
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestMetaMultipleWrites\n";
}

// ==================== 日志追加测试 ====================

void TestEmptyLog() {
    std::string dir = MakeTestDir("log_empty");
    RaftStorage storage(dir);
    auto entries = storage.LoadLogEntries();
    assert(entries.empty());
    CleanupDir(dir);
    std::cout << "[PASS] TestEmptyLog\n";
}

void TestBasicAppendAndLoad() {
    std::string dir = MakeTestDir("log_basic");
    {
        RaftStorage storage(dir);
        assert(storage.AppendLogEntry(MakePut(1, 1, "a", "1")));
        assert(storage.AppendLogEntry(MakePut(2, 1, "b", "2")));
        assert(storage.AppendLogEntry(MakeDel(3, 2, "a")));
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        assert(entries.size() == 3);
        assert(entries[0].index == 1);
        assert(entries[0].command.key == "a");
        assert(entries[2].command.type == Command::Type::DEL);
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestBasicAppendAndLoad\n";
}

void TestOutOfOrderRejected() {
    std::string dir = MakeTestDir("log_ooo");
    RaftStorage storage(dir);
    assert(storage.AppendLogEntry(MakePut(1, 1, "a", "1")));
    // 跳过 index 2 直接写 3 → 拒绝
    assert(!storage.AppendLogEntry(MakePut(3, 1, "c", "3")));
    // 重复写 index 1 → 拒绝
    assert(!storage.AppendLogEntry(MakePut(1, 1, "a", "1")));
    // 顺序写 2 → 成功
    assert(storage.AppendLogEntry(MakePut(2, 1, "b", "2")));
    CleanupDir(dir);
    std::cout << "[PASS] TestOutOfOrderRejected\n";
}

// ==================== AppendLogEntriesFrom 测试 ====================

void TestAppendFromNormal() {
    std::string dir = MakeTestDir("af_normal");
    {
        RaftStorage storage(dir);
        storage.AppendLogEntry(MakePut(1, 1, "a", "1"));
        storage.AppendLogEntry(MakePut(2, 1, "b", "2"));

        // 从 index=3 追加 [3, 4]
        std::vector<LogEntry> batch = {
            MakePut(3, 1, "c", "3"),
            MakePut(4, 1, "d", "4"),
        };
        assert(storage.AppendLogEntriesFrom(3, batch));
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        assert(entries.size() == 4);
        for (int64_t i = 0; i < 4; i++) {
            assert(entries[i].index == i + 1);
        }
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestAppendFromNormal\n";
}

void TestAppendFromOverwrite() {
    std::string dir = MakeTestDir("af_overwrite");
    {
        RaftStorage storage(dir);
        for (int i = 1; i <= 5; i++) {
            storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
        }
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        assert(entries.size() == 5);

        // ★ 覆盖：从 index=4 开始，替换 4、5 为 term=2 的版本
        std::vector<LogEntry> batch = {
            MakePut(4, 2, "D", "44"),
            MakePut(5, 2, "E", "55"),
        };
        assert(storage.AppendLogEntriesFrom(4, batch));
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        assert(entries.size() == 5);
        // 1~3 不变
        assert(entries[0].command.key == "k1");
        assert(entries[1].command.key == "k2");
        assert(entries[2].command.key == "k3");
        // 4、5 被覆盖
        assert(entries[3].term == 2);
        assert(entries[3].command.key == "D");
        assert(entries[4].command.key == "E");
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestAppendFromOverwrite\n";
}

void TestAppendFromEmptyEntries() {
    // 空 entries + 起点 <= LastIndex → 截断
    std::string dir = MakeTestDir("af_empty");
    {
        RaftStorage storage(dir);
        for (int i = 1; i <= 5; i++) {
            storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
        }
        // 空 entries，从 index=3 开始 → 截断 3、4、5
        assert(storage.AppendLogEntriesFrom(3, {}));
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        assert(entries.size() == 2);
        assert(entries[1].index == 2);
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestAppendFromEmptyEntries\n";
}

void TestAppendFromGap() {
    // 起点 > LastIndex + 1 → 拒绝
    std::string dir = MakeTestDir("af_gap");
    RaftStorage storage(dir);
    storage.AppendLogEntry(MakePut(1, 1, "a", "1"));

    std::vector<LogEntry> batch = { MakePut(5, 1, "e", "5") };
    assert(!storage.AppendLogEntriesFrom(5, batch));

    CleanupDir(dir);
    std::cout << "[PASS] TestAppendFromGap\n";
}

// ==================== 加载时去重 / gap 处理 ====================

void TestLoadWithDuplicates() {
    std::string dir = MakeTestDir("load_dup");
    // 手工造一个含重复 index 的文件
    {
        std::ofstream out(dir + "/raft.log");
        out << SerializeLogEntry(MakePut(1, 1, "a", "1")) << "\n";
        out << SerializeLogEntry(MakePut(2, 1, "b", "2")) << "\n";
        out << SerializeLogEntry(MakePut(2, 1, "b", "2")) << "\n";   // 重复
        out << SerializeLogEntry(MakePut(3, 1, "c", "3")) << "\n";
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        assert(entries.size() == 3);
        assert(entries[0].index == 1);
        assert(entries[1].index == 2);
        assert(entries[2].index == 3);
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestLoadWithDuplicates\n";
}

void TestLoadWithGap() {
    std::string dir = MakeTestDir("load_gap");
    {
        std::ofstream out(dir + "/raft.log");
        out << SerializeLogEntry(MakePut(1, 1, "a", "1")) << "\n";
        out << SerializeLogEntry(MakePut(2, 1, "b", "2")) << "\n";
        out << SerializeLogEntry(MakePut(4, 1, "d", "4")) << "\n";   // 缺 3
        out << SerializeLogEntry(MakePut(5, 1, "e", "5")) << "\n";
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        // 遇 gap 截断，只保留 1、2
        assert(entries.size() == 2);
        assert(entries[0].index == 1);
        assert(entries[1].index == 2);
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestLoadWithGap\n";
}

// ==================== 快照测试 ====================

void TestNoSnapshot() {
    std::string dir = MakeTestDir("snap_none");
    RaftStorage storage(dir);
    auto snap = storage.LoadSnapshot();
    assert(!snap.has_value());
    assert(!storage.HasSnapshot());
    CleanupDir(dir);
    std::cout << "[PASS] TestNoSnapshot\n";
}

void TestSnapshotRoundTrip() {
    std::string dir = MakeTestDir("snap_rt");
    {
        RaftStorage storage(dir);
        assert(storage.SaveSnapshot(100, 5, "a 1\nb 2\n"));
    }
    {
        RaftStorage storage(dir);
        assert(storage.HasSnapshot());
        auto snap = storage.LoadSnapshot();
        assert(snap.has_value());
        assert(snap->lastIncludedIndex == 100);
        assert(snap->lastIncludedTerm == 5);
        assert(snap->data == "a 1\nb 2\n");
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestSnapshotRoundTrip\n";
}

void TestSnapshotOverwrite() {
    std::string dir = MakeTestDir("snap_overwrite");
    RaftStorage storage(dir);
    storage.SaveSnapshot(100, 5, "old");
    storage.SaveSnapshot(200, 6, "new");

    auto snap = storage.LoadSnapshot();
    assert(snap.has_value());
    assert(snap->lastIncludedIndex == 200);
    assert(snap->lastIncludedTerm == 6);
    assert(snap->data == "new");
    CleanupDir(dir);
    std::cout << "[PASS] TestSnapshotOverwrite\n";
}

void TestSnapshotEmptyData() {
    std::string dir = MakeTestDir("snap_empty_data");
    RaftStorage storage(dir);
    storage.SaveSnapshot(50, 3, "");

    auto snap = storage.LoadSnapshot();
    assert(snap.has_value());
    assert(snap->lastIncludedIndex == 50);
    assert(snap->data.empty());
    CleanupDir(dir);
    std::cout << "[PASS] TestSnapshotEmptyData\n";
}

void TestSnapshotCorrupt() {
    std::string dir = MakeTestDir("snap_corrupt");
    {
        RaftStorage storage(dir);
        storage.SaveSnapshot(100, 5, "data");
    }
    // 手工截断到 10 字节（header 未完整）
    {
        std::filesystem::resize_file(dir + "/snapshot.bin", 10);
    }
    {
        RaftStorage storage(dir);
        auto snap = storage.LoadSnapshot();
        assert(!snap.has_value());   // 读取失败 → nullopt
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestSnapshotCorrupt\n";
}


// ==================== TruncateLogUpTo ====================

void TestTruncateLogUpToNoop() {
    std::string dir = MakeTestDir("tlu_noop");
    RaftStorage storage(dir);
    for (int i = 1; i <= 5; i++) {
        storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
    }
    // index <= 0 → 无操作
    assert(storage.TruncateLogUpTo(0));
    assert(storage.TruncateLogUpTo(-1));

    auto entries = storage.LoadLogEntries();
    assert(entries.size() == 5);

    CleanupDir(dir);
    std::cout << "[PASS] TestTruncateLogUpToNoop\n";
}

void TestTruncateLogUpToPartial() {
    std::string dir = MakeTestDir("tlu_partial");
    {
        RaftStorage storage(dir);
        for (int i = 1; i <= 5; i++) {
            storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
        }
        // 删 1~3，保留 4、5
        assert(storage.TruncateLogUpTo(3));
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        // ★ 关键：必须是 2 条（4、5），不能是 3 条（3、4、5）
        assert(entries.size() == 2);
        assert(entries[0].index == 4);
        assert(entries[1].index == 5);
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestTruncateLogUpToPartial\n";
}

void TestTruncateLogUpToAll() {
    std::string dir = MakeTestDir("tlu_all");
    {
        RaftStorage storage(dir);
        for (int i = 1; i <= 5; i++) {
            storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
        }
        // index >= lastPersistedIndex_ → 全清
        assert(storage.TruncateLogUpTo(5));
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        assert(entries.empty());
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestTruncateLogUpToAll\n";
}

void TestTruncateLogUpToThenAppend() {
    std::string dir = MakeTestDir("tlu_append");
    {
        RaftStorage storage(dir);
        for (int i = 1; i <= 5; i++) {
            storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
        }
        assert(storage.TruncateLogUpTo(3));   // 剩 4、5
        // 追加新日志，index 从 6 开始
        assert(storage.AppendLogEntry(MakePut(6, 2, "k6", "v")));
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        assert(entries.size() == 3);          // 4、5、6
        assert(entries[0].index == 4);
        assert(entries[1].index == 5);
        assert(entries[2].index == 6);
        assert(entries[2].term == 2);
    }
    CleanupDir(dir);
    std::cout << "[PASS] TestTruncateLogUpToThenAppend\n";
}

// ==================== 综合：重启恢复 ====================

void TestFullRestart() {
    std::string dir = MakeTestDir("full_restart");

    // 第一次：写 meta + 日志
    {
        RaftStorage storage(dir);
        storage.AppendMeta(5, 1);
        storage.AppendLogEntry(MakePut(1, 1, "x", "1"));
        storage.AppendLogEntry(MakePut(2, 5, "y", "2"));
        storage.SaveSnapshot(0, 0, "");   // 无快照
    }

    // 重启
    {
        RaftStorage storage(dir);
        auto [term, votedFor] = storage.LoadMeta();
        assert(term == 5);
        assert(votedFor == 1);

        auto entries = storage.LoadLogEntries();
        assert(entries.size() == 2);
        assert(entries[1].term == 5);
    }

    // 继续追加 + 做快照
    {
        RaftStorage storage(dir);
        storage.AppendMeta(6, 2);
        storage.AppendLogEntry(MakePut(3, 6, "z", "3"));
        storage.SaveSnapshot(3, 6, "x 1\ny 2\nz 3\n");
    }

    // 再次重启
    {
        RaftStorage storage(dir);
        auto [term, votedFor] = storage.LoadMeta();
        assert(term == 6);
        assert(votedFor == 2);

        auto snap = storage.LoadSnapshot();
        assert(snap.has_value());
        assert(snap->lastIncludedIndex == 3);
        assert(snap->lastIncludedTerm == 6);
        assert(snap->data == "x 1\ny 2\nz 3\n");
    }

    CleanupDir(dir);
    std::cout << "[PASS] TestFullRestart\n";
}

// ==================== main ====================

int main() {
    std::cout << "===== RaftStorage Tests =====\n";

    // Meta
    TestEmptyMeta();
    TestMetaRoundTrip();
    TestMetaMultipleWrites();

    // 日志
    TestEmptyLog();
    TestBasicAppendAndLoad();
    TestOutOfOrderRejected();
    TestAppendFromNormal();
    TestAppendFromOverwrite();
    TestAppendFromEmptyEntries();
    TestAppendFromGap();
    TestLoadWithDuplicates();
    TestLoadWithGap();

    // 快照
    TestNoSnapshot();
    TestSnapshotRoundTrip();
    TestSnapshotOverwrite();
    TestSnapshotEmptyData();
    TestSnapshotCorrupt();

    //TruncateLogUpTo
    TestTruncateLogUpToNoop();
    TestTruncateLogUpToPartial();
    TestTruncateLogUpToAll();
    TestTruncateLogUpToThenAppend();

    // 综合
    TestFullRestart();

    std::cout << "\nAll RaftStorage tests passed!\n";
    return 0;
}
/*
重复日志（TestLoadWithDuplicates）—— 验证 map 去重

gap 日志（TestLoadWithGap）—— 验证遇 gap 截断

覆盖追加（TestAppendFromOverwrite）—— 验证 ftruncate 生效

空 entries 截断（TestAppendFromEmptyEntries）—— 验证 Raft 语义

快照往返（TestSnapshotRoundTrip）—— 验证二进制格式

快照损坏（TestSnapshotCorrupt）—— 验证防御

完整重启（TestFullRestart）—— 验证 meta + 日志 + 快照三者协同
*/


// TODO:   RaftStorage::LoadLogEntries(int64_t) 需要传入snapshotIndex，此处需要添加传入参数(暂时用默认参数 0 替代)