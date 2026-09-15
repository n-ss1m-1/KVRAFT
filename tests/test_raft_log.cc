// tests/test_raft_log.cc

#include <cassert>
#include <iostream>
#include <string>
#include "../src/raft/raft_log.h"

// ==================== 辅助函数 ====================

// 构造一个 LogEntry，方便测试
LogEntry MakeEntry(int64_t term, int64_t index) {
    LogEntry e;
    e.index = index;
    e.term = term;
    e.command = Command{Command::Type::PUT,
                        "key" + std::to_string(index),
                        "value" + std::to_string(index)};
    return e;
}

// 构造一个 Command
Command MakeCommand(const std::string& key, const std::string& value) {
    return Command{Command::Type::PUT, key, value};
}

// ==================== 测试用例 ====================

void TestEmptyLog() {
    // Arrange
    RaftLog log;

    // Assert
    assert(log.Size() == 0);
    assert(log.LastIndex() == 0);
    assert(log.LastTerm() == 0);

    std::cout << "[PASS] TestEmptyLog\n";
}

void TestAppendSingle() {
    // Arrange
    RaftLog log;

    // Act
    log.AppendEntry(1, MakeCommand("a", "1"));

    // Assert
    assert(log.Size() == 1);
    assert(log.LastIndex() == 1);
    assert(log.LastTerm() == 1);

    auto e = log.GetEntry(1);
    assert(e.has_value());
    assert(e->index == 1);
    assert(e->term == 1);
    assert(e->command.key == "a");

    std::cout << "[PASS] TestAppendSingle\n";
}

void TestAppendMultiple() {
    // Arrange
    RaftLog log;

    // Act
    log.AppendEntry(1, MakeCommand("a", "1"));
    log.AppendEntry(1, MakeCommand("b", "2"));
    log.AppendEntry(2, MakeCommand("c", "3"));

    // Assert
    assert(log.Size() == 3);
    assert(log.LastIndex() == 3);
    assert(log.LastTerm() == 2);

    // 逐条验证
    assert(log.GetEntry(1)->term == 1);
    assert(log.GetEntry(1)->command.key == "a");
    assert(log.GetEntry(2)->term == 1);
    assert(log.GetEntry(2)->command.key == "b");
    assert(log.GetEntry(3)->term == 2);
    assert(log.GetEntry(3)->command.key == "c");

    std::cout << "[PASS] TestAppendMultiple\n";
}

void TestGetEntryOutOfRange() {
    // Arrange
    RaftLog log;
    log.AppendEntry(1, MakeCommand("a", "1"));
    log.AppendEntry(1, MakeCommand("b", "2"));

    // Act & Assert
    assert(!log.GetEntry(0).has_value());    // index 太小
    assert(!log.GetEntry(-1).has_value());   // 负数
    assert(!log.GetEntry(3).has_value());    // 超过 Size

    std::cout << "[PASS] TestGetEntryOutOfRange\n";
}

void TestGetEntriesRange() {
    // Arrange
    RaftLog log;
    for (int i = 1; i <= 5; i++) {
        log.AppendEntry(1, MakeCommand("k" + std::to_string(i), "v"));
    }

    // Act & Assert

    // 取 [2, 5) = index 2, 3, 4
    auto range = log.GetEntries(2, 5);
    assert(range.size() == 3);
    assert(range[0].index == 2);
    assert(range[1].index == 3);
    assert(range[2].index == 4);

    // 空区间 [3, 3)
    auto empty = log.GetEntries(3, 3);
    assert(empty.empty());

    // 非法区间 [5, 2)
    auto invalid = log.GetEntries(5, 2);
    assert(invalid.empty());

    // 超出范围
    auto outOfRange = log.GetEntries(1, 100);
    assert(outOfRange.empty());

    std::cout << "[PASS] TestGetEntriesRange\n";
}

void TestTruncateFrom() {
    // Arrange
    RaftLog log;
    for (int i = 1; i <= 5; i++) {
        log.AppendEntry(1, MakeCommand("k" + std::to_string(i), "v"));
    }

    // Act
    log.TruncateFrom(3);   // 删除 index 3、4、5

    // Assert
    assert(log.Size() == 2);
    assert(log.LastIndex() == 2);
    assert(log.GetEntry(1).has_value());
    assert(log.GetEntry(2).has_value());
    assert(!log.GetEntry(3).has_value());

    std::cout << "[PASS] TestTruncateFrom\n";
}

