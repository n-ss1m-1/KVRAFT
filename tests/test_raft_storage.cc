//test_raft_storage.cc
// 重写版（适配 c04d536 编排式重构后的新接口）：
//   Meta：空 / round-trip / 多次追加读最后
//   日志：顺序追加、乱序拒绝、AppendLogEntriesFrom 覆盖/空截断/gap
//   加载：重复 index 去重、gap 截断、按 snapshotIndex 加载快照后日志
//   快照：SaveSnapshot(Snapshot) 新接口、覆盖、损坏、二进制 data
//   TruncateLogUpTo：no-op / 部分 / 全删 / 截断后追加
//   综合：多次重启 + meta + 日志 + 快照协同

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "raft/raft_storage.h"
#include "common/command.h"
#include "test_util.h"

static LogEntry MakePut(int64_t index, int64_t term,
                        const std::string& key, const std::string& value) {
    return LogEntry{index, term,
                    Command{Command::Type::PUT, key, value}};
}

static LogEntry MakeDel(int64_t index, int64_t term, const std::string& key) {
    return LogEntry{index, term,
                    Command{Command::Type::DEL, key, ""}};
}

static Snapshot MakeSnap(int64_t idx, int64_t term, const std::string& data) {
    return Snapshot{idx, term, data};
}

// ==================== Meta ====================

void TestEmptyMeta() {
    std::string dir = testutil::MakeTempDir("raft_storage_meta_empty");
    RaftStorage storage(dir);
    auto [term, votedFor] = storage.LoadMeta();
    CHECK_EQ(term, 0);
    CHECK_EQ(votedFor, -1);
    testutil::CleanupDir(dir);
}

void TestMetaRoundTrip() {
    std::string dir = testutil::MakeTempDir("raft_storage_meta_rt");
    {
        RaftStorage storage(dir);
        CHECK(storage.AppendMeta(5, 1));
    }
    {
        RaftStorage storage(dir);
        auto [term, votedFor] = storage.LoadMeta();
        CHECK_EQ(term, 5);
        CHECK_EQ(votedFor, 1);
    }
    testutil::CleanupDir(dir);
}

void TestMetaMultipleWrites() {
    std::string dir = testutil::MakeTempDir("raft_storage_meta_multi");
    {
        RaftStorage storage(dir);
        storage.AppendMeta(1, -1);
        storage.AppendMeta(2, 0);
        storage.AppendMeta(7, 2);
    }
    {
        RaftStorage storage(dir);
        auto [term, votedFor] = storage.LoadMeta();
        CHECK_EQ(term, 7);
        CHECK_EQ(votedFor, 2);
    }
    testutil::CleanupDir(dir);
}

// ==================== 日志追加 ====================

void TestEmptyLog() {
    std::string dir = testutil::MakeTempDir("raft_storage_log_empty");
    RaftStorage storage(dir);
    CHECK(storage.LoadLogEntries().empty());
    testutil::CleanupDir(dir);
}

void TestBasicAppendAndLoad() {
    std::string dir = testutil::MakeTempDir("raft_storage_log_basic");
    {
        RaftStorage storage(dir);
        CHECK(storage.AppendLogEntry(MakePut(1, 1, "a", "1")));
        CHECK(storage.AppendLogEntry(MakePut(2, 1, "b", "2")));
        CHECK(storage.AppendLogEntry(MakeDel(3, 2, "a")));
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        CHECK_EQ(entries.size(), 3u);
        CHECK_EQ(entries[0].index, 1);
        CHECK_STR_EQ(entries[0].command.key, "a");
        CHECK(entries[2].command.type == Command::Type::DEL);
    }
    testutil::CleanupDir(dir);
}

void TestOutOfOrderRejected() {
    std::string dir = testutil::MakeTempDir("raft_storage_log_ooo");
    RaftStorage storage(dir);
    CHECK(storage.AppendLogEntry(MakePut(1, 1, "a", "1")));
    CHECK(!storage.AppendLogEntry(MakePut(3, 1, "c", "3")));  // 跳过 2
    CHECK(!storage.AppendLogEntry(MakePut(1, 1, "a", "1")));  // 重复
    CHECK(storage.AppendLogEntry(MakePut(2, 1, "b", "2")));
    testutil::CleanupDir(dir);
}

