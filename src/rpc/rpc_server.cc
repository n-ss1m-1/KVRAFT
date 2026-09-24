//rpc_server.cc
#include<sstream>
#include<iostream>

#include "rpc/rpc_server.h"
#include "rpc/rpc_message.h"
#include "raft/raft_node.h"


RpcServer::RpcServer(muduo::net::EventLoop* loop,uint16_t port,RaftNode* node):
    server_(loop,muduo::net::InetAddress(port),"RpcServer"),
    codec_([this](const muduo::net::TcpConnectionPtr& conn,         //lambda表达式比bind更清晰?
              const std::string& msg,
              muduo::Timestamp receiveTime){
                OnMessage(conn,msg,receiveTime);
              }),
    node_(node)
{
    server_.setMessageCallback(
        [this](const muduo::net::TcpConnectionPtr& conn,muduo::net::Buffer* buf,muduo::Timestamp receiveTime){ 
            codec_.onMessage(conn,buf,receiveTime);        //codec 解析长度头
        });
}

void RpcServer::Start()
{
    server_.start();
}


void RpcServer::OnMessage(const muduo::net::TcpConnectionPtr& conn,const std::string& msg,muduo::Timestamp receiveTime)
{
    HandleMessage(conn,msg);
}

void RpcServer::HandleMessage(const muduo::net::TcpConnectionPtr& conn,const std::string& msg)
{
    //将msg作为string输入流
    std::istringstream iss(msg);

    //读取：消息类型 + reqId
    uint32_t typeNum;
    uint64_t reqId;
    if(!(iss>>typeNum>>reqId)) return;

    std::cout << "[RpcServer] received type=" << typeNum
              << " reqId=" << reqId << std::endl;

    RpcMessageType type = static_cast<RpcMessageType>(typeNum);

    //分发消息处理
    switch (type)
    {
        case RpcMessageType::kRequestVote:
        {
            auto argsOpt = DeserializeRequestVoteArgs(iss);
            if(!argsOpt.has_value()) return;

            RequestVoteReply reply;
            node_->HandleRequestVote(argsOpt.value(),reply);

            std::string replyStr = SerializeRequestVoteReply(reqId,reply);
            codec_.send(conn.get(),replyStr);                                        //传递裸指针   !!!无法选举成功的原因：本应该回复replyStr，写错成msg了

            break;
        }
        case RpcMessageType::kAppendEntries:
        {
            auto argsOpt = DeserializeAppendEntriesArgs(iss);
            if(!argsOpt.has_value()) return;

            AppendEntriesReply reply;
            node_->HandleAppendEntries(argsOpt.value(),reply);

            std::string replyStr = SerializeAppendEntriesReply(reqId,reply);
            codec_.send(conn.get(),replyStr);                                        //传递裸指针  !!!无法选举成功的原因：本应该回复replyStr，写错成msg了

            break;
        }
            
    default:
        break;
    }

}



/* !▲
1. LengthHeaderCodec 的构造函数接受一个回调，参数是 (conn, msg, time)

2. codec_.onMessage(conn, buf, time) 从 Buffer 里读长度头 + payload，攒够一条就触发回调(会自动去掉长度头)

3. codec_.send(conn, msg) 会自动添加长度头

*/