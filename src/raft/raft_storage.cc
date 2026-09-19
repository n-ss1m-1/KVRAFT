//raft_storage.cc

#include "raft_storage.h"


RaftStorage::RaftStorage(const std::string& dataDir):          //dataDir 由 main 创建并保证存在
        metaPersister_(dataDir+"/meta.txt"),
        logPersister_(dataDir+"/raft.log")
{
}

bool RaftStorage::AppendMeta(int64_t currentTerm,int32_t votedFor)
{
    return metaPersister_.AppendToFile(SerializeMeta(currentTerm,votedFor));
}

std::pair<int64_t,int32_t> RaftStorage::LoadMeta()
{
    std::vector<std::string> lines = metaPersister_.LoadFromFile();
    if(lines.empty()) return {0,-1};                // 元状态为空，返回默认值  

    // 取最后一行元状态，并将string转换为pair
    auto result = DeserializeMeta(lines.back());
    if(!result.has_value()) return {0,-1};          //数据无效，返回默认值

    return result.value();
}

bool RaftStorage::AppendLogEntry(const LogEntry& entry)
{
    return logPersister_.AppendToFile(SerializeLogEntry(entry));
}

bool RaftStorage::AppendLogEntries(const std::vector<LogEntry>& entries)
{
    for(const LogEntry& entry : entries)
    {
        if(!AppendLogEntry(entry)) return false;
    }
    return true;
}

std::vector<LogEntry> RaftStorage::LoadLogEntries()
{   
    std::vector<LogEntry> entries;
    std::vector<std::string> lines = logPersister_.LoadFromFile();

    for(const auto& line : lines)
    {           
        auto entry = DeserializeLogEntry(line);
        if(!entry.has_value()) continue;            //循环里不需要判断line.empty()，只要解析时参数对不上->返回nullopt->此处也会continue
        entries.emplace_back(entry.value());        //!这里需要使用.value()取值 否则错误
    }

    return entries;
}
