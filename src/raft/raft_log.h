//raft_log.h

#pragma once

#include<optional>

#include "raft_message.h"


class RaftLog
{
public:
    //追加日志
    void AppendEntry(int64_t term,const Command& command);                          //追加单项 !不需要传入index——由raft自己计算，index——LastIndex() + 1，避免出错
    bool AppendFrom(int64_t startIndex,const std::vector<LogEntry>& entries);       //从startIndex(覆盖)追加

    //查询日志
    std::optional<LogEntry> GetEntry(int64_t index) const;
    std::vector<LogEntry> GetEntries(int64_t startIndex,int64_t endIndex) const;    //[start,end)


    //获取lastLogIndex lastLogTerm | 不需要： prevLogIndex prevLogTerm ？
    int64_t LastIndex() const;
    int64_t LastTerm() const;


    //截断日志 [index,end]
    void TruncateFrom(int64_t index);


    //获取存储日志数
    int64_t Size() const;

private:
    std::vector<LogEntry> entries_;         //index to LogEntry
    //注：第一条日志索引=1  日志为空：LastIndex() = 0，LastTerm() = 0
};