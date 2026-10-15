//test_raft_log.cc
// 重写版，覆盖 raft/raft_log：
//   基础追加/查询/区间/截断、AppendFrom（覆盖/空 entries/非法）
//   快照边界（全覆盖、快照后追加、快照点超前、倒退无效）
//   GetTerm、CheckConsistency 全分支（含快照冲突 kNeedSnapshot）

#include <iostream>
#include <string>
#include <vector>

#include "raft/raft_log.h"
#include "common/command.h"
#include "common/config.h"
#include "test_util.h"

// 和 raft_log.cc 里一致（不在头文件里，测试自己定义）
constexpr int64_t kNeedSnapshot = -1;

static LogEntry MakePut(int64_t index, int64_t term,
                        const std::string& key, const std::string& value) {
    return LogEntry{index, term,
                    Command{Command::Type::PUT, key, value}};
}

static void FillLog(RaftLog& log, int64_t start, int64_t end, int64_t term) {
    for (int64_t i = start; i <= end; ++i) {
        log.AppendEntry(term, Command{Command::Type::PUT,
                                      "k" + std::to_string(i),
                                      "v" + std::to_string(i)});
    }
}

// ==================== 基础 ====================

void TestEmptyLog() {
    RaftLog log;
    CHECK_EQ(log.Size(), 0);
    CHECK_EQ(log.LastIndex(), 0);
    CHECK_EQ(log.LastTerm(), 0);
    CHECK_EQ(log.FirstIndex(), 1);
    CHECK_EQ(log.SnapshotIndex(), 0);
    CHECK_EQ(log.SnapshotTerm(), 0);
}

void TestAppendAndGet() {
    RaftLog log;
    log.AppendEntry(1, Command{Command::Type::PUT, "a", "1"});
    log.AppendEntry(1, Command{Command::Type::PUT, "b", "2"});
    log.AppendEntry(2, Command{Command::Type::PUT, "c", "3"});

    CHECK_EQ(log.Size(), 3);
    CHECK_EQ(log.LastIndex(), 3);
    CHECK_EQ(log.LastTerm(), 2);
    CHECK_EQ(log.FirstIndex(), 1);

    CHECK_STR_EQ(log.GetEntry(1)->command.key, "a");
    CHECK_STR_EQ(log.GetEntry(2)->command.key, "b");
    CHECK_EQ(log.GetEntry(3)->term, 2);
    CHECK(!log.GetEntry(0).has_value());
    CHECK(!log.GetEntry(4).has_value());
}

void TestGetTerm() {
    RaftLog log;
    FillLog(log, 1, 3, 1);
    FillLog(log, 4, 5, 2);
    CHECK_EQ(log.GetTerm(1), 1);
    CHECK_EQ(log.GetTerm(3), 1);
    CHECK_EQ(log.GetTerm(4), 2);
    CHECK_EQ(log.GetTerm(5), 2);
    CHECK_EQ(log.GetTerm(0), 0);   // 越界

    // 快照点 / 快照内
    log.SetSnapshotIndexAndTerm(3, 1);
    CHECK_EQ(log.GetTerm(3), 1);   // 恰好快照点 -> snapshotTerm
    CHECK_EQ(log.GetTerm(2), 0);   // 快照内 -> 0
    CHECK_EQ(log.GetTerm(4), 2);   // 内存日志
}

void TestGetEntries() {
    RaftLog log;
    FillLog(log, 1, 5, 1);

    auto entries = log.GetEntries(2, 5);   // [2,5) -> 2、3、4
    CHECK_EQ(entries.size(), 3u);
    CHECK_EQ(entries[0].index, 2);
    CHECK_EQ(entries[2].index, 4);

    CHECK(log.GetEntries(3, 3).empty());    // 空区间
    CHECK(log.GetEntries(0, 5).empty());    // 起点越界
    CHECK(log.GetEntries(1, 100).empty());  // 终点越界

    // 取全部
    auto all = log.GetEntries(1, 6);
    CHECK_EQ(all.size(), 5u);
}

void TestTruncateFrom() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    CHECK(log.TruncateFrom(3));
    CHECK_EQ(log.Size(), 2);
    CHECK_EQ(log.LastIndex(), 2);
    // 非法位置
    CHECK(!log.TruncateFrom(0));
    CHECK(!log.TruncateFrom(3));   // 已不存在
}

// ==================== AppendFrom ====================

void TestAppendFromNormal() {
    RaftLog log;
    FillLog(log, 1, 3, 1);
    std::vector<LogEntry> batch = {
        MakePut(4, 2, "d", "4"),
        MakePut(5, 2, "e", "5"),
    };
    CHECK(log.AppendFrom(4, batch));
    CHECK_EQ(log.LastIndex(), 5);
    CHECK_EQ(log.GetEntry(4)->term, 2);
}

