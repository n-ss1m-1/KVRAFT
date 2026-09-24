//rpc_server.h

#pragma once

#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/InetAddress.h>
#include "rpc/codec.h"

class RaftNode;


class RpcServer
{
public:
    RpcServer(muduo::net::EventLoop* loop,uint16_t port,RaftNode* node);
    
    void Start();

    void OnMessage(const muduo::net::TcpConnectionPtr& conn,const std::string& msg,muduo::Timestamp receiveTime);

    void HandleMessage(const muduo::net::TcpConnectionPtr& conn,const std::string& message);

private:
    muduo::net::TcpServer server_;
    LengthHeaderCodec codec_;               // 接收：读取长度头，当长度符合时调用设置好的回调函数(自动去掉长度头) | 发送：自动添加长度头 
    RaftNode* node_;                        // 不拥有 RaftNode，生命周期由 main 保证   约束：RaftNode 必须活得比 RpcServer 长
};