// ==================== AppendLogEntriesFrom ====================

void TestAppendFromNormal() {
    std::string dir = testutil::MakeTempDir("raft_storage_af_normal");
    {
        RaftStorage storage(dir);
        storage.AppendLogEntry(MakePut(1, 1, "a", "1"));
        storage.AppendLogEntry(MakePut(2, 1, "b", "2"));
        std::vector<LogEntry> batch = {
            MakePut(3, 1, "c", "3"),
            MakePut(4, 1, "d", "4"),
        };
        CHECK(storage.AppendLogEntriesFrom(3, batch));
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        CHECK_EQ(entries.size(), 4u);
        for (int64_t i = 0; i < 4; ++i)
            CHECK_EQ(entries[i].index, i + 1);
    }
    testutil::CleanupDir(dir);
}

void TestAppendFromOverwrite() {
    std::string dir = testutil::MakeTempDir("raft_storage_af_overwrite");
    {
        RaftStorage storage(dir);
        for (int i = 1; i <= 5; ++i)
            storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
    }
    {
        RaftStorage storage(dir);
        storage.LoadLogEntries();
        std::vector<LogEntry> batch = {
            MakePut(4, 2, "D", "44"),
            MakePut(5, 2, "E", "55"),
        };
        CHECK(storage.AppendLogEntriesFrom(4, batch));
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        CHECK_EQ(entries.size(), 5u);
        CHECK_STR_EQ(entries[0].command.key, "k1");
        CHECK_STR_EQ(entries[2].command.key, "k3");
        CHECK_EQ(entries[3].term, 2);
        CHECK_STR_EQ(entries[3].command.key, "D");
        CHECK_STR_EQ(entries[4].command.key, "E");
    }
    testutil::CleanupDir(dir);
}

void TestAppendFromEmptyEntries() {
    // 空 entries + 起点 <= lastPersisted -> 截断（ftruncate 覆盖语义）
    std::string dir = testutil::MakeTempDir("raft_storage_af_empty");
    {
        RaftStorage storage(dir);
        for (int i = 1; i <= 5; ++i)
            storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
        CHECK(storage.AppendLogEntriesFrom(3, {}));
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        CHECK_EQ(entries.size(), 2u);
        CHECK_EQ(entries[1].index, 2);
    }
    testutil::CleanupDir(dir);
}

void TestAppendFromInvalid() {
    std::string dir = testutil::MakeTempDir("raft_storage_af_invalid");
    RaftStorage storage(dir);
    storage.AppendLogEntry(MakePut(1, 1, "a", "1"));
    // startIndex=0 非法
    CHECK(!storage.AppendLogEntriesFrom(0, {MakePut(1, 1, "a", "1")}));
    // 起点产生 gap
    CHECK(!storage.AppendLogEntriesFrom(5, {MakePut(5, 1, "e", "5")}));
    // entries 自身不连续
    std::vector<LogEntry> bad = {
        MakePut(2, 1, "b", "2"),
        MakePut(4, 1, "d", "4"),
    };
    CHECK(!storage.AppendLogEntriesFrom(2, bad));
    testutil::CleanupDir(dir);
}

// ==================== 加载时去重 / gap ====================

void TestLoadWithDuplicates() {
    std::string dir = testutil::MakeTempDir("raft_storage_load_dup");
    {
        std::ofstream out(dir + "/raft.log");
        out << SerializeLogEntry(MakePut(1, 1, "a", "1")) << "\n";
        out << SerializeLogEntry(MakePut(2, 1, "b", "2")) << "\n";
        out << SerializeLogEntry(MakePut(2, 1, "b", "2")) << "\n";  // 重复
        out << SerializeLogEntry(MakePut(3, 1, "c", "3")) << "\n";
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        CHECK_EQ(entries.size(), 3u);
        CHECK_EQ(entries[0].index, 1);
        CHECK_EQ(entries[1].index, 2);
        CHECK_EQ(entries[2].index, 3);
    }
    testutil::CleanupDir(dir);
}

