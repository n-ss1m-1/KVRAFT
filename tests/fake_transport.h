#pragma once

#include <cstdint>
#include <unordered_map>

#include "../src/raft/raft_node.h"
#include "../src/raft/raft_transport.h"

// 模拟网络：在同一个进程里直接调用对端 RaftNode 的 HandleXXX 方法
class FakeTransport : public RaftTransport {
public:
    void RegisterNode(int32_t nodeId, RaftNode* node) {
        nodes_[nodeId] = node;
    }

    void UnregisterNode(int32_t nodeId) {
        nodes_.erase(nodeId);
    }

    bool SendRequestVote(int32_t peerId,
                          const RequestVoteArgs& args,
                          RequestVoteReply& reply) override {
        auto it = nodes_.find(peerId);
        if (it == nodes_.end()) return false;
        it->second->HandleRequestVote(args, reply);
        return true;
    }
                            
    bool SendAppendEntries(int32_t peerId,
                            const AppendEntriesArgs& args,
                            AppendEntriesReply& reply) override {
        auto it = nodes_.find(peerId);
        if (it == nodes_.end()) return false;
        it->second->HandleAppendEntries(args, reply);
        return true;
    }

private:
    std::unordered_map<int32_t, RaftNode*> nodes_;
};