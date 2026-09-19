#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "fake_transport.h"
#include "../src/kv/kv_store.h"
#include "../src/raft/raft_node.h"
#include "../src/raft/raft_storage.h"

// ==================== 辅助 ====================

static int g_counter = 0;

std::string MakeTestDir(const std::string& name) {
    std::string dir = "test_data/raft_node_" + name + "_"
                      + std::to_string(getpid()) + "_"
                      + std::to_string(g_counter++);
    std::filesystem::create_directories(dir);
    return dir;
}

// 一个测试集群：N 个节点共享一个 FakeTransport
struct TestCluster {
    std::shared_ptr<FakeTransport> transport;
    std::vector<std::shared_ptr<RaftNode>> nodes;
    std::vector<std::shared_ptr<KVStore>> kvs;
    std::vector<std::string> dirs;

    explicit TestCluster(int n) {
        transport = std::make_shared<FakeTransport>();

        for (int i = 0; i < n; i++) {
            std::string dir = MakeTestDir("node" + std::to_string(i));
            dirs.push_back(dir);

            auto storage = std::make_shared<RaftStorage>(dir);
            auto kv = std::make_shared<KVStore>();
            auto node = std::make_shared<RaftNode>(i, n, transport, storage, kv);

            nodes.push_back(node);
            kvs.push_back(kv);
            transport->RegisterNode(i, node.get());
        }
    }

    ~TestCluster() {
        // 先释放节点，再释放 transport（FakeTransport 持有裸指针）
        nodes.clear();
        kvs.clear();
        transport.reset();

        for (const auto& d : dirs) {
            std::filesystem::remove_all(d);
        }
    }
};

// ==================== 测试 1：单节点选举 ====================

void TestSingleNodeElection() {
    TestCluster cluster(1);
    auto& node = cluster.nodes[0];

    assert(node->GetRole() == Role::Follower);
    assert(node->GetCurrentTerm() == 0);

    node->StartElection();

    // 单节点：自己投票就超过半数（1 > 0）
    assert(node->GetRole() == Role::Leader);
    assert(node->GetCurrentTerm() == 1);

    std::cout << "[PASS] TestSingleNodeElection\n";
}

// ==================== 测试 2：3 节点选举 ====================

void TestThreeNodeElection() {
    TestCluster cluster(3);

    cluster.nodes[0]->StartElection();

    // node0 应该是 Leader
    assert(cluster.nodes[0]->GetRole() == Role::Leader);
    assert(cluster.nodes[0]->GetCurrentTerm() == 1);

    // node1、node2 变成 Follower，且知道 Leader 是 0
    assert(cluster.nodes[1]->GetRole() == Role::Follower);
    assert(cluster.nodes[1]->GetCurrentTerm() == 1);
    assert(cluster.nodes[1]->GetLeaderId() == 0);

    assert(cluster.nodes[2]->GetRole() == Role::Follower);
    assert(cluster.nodes[2]->GetCurrentTerm() == 1);
    assert(cluster.nodes[2]->GetLeaderId() == 0);

    std::cout << "[PASS] TestThreeNodeElection\n";
}

// ==================== 测试 3：Follower 的 Start 应该返回 false ====================

void TestFollowerStartFails() {
    TestCluster cluster(3);

    cluster.nodes[0]->StartElection();
    assert(cluster.nodes[0]->IsLeader());

    // Follower 收到客户端写请求
    Command cmd{Command::Type::PUT, "x", "1"};
    bool ok = cluster.nodes[1]->Start(cmd);

    assert(!ok);
    assert(!cluster.kvs[1]->Get("x").has_value());

    std::cout << "[PASS] TestFollowerStartFails\n";
}

// ==================== 测试 4：Leader 写数据，复制到所有节点 ====================