void TestAppendFromOverwrite() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    std::vector<LogEntry> batch = {
        MakePut(3, 2, "X", "1"),
        MakePut(4, 2, "Y", "2"),
        MakePut(5, 2, "Z", "3"),
    };
    CHECK(log.AppendFrom(3, batch));
    CHECK_EQ(log.LastIndex(), 5);
    CHECK_EQ(log.GetEntry(3)->term, 2);
    CHECK_STR_EQ(log.GetEntry(3)->command.key, "X");
    CHECK_STR_EQ(log.GetEntry(5)->command.key, "Z");
}

void TestAppendFromEmptyEntries() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    // 空 entries + 起点 3 -> 截断 3~5（对应 commit 203d5ce 修复）
    CHECK(log.AppendFrom(3, {}));
    CHECK_EQ(log.LastIndex(), 2);
}

void TestAppendFromInvalid() {
    RaftLog log;
    FillLog(log, 1, 3, 1);
    CHECK(!log.AppendFrom(0, {}));    // 起点越界
    CHECK(!log.AppendFrom(5, {}));    // 合法起点最大是 LastIndex+1=4

    // entries index 不连续
    std::vector<LogEntry> bad = {
        MakePut(2, 1, "a", "1"),
        MakePut(4, 1, "c", "3"),
    };
    CHECK(!log.AppendFrom(2, bad));
}

// ==================== 快照 ====================

void TestSnapshotBasic() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);

    CHECK_EQ(log.FirstIndex(), 4);
    CHECK_EQ(log.LastIndex(), 5);
    CHECK_EQ(log.LastTerm(), 1);
    CHECK_EQ(log.Size(), 2);
    CHECK(!log.GetEntry(1).has_value());
    CHECK(!log.GetEntry(2).has_value());
    CHECK(!log.GetEntry(3).has_value());
    CHECK(log.GetEntry(4).has_value());
    CHECK(log.GetEntry(5).has_value());
}

void TestSnapshotAll() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(5, 1);

    CHECK_EQ(log.FirstIndex(), 6);
    CHECK_EQ(log.LastIndex(), 5);   // 空日志返回 snapshotIndex_
    CHECK_EQ(log.LastTerm(), 1);
    CHECK_EQ(log.Size(), 0);
    CHECK(!log.GetEntry(5).has_value());
    CHECK(!log.GetEntry(6).has_value());
}

void TestSnapshotAheadOfLog() {
    // 快照点可以比内存日志更新（InstallSnapshot 场景）
    RaftLog log;
    FillLog(log, 1, 3, 1);
    log.SetSnapshotIndexAndTerm(10, 5);
    CHECK_EQ(log.SnapshotIndex(), 10);
    CHECK_EQ(log.SnapshotTerm(), 5);
    CHECK_EQ(log.Size(), 0);
    CHECK_EQ(log.FirstIndex(), 11);
    CHECK_EQ(log.LastIndex(), 10);
}

void TestSnapshotCannotRegress() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);
    // 倒退无效
    log.SetSnapshotIndexAndTerm(2, 1);
    log.SetSnapshotIndexAndTerm(3, 9);
    CHECK_EQ(log.SnapshotIndex(), 3);
    CHECK_EQ(log.SnapshotTerm(), 1);
}

void TestSnapshotThenAppend() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);
    log.AppendEntry(2, Command{Command::Type::PUT, "new", "val"});
    CHECK_EQ(log.LastIndex(), 6);
    CHECK_EQ(log.GetEntry(6)->term, 2);
    CHECK_EQ(log.Size(), 3);
}

void TestSnapshotThenAppendFrom() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);
    std::vector<LogEntry> batch = {
        MakePut(4, 2, "X", "1"),
        MakePut(5, 2, "Y", "2"),
        MakePut(6, 2, "Z", "3"),
    };
    CHECK(log.AppendFrom(4, batch));
    CHECK_EQ(log.LastIndex(), 6);
    CHECK_EQ(log.GetEntry(4)->term, 2);
    CHECK_STR_EQ(log.GetEntry(6)->command.key, "Z");
    CHECK_EQ(log.Size(), 3);

    // 起点落在快照覆盖区 -> 拒绝
    CHECK(!log.AppendFrom(3, {}));
}

