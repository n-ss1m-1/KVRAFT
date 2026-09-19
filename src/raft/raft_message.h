//raft_message.h

#pragma once


#include<cstdint>
#include<vector>
#include<string>

#include "../common/command.h"



struct LogEntry
{
    int64_t index = 0;              // 日志索引
    int64_t term = 0;               // 产生该日志时的leader任期
    Command command;
    LogEntry()=default;
    LogEntry(int64_t index,int64_t term,const Command& command):index(index),term(term),command(command){}
};


//选举投票
struct RequestVoteArgs
{
    int64_t term = 0;               // 候选者 当前的任期（接收者据此判断是否更新自己的 term）
    int32_t candidateId = 0;        
    int64_t lastLogIndex = 0;       // 候选者 最后一条日志的 index（选举次要比较）
    int64_t lastLogTerm = 0;        // 候选者 最后一条日志的 term（选举优先比较）
};

struct RequestVoteReply
{
    int64_t term = 0;               // 接收者 当前的任期（候选者据此判断自己是否过时、要退位）
    bool voteGranted = false;
};


//日志复制 / 心跳
struct AppendEntriesArgs
{
    int64_t term = 0;               // Leader 当前任期（Follower 据此判断是否跟随）
    int32_t leaderId = 0;           // Leader ID（客户端重定向、确认 Leader 身份）
    int64_t prevLogIndex = 0;       // ▲ Leader发给Follower的这批日志的 前一条日志的 index（一致性检查）
    int64_t prevLogTerm = 0;        // ▲ Leader发给Follower的这批日志的 前一条日志的 term（一致性检查）
    std::vector<LogEntry> entries;  // 要追加的日志（心跳时为空）
    int64_t leaderCommit = 0;       // Leader 已提交到的 index（Follower 据此更新自己的 commitIndex）
};

struct AppendEntriesReply
{
    int64_t term = 0;               // Follower 当前任期（Leader 据此判断是否退位）
    bool success = false;
    int64_t matchIndex = 0;         // success == true 时有效 ：日志匹配的终点
    int64_t conflictIndex = 0;      // success == false 时有效：日志冲突的起点（加速 Leader 回退）
};