void TestLoadWithGap() {
    std::string dir = testutil::MakeTempDir("raft_storage_load_gap");
    {
        std::ofstream out(dir + "/raft.log");
        out << SerializeLogEntry(MakePut(1, 1, "a", "1")) << "\n";
        out << SerializeLogEntry(MakePut(2, 1, "b", "2")) << "\n";
        out << SerializeLogEntry(MakePut(4, 1, "d", "4")) << "\n";  // 缺 3
        out << SerializeLogEntry(MakePut(5, 1, "e", "5")) << "\n";
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries();
        // 遇 gap 只保留连续前缀 1、2
        CHECK_EQ(entries.size(), 2u);
        CHECK_EQ(entries[0].index, 1);
        CHECK_EQ(entries[1].index, 2);
    }
    testutil::CleanupDir(dir);
}

void TestLoadWithSnapshotIndex() {
    // 物理截断后，LoadLogEntries 必须以快照点为起点，否则会被当成 gap。
    // 注意：gap 会触发物理重建（清空文件），因此"错误用法"和"正确用法"
    // 必须用两个独立目录，不能共用（否则错误用法会先把文件清空）。

    // 场景 A：错误用法（不传快照点）——4 被当成 gap，返回空，且文件被重建
    std::string dirA = testutil::MakeTempDir("raft_storage_load_snap_bad");
    {
        RaftStorage storage(dirA);
        for (int i = 1; i <= 5; ++i)
            storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
        CHECK(storage.TruncateLogUpTo(3));   // 物理只剩 4、5
    }
    {
        RaftStorage storage(dirA);
        CHECK(storage.LoadLogEntries().empty());
        // gap 重建后文件已清空，新对象加载仍为空
        RaftStorage storage2(dirA);
        CHECK(storage2.LoadLogEntries().empty());
    }
    testutil::CleanupDir(dirA);

    // 场景 B：正确用法（传快照点 3）——4、5 正常加载
    std::string dirB = testutil::MakeTempDir("raft_storage_load_snap_ok");
    {
        RaftStorage storage(dirB);
        for (int i = 1; i <= 5; ++i)
            storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
        CHECK(storage.TruncateLogUpTo(3));   // 物理只剩 4、5
    }
    {
        RaftStorage storage(dirB);
        auto entries = storage.LoadLogEntries(3);
        CHECK_EQ(entries.size(), 2u);
        REQUIRE(entries.size() == 2);
        CHECK_EQ(entries[0].index, 4);
        CHECK_EQ(entries[1].index, 5);
    }
    testutil::CleanupDir(dirB);
}

// ==================== 快照 ====================

void TestNoSnapshot() {
    std::string dir = testutil::MakeTempDir("raft_storage_snap_none");
    RaftStorage storage(dir);
    CHECK(!storage.LoadSnapshot().has_value());
    CHECK(!storage.HasSnapshot());
    testutil::CleanupDir(dir);
}

void TestSnapshotRoundTrip() {
    std::string dir = testutil::MakeTempDir("raft_storage_snap_rt");
    {
        RaftStorage storage(dir);
        CHECK(storage.SaveSnapshot(MakeSnap(100, 5, "a\n1\nb\n2\n")));
    }
    {
        RaftStorage storage(dir);
        CHECK(storage.HasSnapshot());
        auto snap = storage.LoadSnapshot();
        CHECK(snap.has_value());
        CHECK_EQ(snap->lastIncludedIndex, 100);
        CHECK_EQ(snap->lastIncludedTerm, 5);
        CHECK_STR_EQ(snap->data, "a\n1\nb\n2\n");
    }
    testutil::CleanupDir(dir);
}

