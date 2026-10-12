#include<sstream>
#include<cstring>       //std::memcpy()

#include "raft/raft_message.h"


// LogEntry序列化和反序列化   
// <index> <term> <操作> <key> [value]
std::string SerializeLogEntry(const LogEntry& entry)
{
    std::ostringstream oss;
    oss <<entry.index<<" "
        <<entry.term<<" "
        <<CommandTypeToString(entry.command.type)<<" "
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

//Snapshot序列化和反序列化
std::string SerializeSnapshot(const Snapshot& snap)
{
    constexpr size_t kHeaderSize = sizeof(int64_t) * 3;
    const int64_t dataLen = static_cast<int64_t>(snap.data.size());

    std::string buf;
    buf.resize(kHeaderSize);
    std::memcpy(buf.data(),         &snap.lastIncludedIndex, sizeof(snap.lastIncludedIndex));
    std::memcpy(buf.data() + 8,     &snap.lastIncludedTerm, sizeof(snap.lastIncludedTerm));
    std::memcpy(buf.data() + 16,    &dataLen, sizeof(dataLen));
    buf.append(snap.data);

    return buf;
}

std::optional<Snapshot> DeserializeSnapshot(const std::string& raw)
{
    constexpr size_t kHeaderSize = sizeof(int64_t) * 3;
    constexpr int64_t kMaxSnapshotSize = 1LL << 30;

    if(raw.size() < kHeaderSize) return std::nullopt;

    Snapshot snap;
    int64_t dataLen = 0;
    std::memcpy(&snap.lastIncludedIndex, raw.data(),      sizeof(snap.lastIncludedIndex));
    std::memcpy(&snap.lastIncludedTerm,  raw.data() + 8,  sizeof(snap.lastIncludedTerm));
    std::memcpy(&dataLen,                raw.data() + 16, sizeof(dataLen));

    if(dataLen < 0 || dataLen > kMaxSnapshotSize) return std::nullopt;                  // dataLen: 为负 or 太大
    if(static_cast<size_t>(dataLen) != raw.size()-kHeaderSize) return std::nullopt;     // dataLen与实际的data的长度不符

    snap.data.assign(raw.data() + kHeaderSize, static_cast<size_t>(dataLen));

    return snap;
}