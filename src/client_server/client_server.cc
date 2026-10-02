//client_server.cc

#include<spdlog/spdlog.h>

#include "client_server/client_server.h"
#include "kv/kv_store.h"
#include "raft/raft_node.h"


ClientServer::ClientServer(muduo::net::EventLoop* loop,uint16_t port,const std::shared_ptr<KVStore>& kvStore,const std::shared_ptr<RaftNode>& raftNode,const std::shared_ptr<ThreadPool>& threadPool):
    server_(loop,muduo::net::InetAddress(port),"ClientServer"),
    codec_(
        [this](const muduo::net::TcpConnectionPtr& conn,const std::string& msg,muduo::Timestamp receiveTime){
            OnMessage(conn,msg,receiveTime);
        }),
    kvStore_(kvStore),
    raftNode_(raftNode),
    threadPool_(threadPool)
{
    server_.setConnectionCallback(
        [this](const muduo::net::TcpConnectionPtr& conn){
            OnConnection(conn);
        });
    server_.setMessageCallback(
        [this](const muduo::net::TcpConnectionPtr& conn,muduo::net::Buffer* buf,muduo::Timestamp receiveTime){
            codec_.onMessage(conn,buf,receiveTime);
        });
}

void ClientServer::Start()
{
    server_.start();
}

void ClientServer::OnConnection(const muduo::net::TcpConnectionPtr& conn)
{
    //可选 打印日志
}

void ClientServer::OnMessage(const muduo::net::TcpConnectionPtr& conn,const std::string& msg,muduo::Timestamp receiveTime)
{
    //以下任务交给线程池做：
    threadPool_->Submit(
        [this,conn,msg](){
            HandleCommand(conn,msg);
        });
}

void ClientServer::HandleCommand(const muduo::net::TcpConnectionPtr& conn,const std::string& msg)
{
    std::string reply;

    //解析msg(command)
    auto commandOpt = ParseCommand(msg);
    if(!commandOpt.has_value())
    {
        spdlog::warn("[ClientServer] invalid command: {}", msg);
        //msg格式错误 无法解析
        reply = "ERROR invalid command";
        return;
    }
    
    auto command = commandOpt.value();
    Command::Type type = command.type;
    if(type == Command::Type::GET)
    {
        //GET：直接使用kvStore查询+返回结果
        auto value = kvStore_->Get(command.key);
        reply = value.has_value() ? value.value() : "nil";
        spdlog::debug("[ClientServer] GET {} -> {}", command.key, reply);
    }
    else if(type == Command::Type::PUT || type == Command::Type::DEL)
    {
        //检查身份
        if(!raftNode_->IsLeader())      //非Leader：拒绝+客户端重定向
        {
            reply = "NOT_LEADER " + std::to_string(raftNode_->GetLeaderId());
            spdlog::debug("[ClientServer] {} rejected: not leader, leaderId={}",
                ((type==Command::Type::PUT)?"PUT":"DEL"), raftNode_->GetLeaderId());
        }
        else        //Leader：需要达成共识再返回结果
        {
            bool ok = raftNode_->Start(command);
            reply = ok ? "OK" : "Timeout";
            spdlog::info("[ClientServer] {} ({},{})-> {}", 
                ((type==Command::Type::PUT)?"PUT":"DEL"), command.key, command.value, reply);
        }
    }
    else
    {
        //非法指令类型
        reply = "ERROR unknown command type";
        spdlog::warn("[ClientServer] Unknow command type: {}", CommandTypeToString(type));
    }


    //send
    codec_.send(conn.get(),reply);
}
