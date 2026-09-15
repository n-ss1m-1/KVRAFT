//raft_storage.h

#pragma once

#include "../persist/persister.h"
#include "raft_message_codec.h"


/*
组合 序列化和反序列化方法(raft_message_codec)+真正操作文件的Persister 
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
    //追加一条日志
    bool AppendLogEntry(const LogEntry& entry);
    //读取所有日志
    std::vector<LogEntry> LoadLogEntries();


private:
    //▲组合复用persister，避免修改
    //▲此处的Persister由RaftStorage私有，不需要外部访问->直接设置为值成员，而非shared_ptr
    Persister metaPersister_;
    Persister logPersister_;
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