void TestSnapshotBinaryData() {
    std::string dir = testutil::MakeTempDir("raft_storage_snap_bin");
    std::string bin;
    for (int i = 0; i < 256; ++i) bin.push_back(static_cast<char>(i));
    {
        RaftStorage storage(dir);
        CHECK(storage.SaveSnapshot(MakeSnap(9, 4, bin)));
    }
    {
        RaftStorage storage(dir);
        auto snap = storage.LoadSnapshot();
        CHECK(snap.has_value());
        CHECK(snap->data == bin);
    }
    testutil::CleanupDir(dir);
}

void TestSnapshotOverwrite() {
    std::string dir = testutil::MakeTempDir("raft_storage_snap_overwrite");
    RaftStorage storage(dir);
    CHECK(storage.SaveSnapshot(MakeSnap(100, 5, "old-long-long-content")));
    CHECK(storage.SaveSnapshot(MakeSnap(200, 6, "new")));  // 短覆盖长
    auto snap = storage.LoadSnapshot();
    CHECK(snap.has_value());
    CHECK_EQ(snap->lastIncludedIndex, 200);
    CHECK_EQ(snap->lastIncludedTerm, 6);
    CHECK_STR_EQ(snap->data, "new");
    testutil::CleanupDir(dir);
}

void TestSnapshotEmptyData() {
    std::string dir = testutil::MakeTempDir("raft_storage_snap_empty");
    RaftStorage storage(dir);
    CHECK(storage.SaveSnapshot(MakeSnap(50, 3, "")));
    auto snap = storage.LoadSnapshot();
    CHECK(snap.has_value());
    CHECK_EQ(snap->lastIncludedIndex, 50);
    CHECK(snap->data.empty());
    testutil::CleanupDir(dir);
}

void TestSnapshotCorrupt() {
    std::string dir = testutil::MakeTempDir("raft_storage_snap_corrupt");
    {
        RaftStorage storage(dir);
        storage.SaveSnapshot(MakeSnap(100, 5, "data"));
    }
    std::filesystem::resize_file(dir + "/snapshot.bin", 10);  // 头不完整
    {
        RaftStorage storage(dir);
        CHECK(!storage.LoadSnapshot().has_value());
    }
    testutil::CleanupDir(dir);
}

// ==================== TruncateLogUpTo ====================

void TestTruncateLogUpToNoop() {
    std::string dir = testutil::MakeTempDir("raft_storage_tlu_noop");
    RaftStorage storage(dir);
    for (int i = 1; i <= 5; ++i)
        storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
    CHECK(storage.TruncateLogUpTo(0));
    CHECK(storage.TruncateLogUpTo(-1));
    CHECK_EQ(storage.LoadLogEntries().size(), 5u);
    testutil::CleanupDir(dir);
}

void TestTruncateLogUpToPartial() {
    std::string dir = testutil::MakeTempDir("raft_storage_tlu_partial");
    {
        RaftStorage storage(dir);
        for (int i = 1; i <= 5; ++i)
            storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
        CHECK(storage.TruncateLogUpTo(3));   // 剩 4、5
    }
    {
        // 物理文件只剩 4、5，用快照点 3 加载
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries(3);
        CHECK_EQ(entries.size(), 2u);
        CHECK_EQ(entries[0].index, 4);
        CHECK_EQ(entries[1].index, 5);
    }
    testutil::CleanupDir(dir);
}

void TestTruncateLogUpToAll() {
    std::string dir = testutil::MakeTempDir("raft_storage_tlu_all");
    {
        RaftStorage storage(dir);
        for (int i = 1; i <= 5; ++i)
            storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
        CHECK(storage.TruncateLogUpTo(5));
    }
    {
        RaftStorage storage(dir);
        CHECK(storage.LoadLogEntries(5).empty());
    }
    testutil::CleanupDir(dir);
}