void TestTruncateFromOutOfRange() {
    // Arrange
    RaftLog log;
    log.AppendEntry(1, MakeCommand("a", "1"));

    // Act & Assert
    log.TruncateFrom(0);      // 非法：无操作
    log.TruncateFrom(-1);     // 非法：无操作
    log.TruncateFrom(100);    // 越界：无操作

    assert(log.Size() == 1);  // 没变

    std::cout << "[PASS] TestTruncateFromOutOfRange\n";
}

void TestAppendFromNormal() {
    // Arrange
    RaftLog log;
    log.AppendEntry(1, MakeCommand("a", "1"));
    log.AppendEntry(1, MakeCommand("b", "2"));

    // 准备要追加的日志：index = 3, 4, 5
    std::vector<LogEntry> entries = {
        MakeEntry(2, 3),
        MakeEntry(2, 4),
        MakeEntry(2, 5),
    };

    // Act
    bool ok = log.AppendFrom(3, entries);

    // Assert
    assert(ok);
    assert(log.Size() == 5);
    assert(log.LastIndex() == 5);
    assert(log.GetEntry(3)->term == 2);
    assert(log.GetEntry(4)->term == 2);
    assert(log.GetEntry(5)->term == 2);

    std::cout << "[PASS] TestAppendFromNormal\n";
}

void TestAppendFromConflict() {
    // Arrange
    RaftLog log;
    log.AppendEntry(1, MakeCommand("a", "1"));
    log.AppendEntry(1, MakeCommand("b", "2"));
    log.AppendEntry(1, MakeCommand("c", "3"));   // 旧 term=1

    // 从 index=3 追加 term=2 的日志，应该覆盖 index=3
    std::vector<LogEntry> entries = {
        MakeEntry(2, 3),
        MakeEntry(2, 4),
    };

    // Act
    bool ok = log.AppendFrom(3, entries);

    // Assert
    assert(ok);
    assert(log.Size() == 4);
    assert(log.GetEntry(3)->term == 2);   // 被覆盖
    assert(log.GetEntry(4)->term == 2);

    std::cout << "[PASS] TestAppendFromConflict\n";
}

void TestAppendFromIndexMismatch() {
    // Arrange
    RaftLog log;
    log.AppendEntry(1, MakeCommand("a", "1"));

    // entries[0].index = 5，但 startIndex = 2
    std::vector<LogEntry> entries = { MakeEntry(2, 5) };

    // Act
    bool ok = log.AppendFrom(2, entries);

    // Assert
    assert(!ok);
    assert(log.Size() == 1);   // 日志没变！

    std::cout << "[PASS] TestAppendFromIndexMismatch\n";
}

void TestAppendFromNotContinuous() {
    // Arrange
    RaftLog log;
    log.AppendEntry(1, MakeCommand("a", "1"));

    // index 应该连续：2, 3, 4，但这里跳过了 3
    std::vector<LogEntry> entries = {
        MakeEntry(2, 2),
        MakeEntry(2, 4),   // 应该是 index=3
    };

    // Act
    bool ok = log.AppendFrom(2, entries);

    // Assert
    assert(!ok);
    assert(log.Size() == 1);   // 日志没变！

    std::cout << "[PASS] TestAppendFromNotContinuous\n";
}

void TestAppendFromGap() {
    // Arrange
    RaftLog log;
    log.AppendEntry(1, MakeCommand("a", "1"));

    // 现在 LastIndex=1，只能从 index=2 开始追加
    // startIndex=5 意味着中间有 gap
    std::vector<LogEntry> entries = { MakeEntry(2, 5) };

    // Act
    bool ok = log.AppendFrom(5, entries);

    // Assert
    assert(!ok);
    assert(log.Size() == 1);   // 日志没变！

    std::cout << "[PASS] TestAppendFromGap\n";
}

void TestAppendFromEmpty() {
    // Arrange
    RaftLog log;
    log.AppendEntry(1, MakeCommand("a", "1"));

    // Act
    bool ok = log.AppendFrom(2, {});

    // Assert
    assert(ok);                 // 空追加应该成功
    assert(log.Size() == 1);    // 日志没变

    std::cout << "[PASS] TestAppendFromEmpty\n";
}

// ==================== main ====================

int main() {
    std::cout << "===== RaftLog Tests =====\n";

    // 基础
    TestEmptyLog();
    TestAppendSingle();
    TestAppendMultiple();

    // 查询边界
    TestGetEntryOutOfRange();
    TestGetEntriesRange();

    // 截断
    TestTruncateFrom();
    TestTruncateFromOutOfRange();

    // AppendFrom
    TestAppendFromNormal();
    TestAppendFromConflict();
    TestAppendFromIndexMismatch();
    TestAppendFromNotContinuous();
    TestAppendFromGap();
    TestAppendFromEmpty();

    std::cout << "\nAll RaftLog tests passed!\n";
    return 0;
}