//test_raft_node.cc
// RaftNode 核心场景测试：FakeTransport 同步直连，手动驱动，不依赖真实网络/线程。
//
// 覆盖：
//   选举：唯一 Leader、重复投票幂等、拒绝他人、高 term 降级、旧 Leader 重选、
//         Tick 超时触发选举、日志落后者不能当选
//   复制：Start 提交、KV 状态机一致、冲突拒绝 + conflictIndex 回退 + 覆盖追平、
//         未达多数超时、空心跳推进 leaderCommit
//   提交规则：§5.4.2 旧 term 日志在新 term 提交前不能被提交
//   持久化：重启恢复 term/log、votedFor 恢复、心跳刷新状态机
//   快照：超阈值触发、InstallSnapshot 让落后节点追平、旧快照被忽略
//   ReadIndex：Leader 成功、非 Leader 失败、下台 abort、Tick 超时
//   单节点集群

#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "raft/raft_node.h"
#include "raft/raft_storage.h"
#include "raft/raft_transport.h"
#include "kv/kv_store.h"
#include "common/command.h"
#include "common/config.h"
#include "test_util.h"

// ==================== FakeTransport ====================

class FakeCluster;

class FakeTransport : public RaftTransport {
public:
    FakeTransport(int32_t selfId, FakeCluster* cluster)
        : selfId_(selfId), cluster_(cluster) {}

    void SendRequestVote(int32_t peerId, const RequestVoteArgs& args,
                         RequestVoteCallback cb) override;
    void SendAppendEntries(int32_t peerId, const AppendEntriesArgs& args,
                           AppendEntriesCallback cb) override;
    void SendInstallSnapshot(int32_t peerId, const InstallSnapshotArgs& args,
                             InstallSnapshotCallback cb) override;

private:
    int32_t selfId_;
    FakeCluster* cluster_;
};

class FakeCluster {
public:
    explicit FakeCluster(int n = 3) : n_(n) {
        for (int i = 0; i < n; ++i) {
            dirs_.push_back(testutil::MakeTempDir("raftnode_" + std::to_string(i)));
            trans_.push_back(std::make_shared<FakeTransport>(i, this));
            alive_.push_back(true);
            swallow_.push_back(false);
            nodes_.push_back(nullptr);
            storages_.push_back(nullptr);
            kvs_.push_back(nullptr);
            createNode(i);
        }
    }

    ~FakeCluster() {
        for (auto& nd : nodes_) nd.reset();
        for (const auto& d : dirs_) testutil::CleanupDir(d);
    }

    void createNode(int i) {
        auto storage = std::make_shared<RaftStorage>(dirs_[i]);
        auto kv = std::make_shared<KVStore>();
        storages_[i] = storage;
        kvs_[i] = kv;
        nodes_[i] = std::make_shared<RaftNode>(i, n_, trans_[i], storage, kv);
    }

    // 模拟节点宕机/恢复（双向不通）
    void isolate(int i) { alive_[i] = false; }
    void heal(int i)    { alive_[i] = true; }

    // 发给 i 的 RPC：i 照常处理，但 reply 被丢弃（模拟回执延迟/丢失）
    void swallowReply(int i, bool on) { swallow_[i] = on; }

    bool canDeliver(int from, int to) const {
        return alive_[from] && alive_[to] && nodes_[from] && nodes_[to];
    }
    bool swallow(int to) const { return swallow_[to]; }

    RaftNode* node(int i) { return nodes_[i].get(); }
    std::shared_ptr<RaftNode> nodePtr(int i) { return nodes_[i]; }
    KVStore* kv(int i) { return kvs_[i].get(); }
    RaftStorage* storage(int i) { return storages_[i].get(); }

    void destroyNode(int i) { nodes_[i].reset(); }
    void recreateNode(int i) {
        nodes_[i].reset();
        storages_[i].reset();
        kvs_[i].reset();
        createNode(i);
    }

    int size() const { return n_; }

    // 找当前 Leader（没有返回 -1）
    int leaderId() const {
        for (int i = 0; i < n_; ++i)
            if (nodes_[i] && nodes_[i]->IsLeader()) return i;
        return -1;
    }

    // 让某个节点发起选举，并等待结果
    void elect(int i) { nodes_[i]->StartElection(); }