void TestTruncateLogUpToThenAppend() {
    std::string dir = testutil::MakeTempDir("raft_storage_tlu_append");
    {
        RaftStorage storage(dir);
        for (int i = 1; i <= 5; ++i)
            storage.AppendLogEntry(MakePut(i, 1, "k" + std::to_string(i), "v"));
        CHECK(storage.TruncateLogUpTo(3));   // 物理剩 4、5
        CHECK(storage.AppendLogEntry(MakePut(6, 2, "k6", "v")));
    }
    {
        RaftStorage storage(dir);
        auto entries = storage.LoadLogEntries(3);
        CHECK_EQ(entries.size(), 3u);
        CHECK_EQ(entries[0].index, 4);
        CHECK_EQ(entries[1].index, 5);
        CHECK_EQ(entries[2].index, 6);
        CHECK_EQ(entries[2].term, 2);
    }
    testutil::CleanupDir(dir);
}

// ==================== 综合：重启恢复 ====================

void TestFullRestart() {
    std::string dir = testutil::MakeTempDir("raft_storage_full_restart");

    {
        RaftStorage storage(dir);
        storage.AppendMeta(5, 1);
        storage.AppendLogEntry(MakePut(1, 1, "x", "1"));
        storage.AppendLogEntry(MakePut(2, 5, "y", "2"));
    }
    {
        RaftStorage storage(dir);
        auto [term, votedFor] = storage.LoadMeta();
        CHECK_EQ(term, 5);
        CHECK_EQ(votedFor, 1);
        auto entries = storage.LoadLogEntries();
        CHECK_EQ(entries.size(), 2u);
        CHECK_EQ(entries[1].term, 5);
    }
    {
        // 继续追加 + 快照 + 截断旧日志
        RaftStorage storage(dir);
        storage.LoadLogEntries();
        storage.AppendMeta(6, 2);
        storage.AppendLogEntry(MakePut(3, 6, "z", "3"));
        CHECK(storage.SaveSnapshot(MakeSnap(3, 6, "x\n1\ny\n2\nz\n3\n")));
        CHECK(storage.TruncateLogUpTo(3));
    }
    {
        // 再次重启：meta 读最新、快照在、日志只剩快照点之后（这里为空）
        RaftStorage storage(dir);
        auto [term, votedFor] = storage.LoadMeta();
        CHECK_EQ(term, 6);
        CHECK_EQ(votedFor, 2);
        auto snap = storage.LoadSnapshot();
        CHECK(snap.has_value());
        CHECK_EQ(snap->lastIncludedIndex, 3);
        CHECK_EQ(snap->lastIncludedTerm, 6);
        CHECK_STR_EQ(snap->data, "x\n1\ny\n2\nz\n3\n");
        CHECK(storage.LoadLogEntries(3).empty());
    }
    testutil::CleanupDir(dir);
}

int main() {
    std::cout << "===== RaftStorage Tests =====\n";

    RUN_TEST(TestEmptyMeta);
    RUN_TEST(TestMetaRoundTrip);
    RUN_TEST(TestMetaMultipleWrites);
    RUN_TEST(TestEmptyLog);
    RUN_TEST(TestBasicAppendAndLoad);
    RUN_TEST(TestOutOfOrderRejected);
    RUN_TEST(TestAppendFromNormal);
    RUN_TEST(TestAppendFromOverwrite);
    RUN_TEST(TestAppendFromEmptyEntries);
    RUN_TEST(TestAppendFromInvalid);
    RUN_TEST(TestLoadWithDuplicates);
    RUN_TEST(TestLoadWithGap);
    RUN_TEST(TestLoadWithSnapshotIndex);
    RUN_TEST(TestNoSnapshot);
    RUN_TEST(TestSnapshotRoundTrip);
    RUN_TEST(TestSnapshotBinaryData);
    RUN_TEST(TestSnapshotOverwrite);
    RUN_TEST(TestSnapshotEmptyData);
    RUN_TEST(TestSnapshotCorrupt);
    RUN_TEST(TestTruncateLogUpToNoop);
    RUN_TEST(TestTruncateLogUpToPartial);
    RUN_TEST(TestTruncateLogUpToAll);
    RUN_TEST(TestTruncateLogUpToThenAppend);
    RUN_TEST(TestFullRestart);

    TEST_SUMMARY_RETURN();
}
