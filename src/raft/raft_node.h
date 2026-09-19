//raft_node.h
#pragma once

#include<memory>
#include<condition_variable>
#include<chrono>
#include<set>

#include "raft_log.h"
#include "raft_storage.h"
#include "../kv/kv_store.h"
#include "raft_transport.h"
#include "../common/command.h"
#include "../common/types.h"


static constexpr int kHeartbeatIntervalMs_ = 50;         // 每次心跳间隔时间             static:静态类成员(所有对象共享一份值) constexpr：编译器确定值 运行时不能修改

class RaftNode {
public:
    RaftNode(int32_t nodeId,
             int32_t totalNodes,
             std::shared_ptr<RaftTransport> transport,
             std::shared_ptr<RaftStorage> storage,
             std::shared_ptr<KVStore> stateMachine);

    // ==================== 客户端接口 ====================

    // 提交一条命令，阻塞等待提交完成
    // 返回 true: 已提交并应用；false: 不是 Leader 或超时
    bool Start(const Command& command);

    // ==================== RPC 接收（网络层调用）====================

    // 处理收到的投票请求，填 reply
    void HandleRequestVote(const RequestVoteArgs& args, RequestVoteReply& reply);

    // 处理收到的日志复制/心跳，填 reply
    void HandleAppendEntries(const AppendEntriesArgs& args, AppendEntriesReply& reply);

    // ==================== RPC 回复处理 ====================

    // 处理投票回复：累加票数，达到大多数则 BecomeLeader
    void HandleRequestVoteReply(int32_t peerId, const RequestVoteReply& reply);

    // 处理日志复制回复：更新 matchIndex/nextIndex，成功则 UpdateCommitIndex
    void HandleAppendEntriesReply(int32_t peerId, const AppendEntriesReply& reply);

    // ==================== 定时器 ====================

    // 由定时器线程周期调用
    // - Follower/Candidate: 检查选举超时 -> StartElection
    // - Leader: 检查心跳超时 -> SendHeartbeat
    void Tick();

    
    // 发起选举：term++、投自己、向所有 peer 发 RequestVote
    // 
    // 谁调用：
    //   - Tick()：选举超时后自动触发
    //   - 测试：手动触发
    // 
    // 调用者必须不持有 mutex_（函数内部会加锁）
    void StartElection();       

    // !向所有 Follower 发送 AppendEntries：
    // - Follower 已同步：entries 为空（纯心跳）
    // - Follower 落后：entries 带上缺失的日志
    // 
    // 谁调用：
    //   - Tick()：Leader 心跳超时后自动触发
    //   - BecomeLeader 后：立即广播（锁外调用）
    //   - Start() 追加日志后：主动推送，不等 Tick
    //   - 测试：手动触发 Follower 提交
    // 
    // 调用者必须不持有 mutex_（函数内部会加锁）
    void SendHeartbeat();       

    // ==================== 查询：加锁（供 ClientServer / 测试使用）====================

    bool IsLeader() const;
    bool IsCandidate() const;
    Role GetRole() const;
    int64_t GetCurrentTerm() const;
    int32_t GetLeaderId() const;


private:
    // ==================== 内部辅助：不加锁，调用者持有锁 ====================
    void BecomeFollower(int64_t newTerm);   // 通常伴随 term 更新
    void BecomeCandidate();                 // term++、投票给自己、发起选举
    void BecomeLeader();                    // 初始化 nextIndex_/matchIndex_、发心跳
    
    void UpdateCommitIndex();   // Leader 用：找大多数 matchIndex，更新 commitIndex_
    void ApplyCommittedEntries();   // 将 (lastApplied_, commitIndex_] 应用到状态机
    bool PersistMeta();         // 持久化 currentTerm_ 和 votedFor_
    bool Recover();             // 启动时：从 storage_ 恢复 meta 和日志(全生命周期仅一次)


    // ==================== RPC 发送（Leader 主动调用）（锁外调用）====================

    // 向 peerId 发送 RequestVote（内部构造 args，调 transport_）
    bool SendRequestVote(int32_t peerId,const RequestVoteArgs& args);

    // 向 peerId 发送 AppendEntries（内部构造 args，调 transport_）
    // entries 为空时就是心跳
    bool SendAppendEntries(int32_t peerId,const AppendEntriesArgs& args);



    // ==================== 静态配置 ====================
    int32_t nodeId_;                        // 本节点 ID
    int32_t totalNodes_;                     // 节点总数目(! 包含自己) (! 规定nodeId_即为nextIndex_和matchIndex_的对应下标)

    // ==================== 持久化状态（重启后必须保留）====================
    int64_t currentTerm_ = 0;               // 当前任期
    int32_t votedFor_ = -1;                 // 本任期投给了谁，-1 表示未投

    RaftLog log_;                           // 日志条目（内存 + 已通过 RaftStorage 落盘）

    // ==================== 易失状态（重启后重建）====================
    Role role_ = Role::Follower;            // 当前角色
    std::set<int32_t> votesReceived_;        // 统计被投票数
    int64_t commitIndex_ = 0;               // 已提交日志的最大 index（含） (▲大多数已确认收到的日志->由Leader统计并通告所有follower提交)
    int64_t lastApplied_ = 0;               // 状态机已应用到的最大 index
    int32_t leaderId_ = -1;                 // 当前 Leader ID，-1 表示未知

    // ==================== Leader 专有 ====================
    // !规定nodeId_即为nextIndex_和matchIndex_的对应下标
    std::vector<int64_t> nextIndex_;        // [i] -> 下一条要发给 peer i 的日志 index
    std::vector<int64_t> matchIndex_;       // [i] -> peer i 已确认收到的最大 index (▲收到!=提交  收到=存入内存日志+磁盘)

    // ==================== 选举超时 ====================
    std::chrono::steady_clock::time_point electionResetTime_;   // follower最后一次收到心跳的时间 / Leader最后一次发出心跳的时间 / 最后一次超时启动选举的时间
    int electionTimeoutMs_;                                 // 选举超时（随机 150-300ms）

    // ==================== 组件依赖 ====================
    std::shared_ptr<RaftTransport> transport_;      // RPC 传输（不依赖具体网络实现）
    std::shared_ptr<RaftStorage> storage_;          // 持久化：meta + 日志
    std::shared_ptr<KVStore> stateMachine_;         // 状态机：KV 内存存储

    // ==================== 并发保护 ====================
    mutable std::mutex mutex_;              // 保护所有状态字段
    std::condition_variable cond_;          // Start() 等待提交完成用
};


/*值成员->shared_ptr
KVStore 需要被 ClientServer 共享（用于 GET）；

RaftStorage 需要被 main 持有（用于快照等）；

RaftTransport

值成员会拷贝（如果有拷贝构造）或根本不能共享。
*/

/*
▲ 加锁的原则：锁的边界是"一次顶层入口的完整逻辑操作"，不是"每个函数"。
- 只有顶层公开方法加锁；
- 内部辅助方法一律不加锁；
- 发 RPC 用作用域限制在锁外。
*/