    // 让 i 当选（触发一轮选举）
    void electLeader(int i) { nodes_[i]->StartElection(); }

private:
    int n_;
    std::vector<std::string> dirs_;
    std::vector<std::shared_ptr<FakeTransport>> trans_;
    std::vector<std::shared_ptr<RaftNode>> nodes_;
    std::vector<std::shared_ptr<RaftStorage>> storages_;
    std::vector<std::shared_ptr<KVStore>> kvs_;
    std::vector<bool> alive_;
    std::vector<bool> swallow_;
};

void FakeTransport::SendRequestVote(int32_t peerId, const RequestVoteArgs& args,
                                    RequestVoteCallback cb) {
    if (!cluster_->canDeliver(selfId_, peerId)) return;   // 宕机：不处理不回
    RequestVoteReply reply;
    cluster_->node(peerId)->HandleRequestVote(args, reply);
    if (cluster_->swallow(peerId)) return;                // 处理了但回执丢弃
    cb(reply);
}

void FakeTransport::SendAppendEntries(int32_t peerId, const AppendEntriesArgs& args,
                                      AppendEntriesCallback cb) {
    if (!cluster_->canDeliver(selfId_, peerId)) return;
    AppendEntriesReply reply;
    cluster_->node(peerId)->HandleAppendEntries(args, reply);
    if (cluster_->swallow(peerId)) return;
    cb(reply);
}

void FakeTransport::SendInstallSnapshot(int32_t peerId, const InstallSnapshotArgs& args,
                                        InstallSnapshotCallback cb) {
    if (!cluster_->canDeliver(selfId_, peerId)) return;
    InstallSnapshotReply reply;
    cluster_->node(peerId)->HandleInstallSnapshot(args, reply);
    if (cluster_->swallow(peerId)) return;
    cb(reply);
}

// ==================== 辅助 ====================

static Command Put(const std::string& k, const std::string& v) {
    return Command{Command::Type::PUT, k, v};
}

// KV 取值：缺失时返回哨兵串（避免 optional::value() 直接抛异常中断整组用例）
static std::string GetOr(KVStore* kv, const std::string& k) {
    auto v = kv->Get(k);
    return v.value_or("<missing>");
}

// 在 leader 上连续写入 n 条（key=k_i），返回成功条数。
// 同步模型下每轮 AppendEntries 的 leaderCommit 是发送前快照，follower 的
// 状态机应用天然落后一轮，写完后补发一轮心跳让 follower 全部应用。
static int WriteN(FakeCluster& c, int leader, int n) {
    int ok = 0;
    for (int i = 1; i <= n; ++i) {
        if (c.node(leader)->Start(Put("k" + std::to_string(i),
                                      "v" + std::to_string(i))))
            ++ok;
    }
    c.node(leader)->SendHeartbeat();
    return ok;
}

// ==================== 一、选举 ====================

void TestElectSingleLeader() {
    FakeCluster c(3);
    c.electLeader(0);

    CHECK(c.node(0)->IsLeader());
    CHECK_EQ(c.node(0)->GetCurrentTerm(), 1);
    CHECK_EQ(c.leaderId(), 0);
    for (int i = 1; i < 3; ++i) {
        CHECK(c.node(i)->GetRole() == Role::Follower);
        CHECK_EQ(c.node(i)->GetCurrentTerm(), 1);
        CHECK_EQ(c.node(i)->GetLeaderId(), 0);
    }
}

void TestVoteIdempotentAndRejectOther() {
    FakeCluster c(3);
    c.electLeader(0);   // term 1

    // node1 在 term 2 发起选举
    c.electLeader(1);   // term 2，node0、node2 投票
    CHECK(c.node(1)->IsLeader());
    CHECK_EQ(c.node(0)->GetCurrentTerm(), 2);

    // 同 term 再次收到 candidateId=1 的投票请求 -> 幂等，仍同意
    RequestVoteArgs again;
    again.term = 2; again.candidateId = 1;
    again.lastLogIndex = 2; again.lastLogTerm = 1;
    RequestVoteReply r1;
    c.node(0)->HandleRequestVote(again, r1);
    CHECK(r1.voteGranted);

    // 同 term 收到 candidateId=2 的投票请求 -> 拒绝（已投给 1）
    RequestVoteArgs other;
    other.term = 2; other.candidateId = 2;
    other.lastLogIndex = 2; other.lastLogTerm = 1;
    RequestVoteReply r2;
    c.node(0)->HandleRequestVote(other, r2);
    CHECK(!r2.voteGranted);
}

