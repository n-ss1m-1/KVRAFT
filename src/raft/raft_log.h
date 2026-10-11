//raft_log.h

#pragma once

#include<optional>

#include "raft/raft_message.h"

struct ConsistencyCheckResult
{
    bool ok = false;
    int64_t conflictIndex = 0;
};

class RaftLog
{
public:
    RaftLog() = default;

    //追加日志
    void AppendEntry(const int64_t term,const Command& command);                          //追加单项 !不需要传入index——由raft自己计算，index——LastIndex() + 1，避免出错
    void AppendEntry(const LogEntry& entry); 
    bool AppendFrom(int64_t startIndex,const std::vector<LogEntry>& entries);       //从startIndex(覆盖)追加

    //查询日志
    //已自动计算偏移 entries[i] -> index = snapshotIndex_ + i + 1
    std::optional<LogEntry> GetEntry(const int64_t index) const;           
    //[start,end)：左开右闭              
    std::vector<LogEntry> GetEntries(const int64_t startIndex,const int64_t endIndex) const;    

    int64_t GetTerm(int64_t index) const;

    
    //获取lastLogIndex lastLogTerm | 不需要： prevLogIndex prevLogTerm ？由nextLogIndex数组中获取 -1即可
    int64_t FirstIndex() const;
    int64_t LastIndex() const;
    int64_t LastTerm() const;


    //截断日志 [index,end]
    bool TruncateFrom(const int64_t index);

    void SetSnapshotIndexAndTerm(int64_t index,int64_t term);

    //一致性检查
    ConsistencyCheckResult CheckConsistency(const int64_t prevLogIndex,const int64_t prevLogTerm) const;


    //获取内存中的日志条数(不含快照覆盖部分)
    int64_t Size() const;

    int64_t SnapshotIndex() const;
    int64_t SnapshotTerm() const;

private:
    size_t IndexToArrayPos(int64_t index) const;

    std::vector<LogEntry> entries_;                     //index to LogEntry
    // ▲前置条件：FirstIndex() <= index <= LastIndex()
    // ▲日志数组下标 i = index - snapshotIndex_ - 1

    //快照 最后一条日志 的index和term
    int64_t snapshotIndex_ = 0;
    int64_t snapshotTerm_ = 0;
};
/*
新增快照 修改方法：
AppendFrom()
GetEntry()
GetEntries()
LastIndex()
LastTerm()
TruncateFrom()
CheckConsistency()

新增方法：
FirstIndex(): 获取entries[0]的index
SetSnapshotIndexAndTerm(): 生成快照后，修改相应的index和term
*/