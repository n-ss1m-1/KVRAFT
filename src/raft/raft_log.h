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
    bool AppendFrom(const int64_t startIndex,const std::vector<LogEntry>& entries);       //从startIndex(覆盖)追加

    //查询日志
    //已自动计算偏移 entries[i] -> index=i+1
    std::optional<LogEntry> GetEntry(const int64_t index) const;           
    //[start,end)：左开右闭              
    std::vector<LogEntry> GetEntries(const int64_t startIndex,const int64_t endIndex) const;    


    //获取lastLogIndex lastLogTerm | 不需要： prevLogIndex prevLogTerm ？由nextLogIndex数组中获取 -1即可
    int64_t LastIndex() const;
    int64_t LastTerm() const;


    //截断日志 [index,end]
    void TruncateFrom(const int64_t index);

    //一致性检查
    ConsistencyCheckResult CheckConsistency(const int64_t prevLogIndex,const int64_t prevLogTerm) const;


    //获取存储日志数
    int64_t Size() const;

private:
    std::vector<LogEntry> entries_;         //index to LogEntry
    //注：第一条日志索引=1  日志为空：LastIndex() = 0，LastTerm() = 0
};