void TestRejectStaleCandidate() {
    FakeCluster c(3);
    c.electLeader(0);   // term 1

    // 一个 term 更旧的候选者
    RequestVoteArgs stale;
    stale.term = 1; stale.candidateId = 2;
    stale.lastLogIndex = 0; stale.lastLogTerm = 0;
    // 先让 node0 到 term 3
    RequestVoteArgs newer;
    newer.term = 3; newer.candidateId = 2;
    newer.lastLogIndex = 0; newer.lastLogTerm = 0;
    RequestVoteReply rn;
    c.node(0)->HandleRequestVote(newer, rn);
    CHECK_EQ(c.node(0)->GetCurrentTerm(), 3);

    RequestVoteReply rs;
    c.node(0)->HandleRequestVote(stale, rs);
    CHECK(!rs.voteGranted);
    CHECK_EQ(rs.term, 3);   // 回执带回更高 term
}

void TestHigherTermStepsDown() {
    FakeCluster c(3);
    c.electLeader(0);
    CHECK(c.node(0)->IsLeader());

    // 收到 term 9 的 AppendEntries -> 立刻降级
    AppendEntriesArgs ae;
    ae.term = 9; ae.leaderId = 2;
    ae.prevLogIndex = 0; ae.prevLogTerm = 0;
    ae.leaderCommit = 0;
    AppendEntriesReply reply;
    c.node(0)->HandleAppendEntries(ae, reply);
    CHECK(!c.node(0)->IsLeader());
    CHECK(c.node(0)->GetRole() == Role::Follower);
    CHECK_EQ(c.node(0)->GetCurrentTerm(), 9);
    CHECK_EQ(c.node(0)->GetLeaderId(), 2);
}

void TestReElectAfterLeaderDown() {
    FakeCluster c(3);
    c.electLeader(0);

    c.isolate(0);                       // 旧 Leader 宕机
    c.electLeader(1);                   // 新选举（node2 投票）
    CHECK(c.node(1)->IsLeader());
    CHECK_EQ(c.node(1)->GetCurrentTerm(), 2);
    CHECK_EQ(c.node(2)->GetLeaderId(), 1);
    CHECK_EQ(c.node(2)->GetCurrentTerm(), 2);
}

void TestLaggingCandidateCannotWin() {
    FakeCluster c(3);
    c.electLeader(0);
    CHECK_EQ(WriteN(c, 0, 3), 3);       // 三节点都有 1~3

    // node2 日志落后（隔离它，让它停在 0 条；此时 leader 0 无法再写入，
    // 直接用一个日志更短的节点发 RV）
    c.isolate(2);
    // 一个全新（空日志）的候选者拉不起来——直接验证 node2 不会投票给更短日志者
    // node2 自身日志为空，它试图拉票，但 node0 日志更新，应拒绝
    // 这里让 node2 发起选举（它拿不到多数票，无法成为 Leader）
    c.node(2)->StartElection();
    // node2 被隔离：谁也收不到它的 RV，自然当不了 Leader
    CHECK(!c.node(2)->IsLeader());

    // 反方向验证：空日志的 node2 收到日志更长的 node0 同 term RV 会投；
    // 而 node0 不会投给日志更短的候选者
    c.heal(2);
    RequestVoteArgs shortLog;
    shortLog.term = 2; shortLog.candidateId = 2;
    shortLog.lastLogIndex = 0; shortLog.lastLogTerm = 0;
    RequestVoteReply reply;
    c.node(0)->HandleRequestVote(shortLog, reply);
    // node0 有 3 条日志（term1），候选者 0 条 -> 拒绝
    CHECK(!reply.voteGranted);
}

void TestTickTriggersElection() {
    FakeCluster c(3);
    c.electLeader(0);
    // 让 node1 的选举超时到期（150~300ms，这里等 350ms）
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    // node1 收不到心跳也会超时；这里再隔离 node0 让它彻底拿不到心跳
    c.isolate(0);
    c.node(1)->Tick();   // 超时 -> StartElection（node2 投票，node1 当选 term2）
    CHECK(c.node(1)->IsLeader());
}

