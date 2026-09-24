#include<sstream>

#include "raft/raft_message.h"


// LogEntry序列化和反序列化   
// <index> <term> <操作> <key> [value]
std::string SerializeLogEntry(const LogEntry& entry)
{
    std::ostringstream oss;
    oss <<entry.index<<" "
        <<entry.term<<" "
        <<CommandTypeToString(entry.command)<<" "
        <<entry.command.key;
    if(entry.command.type==Command::Type::PUT) oss<<" "<<entry.command.value;       //!空格在此处输入 不能在key后输入 避免没有value时多一个空格
    
    return oss.str();
}

std::optional<LogEntry> DeserializeLogEntry(const std::string& line)
{
    std::istringstream iss(line);
    LogEntry entry;
    std::string typeStr;
    
    if(!(iss>>entry.index>>entry.term>>typeStr>>entry.command.key)) return std::nullopt;

    auto type=StringToCommandType(typeStr);
    if(!type.has_value()) return std::nullopt;
    else                  entry.command.type=type.value();

    if(type.value()==Command::Type::PUT) 
    {
        if(!(iss>>entry.command.value)) return std::nullopt;
    }

    return entry;
}


// Raft元状态序列化和反序列化 元状态：当前所在任期、投票给谁  
// META <currentTerm> <votedFor>
std::string SerializeMeta(int64_t currentTerm,int32_t votedFor)
{
    std::ostringstream oss;
    oss <<"META "
        <<currentTerm<<" "
        <<votedFor;

    return oss.str();
}

std::optional<std::pair<int64_t,int32_t>> DeserializeMeta(const std::string& line)
{
    std::istringstream iss(line);
    std::string tag;
    int64_t currentTerm;
    int32_t votedFor;

    if(!(iss>>tag>>currentTerm>>votedFor)) return std::nullopt;
    if(tag!="META") return std::nullopt;

    return std::make_pair(currentTerm,votedFor);                //使用方法(不需要指定模板)! std::make_pair(v1,v2); 
}
