//test_raft_log.cc

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "raft/raft_log.h"
#include "common/command.h"

// ==================== 辅助 ====================

// 和 raft_log.cc 里一致（不在头文件里，测试自己定义）
constexpr int64_t kNeedSnapshot = -1;

LogEntry MakePut(int64_t index, int64_t term,
                 const std::string& key, const std::string& value) {
    LogEntry e;
    e.index = index;
    e.term = term;
    e.command = Command{Command::Type::PUT, key, value};
    return e;
}

void FillLog(RaftLog& log, int64_t startIndex, int64_t endIndex, int64_t term) {
    for (int64_t i = startIndex; i <= endIndex; i++) {
        log.AppendEntry(term, Command{Command::Type::PUT,
                                       "k" + std::to_string(i),
                                       "v" + std::to_string(i)});
    }
}

// ==================== 基础测试 ====================

void TestEmptyLog() {
    RaftLog log;
    assert(log.Size() == 0);
    assert(log.LastIndex() == 0);
    assert(log.LastTerm() == 0);
    assert(log.FirstIndex() == 1);
    std::cout << "[PASS] TestEmptyLog\n";
}

void TestAppendAndGet() {
    RaftLog log;
    log.AppendEntry(1, Command{Command::Type::PUT, "a", "1"});
    log.AppendEntry(1, Command{Command::Type::PUT, "b", "2"});
    log.AppendEntry(2, Command{Command::Type::PUT, "c", "3"});

    assert(log.Size() == 3);
    assert(log.LastIndex() == 3);
    assert(log.LastTerm() == 2);
    assert(log.FirstIndex() == 1);

    assert(log.GetEntry(1)->command.key == "a");
    assert(log.GetEntry(2)->command.key == "b");
    assert(log.GetEntry(3)->term == 2);
    assert(!log.GetEntry(0).has_value());
    assert(!log.GetEntry(4).has_value());

    std::cout << "[PASS] TestAppendAndGet\n";
}

void TestGetEntries() {
    RaftLog log;
    FillLog(log, 1, 5, 1);

    // [2, 5) → index 2、3、4
    auto entries = log.GetEntries(2, 5);
    assert(entries.size() == 3);
    assert(entries[0].index == 2);
    assert(entries[2].index == 4);

    // 空区间
    assert(log.GetEntries(3, 3).empty());

    // 起点越界
    assert(log.GetEntries(0, 5).empty());

    // 终点越界
    assert(log.GetEntries(1, 100).empty());

    std::cout << "[PASS] TestGetEntries\n";
}

void TestTruncateFrom() {
    RaftLog log;
    FillLog(log, 1, 5, 1);

    assert(log.TruncateFrom(3));
    assert(log.Size() == 2);
    assert(log.LastIndex() == 2);

    std::cout << "[PASS] TestTruncateFrom\n";
}

void TestAppendFromNormal() {
    RaftLog log;
    FillLog(log, 1, 3, 1);

    std::vector<LogEntry> batch = {
        MakePut(4, 2, "d", "4"),
        MakePut(5, 2, "e", "5"),
    };
    assert(log.AppendFrom(4, batch));
    assert(log.LastIndex() == 5);
    assert(log.GetEntry(4)->term == 2);

    std::cout << "[PASS] TestAppendFromNormal\n";
}

void TestAppendFromOverwrite() {
    RaftLog log;
    FillLog(log, 1, 5, 1);

    // 覆盖 index 3~5，term 改为 2
    std::vector<LogEntry> batch = {
        MakePut(3, 2, "X", "1"),
        MakePut(4, 2, "Y", "2"),
        MakePut(5, 2, "Z", "3"),
    };
    assert(log.AppendFrom(3, batch));

    assert(log.LastIndex() == 5);
    assert(log.GetEntry(3)->term == 2);
    assert(log.GetEntry(3)->command.key == "X");
    assert(log.GetEntry(5)->command.key == "Z");

    std::cout << "[PASS] TestAppendFromOverwrite\n";
}

void TestAppendFromEmptyEntries() {
    RaftLog log;
    FillLog(log, 1, 5, 1);

    // 空 entries + 起点 3 → 截断 3~5
    assert(log.AppendFrom(3, {}));
    assert(log.LastIndex() == 2);

    std::cout << "[PASS] TestAppendFromEmptyEntries\n";
}

void TestAppendFromInvalid() {
    RaftLog log;
    FillLog(log, 1, 3, 1);

    // 起点越界
    assert(!log.AppendFrom(0, {}));
    assert(!log.AppendFrom(5, {}));   // LastIndex=3, 允许 4，不允许 5

    // entries index 不连续
    std::vector<LogEntry> bad = {
        MakePut(2, 1, "a", "1"),
        MakePut(4, 1, "c", "3"),   // 缺 index 3
    };
    assert(!log.AppendFrom(2, bad));

    std::cout << "[PASS] TestAppendFromInvalid\n";
}

// ==================== 快照相关 ====================