// ==================== 二、日志复制 ====================

void TestStartReplicatesAndApplies() {
    FakeCluster c(3);
    c.electLeader(0);
    CHECK(c.node(0)->Start(Put("name", "alice")));
    c.node(0)->SendHeartbeat();   // 推进 follower 的 leaderCommit

    // 三个节点状态机都应用了
    for (int i = 0; i < 3; ++i) {
        CHECK_STR_EQ(GetOr(c.kv(i), "name"), "alice");
    }
}

void TestStartManyInOrder() {
    FakeCluster c(3);
    c.electLeader(0);
    CHECK_EQ(WriteN(c, 0, 10), 10);
    for (int i = 0; i < 3; ++i) {
        for (int k = 1; k <= 10; ++k) {
            CHECK_STR_EQ(GetOr(c.kv(i), "k" + std::to_string(k)),
                         "v" + std::to_string(k));
        }
    }
}

void TestNotLeaderStartFalse() {
    FakeCluster c(3);
    c.electLeader(0);
    CHECK(!c.node(1)->Start(Put("x", "y")));
    CHECK(!c.node(2)->Start(Put("x", "y")));
}

void TestNoMajorityTimeout() {
    FakeCluster c(3);
    c.electLeader(0);
    c.isolate(1);
    c.isolate(2);
    // 只有 Leader 自己：1 票 < 多数（3 节点需 2），Start 超时返回 false
    bool ok = c.node(0)->Start(Put("lonely", "1"));
    CHECK(!ok);
    // follower 从未收到
    CHECK(!c.kv(1)->Get("lonely").has_value());
    CHECK(!c.kv(2)->Get("lonely").has_value());
}

void TestHeartbeatAdvancesLeaderCommit() {
    FakeCluster c(3);
    c.electLeader(0);
    // 手动构造：follower 先存了 entry 但 leaderCommit=0（未提交），
    // 再用空心跳把 leaderCommit 推进到 1
    LogEntry e1{1, 1, Put("hc", "1")};
    {
        AppendEntriesArgs store;
        store.term = 1; store.leaderId = 0;
        store.prevLogIndex = 0; store.prevLogTerm = 0;
        store.entries = {e1};
        store.leaderCommit = 0;
        AppendEntriesReply rr;
        c.node(2)->HandleAppendEntries(store, rr);
        CHECK(rr.success);
        // 还没提交：状态机里没有
        CHECK(!c.kv(2)->Get("hc").has_value());

        AppendEntriesArgs hb;
        hb.term = 1; hb.leaderId = 0;
        hb.prevLogIndex = 1; hb.prevLogTerm = 1;
        hb.entries = {};
        hb.leaderCommit = 1;
        AppendEntriesReply rr2;
        c.node(2)->HandleAppendEntries(hb, rr2);
        CHECK(rr2.success);
        CHECK(c.kv(2)->Get("hc").has_value());
    }
}

void TestConflictRejectThenOverwrite() {
    FakeCluster c(3);
    c.electLeader(0);
    CHECK_EQ(WriteN(c, 0, 3), 3);                 // 1~3 全复制

    // 分区：{0} vs {1,2}
    c.isolate(0);
    // 旧 Leader 0 在隔离期尝试写 entry4(term1)：只在自己内存/磁盘，超时失败
    CHECK(!c.node(0)->Start(Put("ghost", "old")));
    // node1 当选 term2，写 4、5（term2）并提交
    c.electLeader(1);
    CHECK(c.node(1)->IsLeader());
    CHECK(c.node(1)->Start(Put("k4", "new4")));
    CHECK(c.node(1)->Start(Put("k5", "new5")));

    // 恢复前，模拟 Leader 误以为 node0 已有 4t2，从 index=5 发 -> 冲突拒绝
    c.heal(0);
    {
        AppendEntriesArgs wrong;
        wrong.term = 2; wrong.leaderId = 1;
        wrong.prevLogIndex = 4; wrong.prevLogTerm = 2;
        wrong.entries = {LogEntry{5, 2, Put("k5", "new5")}};
        wrong.leaderCommit = 5;
        AppendEntriesReply rr;
        c.node(0)->HandleAppendEntries(wrong, rr);
        CHECK(!rr.success);
        // node0 在 4 的位置是 term1，返回冲突起点
        CHECK(rr.conflictIndex >= 1);

        // Leader 收到拒绝回执：回退 nextIndex
        c.node(1)->HandleAppendEntriesReply(0, rr);
    }
    // 让 Leader 按回退后的 nextIndex 正常发一轮（真实收敛）
    c.node(1)->SendHeartbeat();

    // node0 最终被 term2 日志覆盖追平
    CHECK_STR_EQ(GetOr(c.kv(0), "ghost"), "<missing>");  // 未提交的 4t1 被覆盖
    CHECK_STR_EQ(GetOr(c.kv(0), "k4"), "new4");
    CHECK_STR_EQ(GetOr(c.kv(0), "k5"), "new5");
    CHECK_EQ(c.node(0)->GetCurrentTerm(), 2);
}

