//client_server.h

#pragma once
#include<memory>

#include<muduo/net/EventLoop.h>
#include<muduo/net/InetAddress.h>
#include<muduo/net/TcpConnection.h>
#include<muduo/net/TcpServer.h>

#include "rpc/codec.h"
#include "common/thread_pool.h"

class KVStore;
class RaftNode;



class ClientServer
{
public:
    ClientServer(muduo::net::EventLoop* loop,uint16_t port,const std::shared_ptr<KVStore>& kvStore,const std::shared_ptr<RaftNode>& raftNode,const std::shared_ptr<ThreadPool>& threadPool);

    void Start();


private:
    void OnConnection(const muduo::net::TcpConnectionPtr& conn);
    void OnMessage(const muduo::net::TcpConnectionPtr& conn,const std::string& msg,muduo::Timestamp receiveTime);
    void HandleCommand(const muduo::net::TcpConnectionPtr& conn,const std::string& msg);

    muduo::net::TcpServer server_;
    LengthHeaderCodec codec_;
    std::shared_ptr<KVStore> kvStore_;     
    std::shared_ptr<RaftNode> raftNode_;
    std::shared_ptr<ThreadPool> threadPool_;
};


