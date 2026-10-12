//raft_storage.cc
#include<fstream>
#include<filesystem>

#include<spdlog/spdlog.h>

#include "raft/raft_storage.h"

/* 
 仅当前.cc文件可见，不污染头文件
 不属于类，语义纯粹：只是IO工具函数

 ! 为什么是 void* ：可以接收任意类型的指针，无论是int、string、struct

 ! 为什么需要实现循环写：ifstream::read()内部已实现循环读，不需要再实现
*/
static bool WriteAll(int fd, const void* buf, size_t len)   
{
    const char* p = static_cast<const char*>(buf);          // !转为 char*类型 是为了做指针运算
    size_t written = 0;

    while(written < len)
    {
        ssize_t n = write(fd, p+written, len-written);              //! 需要使用ssize_t：若使用size_t(无符号整型) 收到-1时会变为最大值 -> 无法走到错误处理
        if(n < 0)
        {
            if(errno == EINTR) continue;
            spdlog::error("[RaftStorage] WriteAll: write failed, errno={}: {}",errno,strerror(errno));
            return false;
        }
        written += n;
    }

    return true;
}



RaftStorage::RaftStorage(const std::string& dataDir):          //dataDir 由 main 创建并保证存在
        metaPersister_(dataDir+"/meta.txt"),
        logPersister_(dataDir+"/raft.log"),
        snapPersister_(dataDir+"/snapshot.bin", /*needCreate=*/false)          // 添加快照文件交给Persister管理
{
}

bool RaftStorage::AppendMeta(int64_t currentTerm,int32_t votedFor)
{
    return metaPersister_.AppendToFile(SerializeMeta(currentTerm,votedFor)).has_value();
}

std::pair<int64_t,int32_t> RaftStorage::LoadMeta()
{
    std::vector<std::string> lines = metaPersister_.LoadFromFile();
    if(lines.empty()) return {0,-1};                // 元状态为空，返回默认值  

    // 取最后一行元状态，并将string转换为pair           TODO: 处理空行和错误行
    auto result = DeserializeMeta(lines.back());
    if(!result.has_value()) return {0,-1};          //数据无效，返回默认值

    return result.value();
}

bool RaftStorage::AppendLogEntry(const LogEntry& entry)
{
    // 必须严格按照index顺序追加
    if(entry.index != lastPersistedIndex_+1)
    {
        spdlog::error("[Storage] out-of-order append: index={}, expected={}",entry.index, lastPersistedIndex_+1);
        return false;
    }

    // 序列化 
    const std::string line = SerializeLogEntry(entry);

    // 持久化到磁盘 ! 此处返回该日志起始偏移
    auto offsetOpt = logPersister_.AppendToFile(line);
    if(!offsetOpt.has_value())
    {
        spdlog::error("[Storage] AppendLogEntry: AppendToFile failed: index={}", entry.index);
        return false;
    }

    // 记录到偏移表 更新文件大小 更新最后持久化的日志index
    int64_t offset = offsetOpt.value();
    indexToOffset_[entry.index] = offset;
    lastPersistedIndex_ = entry.index;

    spdlog::trace("[Storage] AppendLogEntry: persisted entry index={}, term={}, offset={}",entry.index,entry.term,offset);

    return true;
}

bool RaftStorage::AppendLogEntriesFrom(int64_t startIndex,const std::vector<LogEntry>& entries)
{
    // lastPersistedIndex校验startIndex：不能存在gap   +   startIndex需要>=lastPersistedIndex_+1
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
            //! 找不到起点偏移 -> 说明磁盘索引已损坏 -> 不能跳过截断去追加（会把垃圾永久夹在文件中间）
            spdlog::error("[RaftStorage] AppendLogEntriesFrom: cannot find offset for index={}, abort", startIndex);
            return false;
        }
        else            //找到了
        {
            // Truncate
            int64_t truncateOffset = it1->second;
            spdlog::info("[Storage] AppendLogEntriesFrom: truncating log from index={}, offset={}, entries.size={}", startIndex, truncateOffset,entries.size());
            if(!logPersister_.Truncate(truncateOffset))
            {
                return false;
            }
            // 更新: IndexToOffset_  lastPersistedIndex_
            auto it2 = indexToOffset_.lower_bound(startIndex);      //找到 key>=startIndex 的起点
            indexToOffset_.erase(it2,indexToOffset_.end());
            lastPersistedIndex_ = startIndex - 1;
        }
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