// ==================== 三、§5.4.2 提交规则 ====================

void TestCannotCommitOldTermByMajority() {
    FakeCluster c(3);
    c.electLeader(0);
    CHECK_EQ(WriteN(c, 0, 2), 2);   // 1、2 提交

    // 两个 follower 都"处理但回执丢弃"：entry3 落到 3 个节点（多数副本），
    // 但 Leader 0 视角看不到任何 ack，commitIndex 停在 2
    c.swallowReply(1, true);
    c.swallowReply(2, true);
    c.node(0)->Start(Put("k3", "oldterm"));   // 超时（但不影响 follower 已存）
    c.swallowReply(1, false);
    c.swallowReply(2, false);

    // node0 隔离，node1 带 entry3(term1) 当选 term2
    c.isolate(0);
    c.electLeader(1);
    CHECK(c.node(1)->IsLeader());
    CHECK_EQ(c.node(1)->GetCurrentTerm(), 2);

    // 新 Leader 尚未写 term2 日志：一轮心跳后，即使 entry3 已在多数副本，
    // 也不能直接提交（term1 日志）
    c.node(1)->SendHeartbeat();
    CHECK(!c.kv(1)->Get("k3").has_value());
    CHECK(!c.kv(2)->Get("k3").has_value());

    // 一旦写入并提交 term2 的 entry4，entry3 被顺带提交
    CHECK(c.node(1)->Start(Put("k4", "newterm")));
    c.node(1)->SendHeartbeat();
    CHECK_STR_EQ(GetOr(c.kv(1), "k3"), "oldterm");
    CHECK_STR_EQ(GetOr(c.kv(1), "k4"), "newterm");
}

// ==================== 四、持久化恢复 ====================

void TestRestartRestoresMetaAndLog() {
    FakeCluster c(3);
    c.electLeader(0);
    CHECK_EQ(WriteN(c, 0, 2), 2);
    int64_t oldTerm = c.node(0)->GetCurrentTerm();

    c.destroyNode(0);
    c.recreateNode(0);
    // term 恢复
    CHECK_EQ(c.node(0)->GetCurrentTerm(), oldTerm);
    // 重启后状态机暂未重放日志（无快照，lastApplied=0）
    CHECK(!c.kv(0)->Get("k1").has_value());

    // 让 node1 当选新 term，通过心跳把 node0 的状态机刷新追平
    c.node(1)->StartElection();
    c.node(1)->SendHeartbeat();
    CHECK_STR_EQ(GetOr(c.kv(0), "k1"), "v1");
    CHECK_STR_EQ(GetOr(c.kv(0), "k2"), "v2");
}

void TestVotedForPersistedAcrossRestart() {
    FakeCluster c(3);
    c.electLeader(0);
    // node1 在 term2 竞选，node0 投给它
    c.node(1)->StartElection();
    CHECK(c.node(1)->IsLeader());
    CHECK_EQ(c.node(0)->GetCurrentTerm(), 2);

    // node0 重启：term=2 / votedFor=1 恢复
    c.destroyNode(0);
    c.recreateNode(0);
    // 同 term 内 candidateId=2 来拉票 -> 因已投给 1 而拒绝
    RequestVoteArgs args;
    args.term = 2; args.candidateId = 2;
    args.lastLogIndex = 0; args.lastLogTerm = 0;
    RequestVoteReply reply;
    c.node(0)->HandleRequestVote(args, reply);
    CHECK(!reply.voteGranted);
}

// ==================== 五、快照 ====================

