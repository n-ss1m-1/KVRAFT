//rpc_client.h
#pragma once
#include<unordered_map>
#include<mutex>
#include<memory>

#include<muduo/net/TcpClient.h>
#include<muduo/net/EventLoop.h>
#include<muduo/net/InetAddress.h>
#include "rpc/codec.h"
#include "raft/raft_transport.h"
#include "rpc_message.h"
#include "common/types.h"


class RpcClient : public RaftTransport
{
public:
    RpcClient(muduo::net::EventLoop* loop,const std::vector<PeerInfo>& peers);
    ~RpcClient();
    void Stop();
    void SendRequestVote(int32_t peerId,const RequestVoteArgs& args,RequestVoteCallback cb) override;            //const &或者 值传递 才能接收lambda表达式
    void SendAppendEntries(int32_t peerId,const AppendEntriesArgs& args,AppendEntriesCallback cb) override;      //值传递：使用move零拷贝


private:
    struct peerConn
    {
        std::unique_ptr<muduo::net::TcpClient> client;
        muduo::net::TcpConnectionPtr conn;
        std::unique_ptr<LengthHeaderCodec> codec;

        //  新增：最后一次收到该 peer 回复的时间
        std::chrono::steady_clock::time_point lastResponseTime;
    
        //  新增：最后一次发送的时间（用于检测"发了但没回"）
        std::chrono::steady_clock::time_point lastSendTime;
    };
    struct PendingVote
    {
        int32_t peerId;
        RequestVoteCallback cb;
    };
    struct PendingAppend
    {
        int32_t peerId;
        AppendEntriesCallback cb;
    };
    
    void CheckPeerHealth(int32_t peerId, RpcClient::peerConn* pc);      // 调用前必须已持锁

    void OnConnection(int32_t peerId,peerConn* pc,const muduo::net::TcpConnectionPtr& conn);        //?

    void handleReply(int32_t peerId, const std::string& msg);

    std::unordered_map<int32_t,std::unique_ptr<peerConn>> peers_;

    muduo::net::EventLoop* loop_;


    uint64_t nextReqId_ = 0;
    std::unordered_map<uint64_t,PendingVote> pendingVoteCallbacks_;
    std::unordered_map<uint64_t,PendingAppend> pendingAppendCallbacks_;
    bool stopping_ = false;
    std::mutex mutex_;
};