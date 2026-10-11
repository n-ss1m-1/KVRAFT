//raft_storage.h

#pragma once

#include<map>

#include "persist/persister.h"
#include "raft/raft_message.h"


/*
组合 序列化和反序列化方法(raft_message)+真正操作文件的Persister 
-> 避免在Persister中直接调用序列化和反序列化(这需要在persister中包含LogEntry等内容)
-> 从而避免下层的Persister依赖上层的raft
*/

class RaftStorage
{
public:
    RaftStorage(const std::string& dataDir);            //!这里使用const+引用(构造函数中不能修改+减少无效拷贝)

    //=========== 元状态 ===========
    //保存当前元状态(追加)
    bool AppendMeta(int64_t currentTerm,int32_t votedFor);
    //读取最后一条元状态 默认值{0,-1}
    std::pair<int64_t,int32_t> LoadMeta();

    //=========== 日志 ===========
    //追加一条日志(顺序追加)
    bool AppendLogEntry(const LogEntry& entry);
    //添加多条日志(覆盖追加)
    bool AppendLogEntriesFrom(int64_t startIndex,const std::vector<LogEntry>& entries);
    //读取所有日志
    std::vector<LogEntry> LoadLogEntries(int64_t snapshotIndex = 0);

    //=========== 快照 ===========
    //! TODO:可以添加版本号，表示不同格式的快照(应对未来添加字段)
    struct Snapshot
    {
        int64_t lastIncludedIndex = 0;
        int64_t lastIncludedTerm = 0;
        std::string data;
    };

    /*
     保存快照：先写到tmp 再原子替换(rename)
     快照格式：
        [8字节 lastIncludedIndex]
        [8字节 lastIncludedTerm]
        [8字节 dataLen(字节数)]
        [data ...]
     */
    bool SaveSnapshot(int64_t lastIncludedIndex,int64_t lastIncludedTerm,const std::string& data);

    // 加载快照：若返回nullopt则表示没有快照文件
    std::optional<Snapshot> LoadSnapshot();

    // 判断是否有快照
    bool HasSnapshot() const;

    /*
     生成快照后：物理日志的截断 
     截断范围: entry.index<=index的日志   保留: entry.index>index的日志
     ▲! 借助indexToOffset_找到需要保留的起始点 -> sendfile + newMap 减少冗余操作
    */
    bool TruncateLogUpTo(int64_t index);


private:
    //▲组合复用persister，避免修改
    //▲此处的Persister由RaftStorage私有，不需要外部访问->直接设置为值成员，而非shared_ptr
    Persister metaPersister_;
    Persister logPersister_;

    //保证追加到磁盘的日志不会重复(Load时map去重)
    std::map<int64_t,int64_t> indexToOffset_;       // index -> 该日志在文件中的起始字节偏移量 (相较unordered_map，map是index有序的，遍历友好)
    int64_t lastPersistedIndex_ = 0;                // 最后持久化的日志index

    std::string snapshotPath_;                  // 快照文件的路径
};



/*
RaftNode
    │ 调 SaveMeta(5, 1) / AppendLogEntry(entry)
    ▼
RaftStorage             ← 序列化成字符串，转发给 Persister
    │ 调 AppendToFile("META 5 1") / AppendToFile("1 5 PUT x 1")
    ▼
Persister               ← 真正操作文件（open/write/fsync）
    │
    ▼
磁盘文件

*/

/*
生成快照的完整调用链：
RaftNode::TakeSnapshot()
    ├── KVStore::Serialize()
    ├── RaftStorage::SaveSnapshot(...)      ← 快照文件
    ├── RaftLog::SetSnapshotIndexAndTerm()  ← 内存日志起点
    └── RaftStorage::TruncateLogUpTo(K)     ← 物理日志截断
            │
            ├── (全删) logPersister_.Truncate(0)
            │
            └── (部分) logPersister_.TruncateFront(startOffset)
                    │
                    ├── open src / open dst
                    ├── sendfile 拷贝 [startOffset, size_)
                    ├── fsync + close
                    ├── rename(dst, src)
                    ├── ReopenLocked()
                    └── size_ -= bytesToRemove
*/