void TestSnapshotTriggeredByThreshold() {
    FakeCluster c(3);
    c.electLeader(0);
    // 阈值 kSnapshotThreshold=20：写 21 条必然触发
    int ok = WriteN(c, 0, 21);
    CHECK_EQ(ok, 21);

    CHECK(c.storage(0)->HasSnapshot());
    auto snap = c.storage(0)->LoadSnapshot();
    REQUIRE(snap.has_value());
    CHECK_EQ(snap->lastIncludedIndex, 21);

    // 状态机数据完整
    for (int i = 1; i <= 21; ++i) {
        CHECK_STR_EQ(GetOr(c.kv(0), "k" + std::to_string(i)),
                     "v" + std::to_string(i));
    }

    // 快照后还能继续写入（index 连续）
    CHECK(c.node(0)->Start(Put("after", "snap")));
    c.node(0)->SendHeartbeat();
    CHECK_STR_EQ(GetOr(c.kv(0), "after"), "snap");
}

void TestInstallSnapshotCatchesUpFollower() {
    FakeCluster c(3);
    // node2 开局隔离，Leader 0 靠 node1 当选并写入 21 条（触发快照）
    c.isolate(2);
    c.node(0)->StartElection();
    CHECK(c.node(0)->IsLeader());
    CHECK_EQ(WriteN(c, 0, 21), 21);
    CHECK(c.storage(0)->HasSnapshot());

    // 恢复 node2：它空日志，nextIndex=1 <= snapshotIndex(21) -> 发快照
    c.heal(2);
    c.node(0)->SendHeartbeat();

    // node2 通过快照追平
    CHECK(c.storage(2)->HasSnapshot());
    auto snap = c.storage(2)->LoadSnapshot();
    REQUIRE(snap.has_value());
    CHECK_EQ(snap->lastIncludedIndex, 21);
    for (int i = 1; i <= 21; ++i) {
        CHECK_STR_EQ(GetOr(c.kv(2), "k" + std::to_string(i)),
                     "v" + std::to_string(i));
    }

    // 快照之后新写入的日志能正常复制给 node2
    CHECK(c.node(0)->Start(Put("post", "1")));
    c.node(0)->SendHeartbeat();
    CHECK_STR_EQ(GetOr(c.kv(2), "post"), "1");
}

void TestOlderSnapshotIgnored() {
    FakeCluster c(3);
    c.isolate(2);
    c.node(0)->StartElection();
    CHECK_EQ(WriteN(c, 0, 21), 21);
    c.heal(2);
    c.node(0)->SendHeartbeat();            // node2 装了 index=21 的快照
    auto s0 = c.storage(2)->LoadSnapshot();
    REQUIRE(s0.has_value());
    CHECK_EQ(s0->lastIncludedIndex, 21);

    // 再发一个更旧的快照（index=10）-> 被忽略，保留本地 21
    InstallSnapshotArgs old;
    old.term = 1; old.leaderId = 0;
    old.lastIncludedIndex = 10; old.lastIncludedTerm = 1;
    old.data = "k1\nv1\n";
    old.dataLen = static_cast<int64_t>(old.data.size());
    InstallSnapshotReply reply;
    c.node(2)->HandleInstallSnapshot(old, reply);
    CHECK(reply.success);
    CHECK_EQ(reply.lastIncludedIndex, 21);   // 回报本地快照点
    auto s1 = c.storage(2)->LoadSnapshot();
    REQUIRE(s1.has_value());
    CHECK_EQ(s1->lastIncludedIndex, 21);
}

// ==================== 六、ReadIndex ====================