void TestThreeNodeLogReplication() {
    TestCluster cluster(3);

    cluster.nodes[0]->StartElection();
    assert(cluster.nodes[0]->IsLeader());

    Command cmd{Command::Type::PUT, "x", "1"};
    bool ok = cluster.nodes[0]->Start(cmd);
    assert(ok);

    // ★ Leader 的 commitIndex_ 更新了，但 Follower 还不知道
    //   手动触发一次心跳，把 commitIndex_ 传给 Follower
    cluster.nodes[0]->SendHeartbeat();   // 需要 SendHeartbeat 是 public

    // 所有节点的状态机都应该有 x=1
    assert(cluster.kvs[0]->Get("x").value() == "1");
    assert(cluster.kvs[1]->Get("x").value() == "1");
    assert(cluster.kvs[2]->Get("x").value() == "1");

    std::cout << "[PASS] TestThreeNodeLogReplication\n";
}

// ==================== 测试 5：Leader 挂了，重新选举 ====================

void TestLeaderFailover() {
    TestCluster cluster(3);

    cluster.nodes[0]->StartElection();
    assert(cluster.nodes[0]->IsLeader());
    assert(cluster.nodes[0]->GetCurrentTerm() == 1);

    // 模拟 node0 宕机：从 FakeTransport 中注销
    cluster.transport->UnregisterNode(0);

    // node1 发起竞选
    cluster.nodes[1]->StartElection();

    // node1 当选（term=2）
    assert(cluster.nodes[1]->IsLeader());
    assert(cluster.nodes[1]->GetCurrentTerm() == 2);

    // node2 跟随 node1
    assert(cluster.nodes[2]->GetLeaderId() == 1);

    std::cout << "[PASS] TestLeaderFailover\n";
}

// ==================== 测试 6：连续多次写 ====================

void TestMultiplePuts() {
    TestCluster cluster(3);

    cluster.nodes[0]->StartElection();
    assert(cluster.nodes[0]->IsLeader());

    for (int i = 0; i < 5; i++) {
        Command cmd{Command::Type::PUT,
                    "k" + std::to_string(i),
                    "v" + std::to_string(i)};
        bool ok = cluster.nodes[0]->Start(cmd);
        assert(ok);
    }

    // ★ Leader 的 commitIndex_ 更新了，但 Follower 还不知道
    //   手动触发一次心跳，把 commitIndex_ 传给 Follower
    cluster.nodes[0]->SendHeartbeat();   // 需要 SendHeartbeat 是 public

    for (int i = 0; i < 5; i++) {
        std::string expected = "v" + std::to_string(i);
        std::string key = "k" + std::to_string(i);
        assert(cluster.kvs[0]->Get(key).value() == expected);
        assert(cluster.kvs[1]->Get(key).value() == expected);
        assert(cluster.kvs[2]->Get(key).value() == expected);
    }

    std::cout << "[PASS] TestMultiplePuts\n";
}

// ==================== 测试 7：DEL 操作 ====================

void TestDelete() {
    TestCluster cluster(3);

    cluster.nodes[0]->StartElection();

    // 先 PUT
    cluster.nodes[0]->Start(Command{Command::Type::PUT, "x", "1"});
    assert(cluster.kvs[0]->Get("x").has_value());

    // 再 DEL
    cluster.nodes[0]->Start(Command{Command::Type::DEL, "x", ""});

    // ★ Leader 的 commitIndex_ 更新了，但 Follower 还不知道
    //   手动触发一次心跳，把 commitIndex_ 传给 Follower
    cluster.nodes[0]->SendHeartbeat();   // 需要 SendHeartbeat 是 public
    
    assert(!cluster.kvs[0]->Get("x").has_value());
    assert(!cluster.kvs[1]->Get("x").has_value());
    assert(!cluster.kvs[2]->Get("x").has_value());

    std::cout << "[PASS] TestDelete\n";
}

// ==================== main ====================

int main() {
    std::cout << "===== RaftNode Tests =====\n";

    TestSingleNodeElection();
    TestThreeNodeElection();
    TestFollowerStartFails();
    TestThreeNodeLogReplication();
    TestLeaderFailover();
    TestMultiplePuts();
    TestDelete();

    std::cout << "\nAll RaftNode tests passed!\n";
    return 0;
}