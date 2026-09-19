//raft_transport.h

#pragma once
#include "raft_message.h"

class RaftTransport
{
public:
    virtual ~RaftTransport()=default;
    
    virtual bool SendRequestVote(int32_t peerId,const RequestVoteArgs& args,RequestVoteReply& reply) = 0;
    virtual bool SendAppendEntries(int32_t peerId,const AppendEntriesArgs& args,AppendEntriesReply& reply) = 0;

};