void TestReadIndexOnLeader() {
    FakeCluster c(3);
    c.electLeader(0);
    CHECK_EQ(WriteN(c, 0, 1), 1);   // 产生心跳 ack

    std::promise<bool> result;
    std::future<bool> f = result.get_future();
    c.node(0)->ReadIndex([&result](bool ok) { result.set_value(ok); });
    // ReadIndex 内部会立即补发一轮心跳，同步触发多数 ack -> 回调
    CHECK_EQ(f.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    CHECK(f.get());
}

void TestReadIndexOnNonLeader() {
    FakeCluster c(3);
    c.electLeader(0);
    std::promise<bool> result;
    std::future<bool> f = result.get_future();
    c.node(1)->ReadIndex([&result](bool ok) { result.set_value(ok); });
    CHECK_EQ(f.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    CHECK(!f.get());
}

void TestReadIndexAbortedWhenStepDown() {
    FakeCluster c(3);
    c.electLeader(0);
    CHECK_EQ(WriteN(c, 0, 1), 1);
    // 隔离所有 peer：新心跳无法获得多数 ack，读会挂起
    c.isolate(1);
    c.isolate(2);
    // 等待时间超过上一轮心跳 ack，确保 bornTime 晚于 validTime
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::promise<bool> result;
    std::future<bool> f = result.get_future();
    c.node(0)->ReadIndex([&result](bool ok) { result.set_value(ok); });
    // 此刻读仍在 pending（心跳发不出去）
    CHECK_EQ(f.wait_for(std::chrono::milliseconds(100)),
             std::future_status::timeout);

    // 收到更高 term 的 RPC 被迫下台 -> abort 所有挂起读
    RequestVoteArgs higher;
    higher.term = 5; higher.candidateId = 1;
    higher.lastLogIndex = 1; higher.lastLogTerm = 1;
    RequestVoteReply reply;
    c.node(0)->HandleRequestVote(higher, reply);
    CHECK_EQ(f.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    CHECK(!f.get());
}

void TestReadIndexTimeoutViaTick() {
    FakeCluster c(3);
    c.electLeader(0);
    CHECK_EQ(WriteN(c, 0, 1), 1);
    c.isolate(1);
    c.isolate(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::promise<bool> result;
    std::future<bool> f = result.get_future();
    c.node(0)->ReadIndex([&result](bool ok) { result.set_value(ok); });

    // 超过 500ms 后 Tick 清理超时 pending read
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    c.node(0)->Tick();
    CHECK_EQ(f.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    CHECK(!f.get());
}

// ==================== 七、单节点 ====================

void TestSingleNodeCluster() {
    FakeCluster c(1);
    c.node(0)->StartElection();   // 自己一票即多数
    CHECK(c.node(0)->IsLeader());

    // 单节点：自己 1 票即多数，写直接提交
    CHECK(c.node(0)->Start(Put("only", "me")));
    CHECK_STR_EQ(GetOr(c.kv(0), "only"), "me");

    // 单节点 ReadIndex：自己心跳时间为 max，立即确认
    std::promise<bool> result;
    std::future<bool> f = result.get_future();
    c.node(0)->ReadIndex([&result](bool ok) { result.set_value(ok); });
    CHECK_EQ(f.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    CHECK(f.get());
}

int main() {
    std::cout << "===== RaftNode Tests (FakeTransport) =====\n";

    // 选举
    RUN_TEST(TestElectSingleLeader);
    RUN_TEST(TestVoteIdempotentAndRejectOther);
    RUN_TEST(TestRejectStaleCandidate);
    RUN_TEST(TestHigherTermStepsDown);
    RUN_TEST(TestReElectAfterLeaderDown);
    RUN_TEST(TestLaggingCandidateCannotWin);
    RUN_TEST(TestTickTriggersElection);
    // 复制
    RUN_TEST(TestStartReplicatesAndApplies);
    RUN_TEST(TestStartManyInOrder);
    RUN_TEST(TestNotLeaderStartFalse);
    RUN_TEST(TestNoMajorityTimeout);
    RUN_TEST(TestHeartbeatAdvancesLeaderCommit);
    RUN_TEST(TestConflictRejectThenOverwrite);
    // 提交规则
    RUN_TEST(TestCannotCommitOldTermByMajority);
    // 持久化
    RUN_TEST(TestRestartRestoresMetaAndLog);
    RUN_TEST(TestVotedForPersistedAcrossRestart);
    // 快照
    RUN_TEST(TestSnapshotTriggeredByThreshold);
    RUN_TEST(TestInstallSnapshotCatchesUpFollower);
    RUN_TEST(TestOlderSnapshotIgnored);
    // ReadIndex
    RUN_TEST(TestReadIndexOnLeader);
    RUN_TEST(TestReadIndexOnNonLeader);
    RUN_TEST(TestReadIndexAbortedWhenStepDown);
    RUN_TEST(TestReadIndexTimeoutViaTick);
    // 单节点
    RUN_TEST(TestSingleNodeCluster);

    TEST_SUMMARY_RETURN();
}