void TestSnapshotBasic() {
    RaftLog log;
    FillLog(log, 1, 5, 1);

    // 快照覆盖 1~3
    log.SetSnapshotIndexAndTerm(3, 1);

    assert(log.FirstIndex() == 4);
    assert(log.LastIndex() == 5);
    assert(log.LastTerm() == 1);
    assert(log.Size() == 2);   // 只剩 4、5

    // 快照覆盖的返回 nullopt
    assert(!log.GetEntry(1).has_value());
    assert(!log.GetEntry(2).has_value());
    assert(!log.GetEntry(3).has_value());

    // 内存日志正常
    assert(log.GetEntry(4).has_value());
    assert(log.GetEntry(5).has_value());

    std::cout << "[PASS] TestSnapshotBasic\n";
}

void TestSnapshotAll() {
    RaftLog log;
    FillLog(log, 1, 5, 1);

    log.SetSnapshotIndexAndTerm(5, 1);

    assert(log.FirstIndex() == 6);
    assert(log.LastIndex() == 5);       // 空日志返回 snapshotIndex_
    assert(log.LastTerm() == 1);        // 空日志返回 snapshotTerm_
    assert(log.Size() == 0);

    assert(!log.GetEntry(5).has_value());   // 已被覆盖
    assert(!log.GetEntry(6).has_value());   // 超出

    std::cout << "[PASS] TestSnapshotAll\n";
}

void TestSnapshotThenAppend() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);   // 剩 4、5

    // 追加新日志
    log.AppendEntry(2, Command{Command::Type::PUT, "new", "val"});
    assert(log.LastIndex() == 6);
    assert(log.GetEntry(6)->term == 2);
    assert(log.Size() == 3);   // 4、5、6

    std::cout << "[PASS] TestSnapshotThenAppend\n";
}

void TestSnapshotThenAppendFrom() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);   // 剩 4、5

    // 从 index=4 覆盖
    std::vector<LogEntry> batch = {
        MakePut(4, 2, "X", "1"),
        MakePut(5, 2, "Y", "2"),
        MakePut(6, 2, "Z", "3"),
    };
    assert(log.AppendFrom(4, batch));

    assert(log.LastIndex() == 6);
    assert(log.GetEntry(4)->term == 2);
    assert(log.GetEntry(6)->command.key == "Z");
    assert(log.Size() == 3);

    std::cout << "[PASS] TestSnapshotThenAppendFrom\n";
}

void TestSnapshotThenAppendFromCrossSnapshotBoundary() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);

    // 从 index=4 覆盖，含 4、5
    std::vector<LogEntry> batch = {
        MakePut(4, 2, "X", "1"),
        MakePut(5, 2, "Y", "2"),
    };
    assert(log.AppendFrom(4, batch));
    assert(log.LastIndex() == 5);
    assert(log.GetEntry(4)->term == 2);

    std::cout << "[PASS] TestSnapshotThenAppendFromCrossSnapshotBoundary\n";
}

void TestSnapshotGetEntries() {
    RaftLog log;
    FillLog(log, 1, 10, 1);
    log.SetSnapshotIndexAndTerm(5, 1);   // 剩 6~10

    // 正常区间 [7, 10) → 7、8、9
    auto entries = log.GetEntries(7, 10);
    assert(entries.size() == 3);
    assert(entries[0].index == 7);
    assert(entries[2].index == 9);

    // 起点被快照覆盖 → 返回空（当前实现是"区间越界就返回空"）
    auto entries2 = log.GetEntries(3, 8);
    assert(entries2.empty());   // 起点 < FirstIndex() 时返回 {}

    // 取全部内存日志
    auto all = log.GetEntries(log.FirstIndex(), log.LastIndex() + 1);
    assert(all.size() == 5);

    std::cout << "[PASS] TestSnapshotGetEntries\n";
}

void TestSnapshotTruncate() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);   // 剩 4、5

    // 截断到快照点之前 → 失败（不能碰快照）
    assert(!log.TruncateFrom(3));

    // 截断 index=5
    assert(log.TruncateFrom(5));
    assert(log.LastIndex() == 4);
    assert(log.Size() == 1);

    // 截断到 FirstIndex → 清空内存日志
    assert(log.TruncateFrom(4));
    assert(log.Size() == 0);
    assert(log.LastIndex() == 3);   // snapshotIndex_

    std::cout << "[PASS] TestSnapshotTruncate\n";
}

// ==================== CheckConsistency ====================

void TestCheckConsistencyEmptyLog() {
    RaftLog log;
    // prevLogIndex = 0 → 通过
    auto r = log.CheckConsistency(0, 0);
    assert(r.ok);

    std::cout << "[PASS] TestCheckConsistencyEmptyLog\n";
}

void TestCheckConsistencyBehind() {
    RaftLog log;
    FillLog(log, 1, 3, 1);

    // prevLogIndex = 5 > LastIndex(3)
    auto r = log.CheckConsistency(5, 1);
    assert(!r.ok);
    assert(r.conflictIndex == 4);   // LastIndex + 1

    std::cout << "[PASS] TestCheckConsistencyBehind\n";
}

