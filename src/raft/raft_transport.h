//raft_transport.h

#pragma once

#include<functional>

#include "raft_message.h"

class RaftTransport
{
public:
    using RequestVoteCallback = std::function<void(const RequestVoteReply&)>;
    using AppendEntriesCallback = std::function<void(const AppendEntriesReply&)>;


    virtual ~RaftTransport()=default;
    
    virtual void SendRequestVote(int32_t peerId,const RequestVoteArgs& args,RequestVoteCallback cb) = 0;             //const &或者 值传递 才能接收lambda表达式
    virtual void SendAppendEntries(int32_t peerId,const AppendEntriesArgs& args,AppendEntriesCallback cb) = 0;      //值传递：使用move零拷贝

};