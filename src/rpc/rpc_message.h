//rpc_message.h

#pragma once
#include "raft/raft_message.h"
#include "kv/kv_store.h"
#include "raft/raft_storage.h"
#include "raft/raft_transport.h"


enum class RpcMessageType : uint32_t        //!? 显式写值——避免将来插入新枚举时旧值变化
{
    kRequestVote            = 1,
    kRequestVoteReply       = 2,
    kAppendEntries          = 3,
    kAppendEntriesReply     = 4,
    kUnknow                 = 0
};
//解析消息类型时：使用static_cast进行类型转换 + default处理未知消息类型


//序列化：<消息类型> <reqId> <args/reply>
std::string SerializeRequestVoteArgs(uint64_t reqId,const RequestVoteArgs& args);
std::string SerializeRequestVoteReply(uint64_t reqId,const RequestVoteReply& reply);
std::string SerializeAppendEntriesArgs(uint64_t reqId,const AppendEntriesArgs& args);
std::string SerializeAppendEntriesReply(uint64_t reqId,const AppendEntriesReply& reply);

//反序列化： <args/reply>       <消息类型> <reqId>在外层进行解析
std::optional<RequestVoteArgs> DeserializeRequestVoteArgs(std::istringstream& iss);
std::optional<RequestVoteReply> DeserializeRequestVoteReply(std::istringstream& iss);
std::optional<AppendEntriesArgs> DeserializeAppendEntriesArgs(std::istringstream& iss);
std::optional<AppendEntriesReply> DeserializeAppendEntriesReply(std::istringstream& iss);