void TestCheckConsistencyMatch() {
    RaftLog log;
    FillLog(log, 1, 5, 1);

    auto r = log.CheckConsistency(3, 1);
    assert(r.ok);

    std::cout << "[PASS] TestCheckConsistencyMatch\n";
}

void TestCheckConsistencyTermMismatch() {
    RaftLog log;
    FillLog(log, 1, 3, 1);          // 1~3 term=1
    FillLog(log, 4, 5, 2);          // 4~5 term=2

    // prevLogIndex=5 的 term 应为 2，给 1 → 不匹配
    auto r = log.CheckConsistency(5, 1);
    assert(!r.ok);
    assert(r.conflictIndex == 4);   // term=2 段的起点

    // prevLogIndex=5 的 term 应为 2，给 3 → 不匹配
    auto r2 = log.CheckConsistency(5, 3);
    assert(!r2.ok);
    assert(r2.conflictIndex == 4);

    std::cout << "[PASS] TestCheckConsistencyTermMismatch\n";
}

void TestCheckConsistencySnapshotEquals() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);

    // prevLogIndex == snapshotIndex_，term 匹配
    auto r = log.CheckConsistency(3, 1);
    assert(r.ok);

    // prevLogIndex == snapshotIndex_，term 不匹配 → kNeedSnapshot
    auto r2 = log.CheckConsistency(3, 2);
    assert(!r2.ok);
    assert(r2.conflictIndex == kNeedSnapshot);

    std::cout << "[PASS] TestCheckConsistencySnapshotEquals\n";
}

void TestCheckConsistencyStrictlyLessThanSnapshot() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);

    // prevLogIndex < snapshotIndex_ → kNeedSnapshot
    auto r = log.CheckConsistency(2, 1);
    assert(!r.ok);
    assert(r.conflictIndex == kNeedSnapshot);

    auto r2 = log.CheckConsistency(1, 1);
    assert(!r2.ok);
    assert(r2.conflictIndex == kNeedSnapshot);

    std::cout << "[PASS] TestCheckConsistencyStrictlyLessThanSnapshot\n";
}

void TestCheckConsistencyAfterSnapshotMatch() {
    RaftLog log;
    FillLog(log, 1, 5, 1);
    log.SetSnapshotIndexAndTerm(3, 1);

    // prevLogIndex=5，term=1（内存日志中）
    auto r = log.CheckConsistency(5, 1);
    assert(r.ok);

    std::cout << "[PASS] TestCheckConsistencyAfterSnapshotMatch\n";
}

void TestCheckConsistencyAfterSnapshotTermMismatch() {
    RaftLog log;
    FillLog(log, 1, 3, 1);
    FillLog(log, 4, 5, 2);
    log.SetSnapshotIndexAndTerm(3, 1);   // 剩 4、5，都是 term=2

    // prevLogIndex=5，给 term=1（不匹配，内存里的是 term=2）
    auto r = log.CheckConsistency(5, 1);
    assert(!r.ok);
    assert(r.conflictIndex == 4);   // term=2 段的最早位置是 4（不会退到快照前）

    std::cout << "[PASS] TestCheckConsistencyAfterSnapshotTermMismatch\n";
}

// ==================== main ====================

int main() {
    std::cout << "===== RaftLog Tests =====\n";

    // 基础
    TestEmptyLog();
    TestAppendAndGet();
    TestGetEntries();
    TestTruncateFrom();
    TestAppendFromNormal();
    TestAppendFromOverwrite();
    TestAppendFromEmptyEntries();
    TestAppendFromInvalid();

    // 快照
    TestSnapshotBasic();
    TestSnapshotAll();
    TestSnapshotThenAppend();
    TestSnapshotThenAppendFrom();
    TestSnapshotThenAppendFromCrossSnapshotBoundary();
    TestSnapshotGetEntries();
    TestSnapshotTruncate();

    // 一致性检查
    TestCheckConsistencyEmptyLog();
    TestCheckConsistencyBehind();
    TestCheckConsistencyMatch();
    TestCheckConsistencyTermMismatch();
    TestCheckConsistencySnapshotEquals();
    TestCheckConsistencyStrictlyLessThanSnapshot();
    TestCheckConsistencyAfterSnapshotMatch();
    TestCheckConsistencyAfterSnapshotTermMismatch();

    std::cout << "\nAll RaftLog tests passed!\n";
    return 0;
}

/*
基础	            空日志、追加、查询、区间查询、截断、批量追加、覆盖、空 entries 截断、非法参数
快照基础	        设置快照后的 FirstIndex/LastIndex/LastTerm、访问快照范围返回 nullopt
快照全覆盖	        日志全被快照覆盖时的行为
快照后追加	        AppendEntry 和 AppendFrom 在快照后正常
快照后 GetEntries	起点越界、正常区间、取全部
快照后 Truncate	    不能截断快照范围、能截断内存日志
一致性检查	        所有分支（prevLogIndex=0、落后、匹配、term 不匹配、等于快照点、小于快照点）
*/