std::vector<LogEntry> RaftStorage::LoadLogEntries(int64_t snapshotIndex)
{   
    // 1. 清空所有状态(防御 Recover 被多次调用)
    indexToOffset_.clear();
    lastPersistedIndex_ = snapshotIndex;                        // TODO: 此处可以由RaftNode::Recover()传进来，同时更改下方的expectedIndex
                                                                // ok !▲此处其实是一个bug，因为当磁盘日志文件为空时，无法正确初始化lastPersistedIndex_
    // 2. 逐行扫描：index->{offset,entry}  
    std::map<int64_t,std::pair<int64_t,LogEntry>> byIndex;          //!▲ 使用map去重
    int64_t offset = 0;
    for(const auto& line : logPersister_.LoadFromFile())
    {
        int64_t lineOffset = offset;        // 该日志在文件中的起始字节偏移量
        offset += static_cast<int64_t>(line.size()) + 1;  // + 1 for '\n'

        if(line.empty()) continue;

        auto entryOpt = DeserializeLogEntry(line);
        if(!entryOpt.has_value())
        {
            spdlog::error("[RaftStorage] LoadLogEntries: parse failed at offset {}: {}", lineOffset, line);
            continue;   // 跳过无法解析的行
        }

        byIndex[entryOpt.value().index] = {lineOffset,entryOpt.value()};    //遇到重复的index->后者覆盖前者
    }

    // 3. 取出entries，检查连续性，遇到gap就停止 (防御)
    std::vector<LogEntry> entries;
    bool gapDetected = false;
    int64_t expectedIndex = lastPersistedIndex_ + 1;
    for(auto& [index,pair] : byIndex)
    {
        // 增加快照功能后，首个index不再是1
        if(index != expectedIndex)
        {
            spdlog::error("[RaftStorage] LoadLogEntries: log gap, expected index={} got={}, truncating at {}",
                expectedIndex, index, expectedIndex);
            gapDetected = true;
            break;
        }

        indexToOffset_[index] = pair.first;
        entries.push_back(std::move(pair.second));
        lastPersistedIndex_ = index;
        expectedIndex++;
    }

    // 4. (防御) 发现gap：物理文件中 gap 之后的垃圾必须清掉，否则后续追加后每次重启都加载不到尾部
    //           解决办法：用已校验的连续前缀重建干净的RaftStorage，Leader后续会把缺失部分补齐
    if(gapDetected)
    {
        spdlog::warn("[RaftStorage] LoadLogEntries: rebuilding raft.log with {} contiguous prefix entries",
            entries.size());

        if(logPersister_.Truncate(0))
        {
            indexToOffset_.clear();
            lastPersistedIndex_ = 0;
            for(const auto& e : entries)
            {
                if(!AppendLogEntry(e))      // 内部会重建 indexToOffset_ 和 lastPersistedIndex_
                {
                    spdlog::error("[RaftStorage] LoadLogEntries: rebuild failed at index={}", e.index);
                    break;
                }
            }
        }
        else
        {
            spdlog::error("[RaftStorage] LoadLogEntries: cannot truncate corrupted raft.log");
        }
    }

    return entries;
}

bool RaftStorage::SaveSnapshot(const Snapshot& snap)
{
    spdlog::info("[RaftStorage] SaveSnapshot: index={}, term={}, size={}",
        snap.lastIncludedIndex, snap.lastIncludedTerm, snap.data.size());

    if(!snapPersister_.OverwriteFile(SerializeSnapshot(snap)))
    {
        spdlog::error("[RaftStorage] SaveSnapshot: OverwriteFile failed: {}", snapPersister_.GetFileName());
        return false;
    }

    return true;
}

std::optional<Snapshot> RaftStorage::LoadSnapshot()
{
    if(!snapPersister_.Exists())
    {
        spdlog::error("[RaftStorage] LoadSnapshot: snapshot didn't exist");
        return std::nullopt;
    }

    auto rawOpt = snapPersister_.ReadwholeFile();
    if(!rawOpt.has_value())
    {
        spdlog::error("[RaftStorage] LoadSnapshot: ReadwholeFile failed");
        return std::nullopt;
    }

    auto snapOpt = DeserializeSnapshot(rawOpt.value());
    if(!snapOpt.has_value())
    {
        spdlog::error("[RaftStorage] LoadSnapshot: DeserializeSnapshot failed");
        return std::nullopt;
    }

    return snapOpt.value();
}

bool RaftStorage::HasSnapshot() const
{
    return snapPersister_.Exists();
}

bool RaftStorage::TruncateLogUpTo(int64_t index)
{
    // 1.1 无需截断
    if(index <= 0) return true;

    // 1.2 全截断
    if(index >= lastPersistedIndex_)
    {
        spdlog::info("[Storage] TruncateLogUpTo({}): clear all (lastIndex={})",index, lastPersistedIndex_);
        
        if(!logPersister_.Truncate(0))
        {
            spdlog::error("[Storage] clear log failed");
            return false;
        }

        indexToOffset_.clear();
        lastPersistedIndex_ = index;        //!▲ bug  从未append,直接快照复制 -> index > lastPersistedIndex_ -> 此时需要修改
        return true;
    }

    // 1.3 部分截断: 找到 index+1 的起始偏移(从这开始需要保留)
    auto it = indexToOffset_.find(index+1);             //! 注意：index+1
    if(it == indexToOffset_.end())          
    {
        //错误: 找不到index
        spdlog::error("[Storage] TruncateLogUpTo({}): index+1 not found in map, lastPersistedIndex={}",index,lastPersistedIndex_);
        return false;
    }
    int64_t startOffset = it->second;

    spdlog::info("[Storage] TruncateLogUpTo({}): removing first {} bytes (kept {})", index, startOffset, lastPersistedIndex_ - index);

    //交给 Persister 做头部截断(内部处理: 锁、TruncateLocked / sendfile、rename、reopen)
    if(!logPersister_.TruncateFront(startOffset))
    {
        spdlog::error("[Storage] TruncateFront failed");
        return false;
    }

    // 2. 重建map: 使用newMap，保留 index > K 的部分，偏移全部减去startOffset
    std::map<int64_t,int64_t> newMap;
    for(const auto& [idx,off] : indexToOffset_)
    {
        if(idx > index)
        {
            newMap[idx] = off - startOffset;
        }
    }
    indexToOffset_ = std::move(newMap);

    return true;
}

