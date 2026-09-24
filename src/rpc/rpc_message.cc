//rpc_message.cc

#include<sstream>

#include "rpc/rpc_message.h"

std::string SerializeRequestVoteArgs(uint64_t reqId,const RequestVoteArgs& args)
{
    std::ostringstream oss;
    oss <<static_cast<uint32_t>(RpcMessageType::kRequestVote)<<" "
        <<reqId<<" "                //!勿忘reqId
        <<args.term<<" "
        <<args.candidateId<<" "
        <<args.lastLogIndex<<" "
        <<args.lastLogTerm;       
    
    return oss.str();
}

std::string SerializeRequestVoteReply(uint64_t reqId,const RequestVoteReply& reply)
{
    std::ostringstream oss;
    oss <<static_cast<uint32_t>(RpcMessageType::kRequestVoteReply)<<" "
        <<reqId<<" "
        <<reply.term<<" "
        <<reply.voteGranted;
    
    return oss.str();
}

std::string SerializeAppendEntriesArgs(uint64_t reqId,const AppendEntriesArgs& args)
{
    std::ostringstream oss;
    oss <<static_cast<uint32_t>(RpcMessageType::kAppendEntries)<<" "
        <<reqId<<" "
        <<args.term<<" "
        <<args.leaderId<<" "
        <<args.prevLogIndex<<" "
        <<args.prevLogTerm<<" "
        <<args.leaderCommit<<" "
        <<args.entries.size();            //!先标注entries的个数      
    
    for(const auto& entry : args.entries)
    {
        oss <<"\n"<<SerializeLogEntry(entry);   //!之后每行一个entry：  第一条 entry 前加 \n，后续每条也如此，这样就不会有"末尾 \n"问题
    }
        
    
    return oss.str();
}

std::string SerializeAppendEntriesReply(uint64_t reqId,const AppendEntriesReply& reply)
{
    std::ostringstream oss;
    oss <<static_cast<uint32_t>(RpcMessageType::kAppendEntriesReply)<<" "
        <<reqId<<" "
        <<reply.term<<" "
        <<reply.success<<" "
        <<reply.matchIndex<<" "
        <<reply.conflictIndex;
    
    return oss.str();
}


std::optional<RequestVoteArgs> DeserializeRequestVoteArgs(std::istringstream& iss)
{
    RequestVoteArgs args;
    if(!(iss>>args.term>>args.candidateId>>args.lastLogIndex>>args.lastLogTerm)) return std::nullopt;

    return args;
}

std::optional<RequestVoteReply> DeserializeRequestVoteReply(std::istringstream& iss)
{
    RequestVoteReply reply;
    if(!(iss>>reply.term>>reply.voteGranted)) return std::nullopt;
    
    return reply;
}

std::optional<AppendEntriesArgs> DeserializeAppendEntriesArgs(std::istringstream& iss)
{
    AppendEntriesArgs args;
    size_t count;
    if(!(iss>>args.term>>args.leaderId>>args.prevLogIndex>>args.prevLogTerm>>args.leaderCommit>>count)) return std::nullopt;
    iss >> std::ws;                 //! 跳过所有空白字符（含换行）                  

    for(size_t i=0;i<count;i++)
    {
        std::string line;
        std::getline(iss,line);                         //!▲ 区分<<和getline：  <<不会自动丢弃末尾的换行符   getline相反，会自动丢弃末尾的换行符
        auto entry = DeserializeLogEntry(line);
        if(!entry.has_value()) return std::nullopt;
        args.entries.emplace_back(entry.value());       //!插入实际类型 而非optional
    }

    return args;
}

std::optional<AppendEntriesReply> DeserializeAppendEntriesReply(std::istringstream& iss)
{
    AppendEntriesReply reply;
    if(!(iss>>reply.term>>reply.success>>reply.matchIndex>>reply.conflictIndex)) return std::nullopt;
    
    return reply;
}