void TestSnapshotGetEntries() {
    RaftLog log;
    FillLog(log, 1, 10, 1);
    log.SetSnapshotIndexAndTerm(5, 1);   // 剩 6~10

    auto entries = log.GetEntries(7, 10);
    CHECK_EQ(entries.size(), 3u);
    CHECK_EQ(entries[0].index, 7);
    CHECK_EQ(entries[2].index, 9);

    CHECK(log.GetEntries(3, 8).empty());  // 起点被快照覆盖

    auto all = log.GetEntries(log.FirstIndex(), log.LastIndex() + 1);
    CHECK_EQ(all.size(), 5u);
}

void TestSnapshotTruncate() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);

    CHECK(!log.TruncateFrom(3));   // 不能碰快照
    CHECK(log.TruncateFrom(5));
    CHECK_EQ(log.LastIndex(), 4);
    CHECK_EQ(log.Size(), 1);
    CHECK(log.TruncateFrom(4));
    CHECK_EQ(log.Size(), 0);
    CHECK_EQ(log.LastIndex(), 3);   // snapshotIndex_
}

// ==================== CheckConsistency ====================

void TestCheckConsistencyEmptyLog() {
    RaftLog log;
    CHECK(log.CheckConsistency(0, 0).ok);
}

void TestCheckConsistencyBehind() {
    RaftLog log;
    FillLog(log, 1, 3, 1);
    auto r = log.CheckConsistency(5, 1);
    CHECK(!r.ok);
    CHECK_EQ(r.conflictIndex, 4);
}

void TestCheckConsistencyMatch() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    CHECK(log.CheckConsistency(3, 1).ok);
}

void TestCheckConsistencyTermMismatch() {
    RaftLog log;
    FillLog(log, 1, 3, 1);
    FillLog(log, 4, 5, 2);
    auto r = log.CheckConsistency(5, 1);
    CHECK(!r.ok);
    CHECK_EQ(r.conflictIndex, 4);   // term=2 段起点
    auto r2 = log.CheckConsistency(5, 3);
    CHECK(!r2.ok);
    CHECK_EQ(r2.conflictIndex, 4);
}

void TestCheckConsistencySnapshotEquals() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);

    CHECK(log.CheckConsistency(3, 1).ok);

    auto r2 = log.CheckConsistency(3, 2);
    CHECK(!r2.ok);
    CHECK_EQ(r2.conflictIndex, kNeedSnapshot);
}

void TestCheckConsistencyStrictlyLessThanSnapshot() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);
    auto r = log.CheckConsistency(2, 1);
    CHECK(!r.ok);
    CHECK_EQ(r.conflictIndex, kNeedSnapshot);
    auto r2 = log.CheckConsistency(1, 1);
    CHECK(!r2.ok);
    CHECK_EQ(r2.conflictIndex, kNeedSnapshot);
}

void TestCheckConsistencyAfterSnapshot() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);
    CHECK(log.CheckConsistency(5, 1).ok);
}

void TestCheckConsistencyAfterSnapshotTermMismatch() {
    RaftLog log;
    FillLog(log, 1, 3, 1);
    FillLog(log, 4, 5, 2);
    log.SetSnapshotIndexAndTerm(3, 1);
    auto r = log.CheckConsistency(5, 1);
    CHECK(!r.ok);
    CHECK_EQ(r.conflictIndex, 4);
}

int main() {
    std::cout << "===== RaftLog Tests =====\n";

    RUN_TEST(TestEmptyLog);
    RUN_TEST(TestAppendAndGet);
    RUN_TEST(TestGetTerm);
    RUN_TEST(TestGetEntries);
    RUN_TEST(TestTruncateFrom);
    RUN_TEST(TestAppendFromNormal);
    RUN_TEST(TestAppendFromOverwrite);
    RUN_TEST(TestAppendFromEmptyEntries);
    RUN_TEST(TestAppendFromInvalid);
    RUN_TEST(TestSnapshotBasic);
    RUN_TEST(TestSnapshotAll);
    RUN_TEST(TestSnapshotAheadOfLog);
    RUN_TEST(TestSnapshotCannotRegress);
    RUN_TEST(TestSnapshotThenAppend);
    RUN_TEST(TestSnapshotThenAppendFrom);
    RUN_TEST(TestSnapshotGetEntries);
    RUN_TEST(TestSnapshotTruncate);
    RUN_TEST(TestCheckConsistencyEmptyLog);
    RUN_TEST(TestCheckConsistencyBehind);
    RUN_TEST(TestCheckConsistencyMatch);
    RUN_TEST(TestCheckConsistencyTermMismatch);
    RUN_TEST(TestCheckConsistencySnapshotEquals);
    RUN_TEST(TestCheckConsistencyStrictlyLessThanSnapshot);
    RUN_TEST(TestCheckConsistencyAfterSnapshot);
    RUN_TEST(TestCheckConsistencyAfterSnapshotTermMismatch);

    TEST_SUMMARY_RETURN();
}
