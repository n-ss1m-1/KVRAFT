//raft_storage.cc
#include<iostream>
#include<fstream>

#include "raft/raft_storage.h"


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
    // 必须严格按照index顺序追加
    if(entry.index != lastPersistedIndex_+1)
    {
        std::cerr << "[RaftStorage] AppendLogEntry out of order: entry.index="
                  << entry.index << " expected=" << (lastPersistedIndex_ + 1)
                  << std::endl;
        return false;
    }

    // 序列化 记录该日志起始偏移
    const std::string line = SerializeLogEntry(entry);
    int64_t offset = fileSize_;

    // 持久化到磁盘
    if(!logPersister_.AppendToFile(line))
    {
        return false;
    }

    // 记录到偏移表 更新文件大小 更新最后持久化的日志index
    int64_t bytesWritten = static_cast<int64_t>(line.size())+1;     //AppendToFile内部加了'\n'，所以实际字节数需要+1
    indexToOffset_[entry.index] = offset;
    fileSize_ += bytesWritten;
    lastPersistedIndex_ = entry.index;
    return true;
}

bool RaftStorage::AppendLogEntriesFrom(int64_t startIndex,const std::vector<LogEntry>& entries)
{
    // lastPersistedIndex校验startIndex：不能存在gap   +   startIndex需要>=1
    if(startIndex <= 0 || startIndex > lastPersistedIndex_+1) return false;

    // startIndex校验entries的起点和连续性
    if(!entries.empty())                            //非空再校验
    {
        for(size_t i=0;i<entries.size();i++)
        {
            if(entries[i].index != startIndex+static_cast<int64_t>(i))
            {
                return false;
            }
        }
    }

    // 需要覆盖的场景：先截断                       //▲!不管entries是否为空 都需要判断是否需要截断
    if(startIndex <= lastPersistedIndex_)
    {
        //先找到startIndex对应的起始偏移
        auto it1 = indexToOffset_.find(startIndex);
        if(it1 == indexToOffset_.end())              //找不到
        {
            //! 找不到偏移（异常），保守起见不截断，直接追加
            //  这样会导致重复，但加载时 map 去重能兜底
            std::cerr << "[RaftStorage] cannot find offset for index="
                      << startIndex << ", skip truncate" << std::endl;
        }
        else            //找到了
        {
            // Truncate
            int64_t truncateOffset = it1->second;
            if(!logPersister_.Truncate(truncateOffset))
            {
                return false;
            }
            // 更新：fileSize_  IndexToOffset_  lastPersistedIndex_
            fileSize_ = truncateOffset;
            auto it2 = indexToOffset_.lower_bound(startIndex);      //找到 key>=startIndex 的起点
            indexToOffset_.erase(it2,indexToOffset_.end());
            lastPersistedIndex_ = startIndex - 1;
        }

        //更新：fileSize_  IndexToOffset_  lastPersistedIndex_
    }

    //追加新内容
    for(const auto& entry : entries)
    {
        if(!AppendLogEntry(entry))
        {
            return false;
        }
    }

    return true;
}

std::vector<LogEntry> RaftStorage::LoadLogEntries()
{   
    // 1. 清空所有状态(防御 Recover 被多次调用)
    indexToOffset_.clear();
    fileSize_ = 0;
    lastPersistedIndex_ = 0;

    // 2. 逐行扫描：index->{offset,entry}  
    std::ifstream in(logPersister_.GetFileName());
    if(!in.is_open()) return {};                //首次启动，无日志文件

    std::map<int64_t,std::pair<int64_t,LogEntry>> byIndex;
    std::string line;
    int64_t offset = 0;
    while(std::getline(in,line))
    {
        int64_t lineOffset = offset;        // 该日志在文件中的起始字节偏移量
        offset += static_cast<int64_t>(line.size()) + 1;  // + 1 for '\n'

        if(line.empty()) continue;

        auto entryOpt = DeserializeLogEntry(line);
        if(!entryOpt.has_value())
        {
            std::cerr << "[RaftStorage] parse failed at offset "
                      << lineOffset << ": " << line << std::endl;
            continue;   // 跳过无法解析的行
        }

        byIndex[entryOpt.value().index] = {lineOffset,entryOpt.value()};    //遇到重复的index->后者覆盖前者
    }
    fileSize_ = offset;

    // 3. 取出entries，检查连续性，遇到gap就停止
    std::vector<LogEntry> entries;
    int64_t expectedIndex = 1;
    for(auto& [index,pair] : byIndex)
    {
        if(index != expectedIndex)
        {
            std::cerr << "[RaftStorage] log gap: expected index="
                << expectedIndex << " got=" << index
                << ", truncating at " << expectedIndex << std::endl;
            break;
        }
        indexToOffset_[index] = pair.first;
        entries.push_back(std::move(pair.second));
        lastPersistedIndex_ = index;
        expectedIndex++;
    }

    return entries;
}
