//rpc_client.cc

#include<sstream>
#include<iostream>

#include "rpc/rpc_client.h"


//!▲ 理解回调函数和unique_ptr?
RpcClient::RpcClient(muduo::net::EventLoop* loop,const std::vector<peerInfo>& peers):
    peers_(),
    loop_(loop)
{
    for(auto& info : peers)
    {
        //创建一个连接管理(unique_ptr?)
        auto pc = std::make_unique<peerConn>();

        //构造TcpClient
        pc->client = std::make_unique<muduo::net::TcpClient>(
            loop, muduo::net::InetAddress(info.host,info.port), "RpcClient-"+std::to_string(info.peerId));

        pc->conn = nullptr;
        
        //! 对比raftNode需要weak_ptr检查是否存活，此处不需要：RpcClient持有shared_ptr(codec和client)，故RpcClient的生命周期更长
        //构造Codec
        pc->codec = std::make_unique<LengthHeaderCodec>(
            [this,peerId = info.peerId](const muduo::net::TcpConnectionPtr& conn, const std::string& msg,muduo::Timestamp receiveTime){
                handleReply(peerId,msg);
            });
        
        //设置回调
        pc->client->setConnectionCallback(
            [this,peerId = info.peerId,pc = pc.get()](const muduo::net::TcpConnectionPtr& conn){                               //?
                OnConnection(peerId,pc,conn);
            });
        pc->client->setMessageCallback(
            [this,pc = pc.get()](const muduo::net::TcpConnectionPtr& conn, muduo::net::Buffer* buf,muduo::Timestamp receiveTime){           //?
                pc->codec->onMessage(conn,buf,receiveTime);
            });
        
        //连接
        pc->client->connect();          //异步
        
        //存入连接池
        peers_[info.peerId] = std::move(pc);
    }
}

RpcClient::~RpcClient()
{
    //在 RpcClient 析构时，muduo 可能还在处理回调 -> 析构时直接断开连接
    if(!stopping_) Stop();

}

void RpcClient::Stop() 
{
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;   // 标记正在停止
    
    for (auto& [id, pc] : peers_) {
        if (pc->client) pc->client->disconnect();
        pc->conn.reset();
    }
    
    pendingVoteCallbacks_.clear();
    pendingAppendCallbacks_.clear();
}

void RpcClient::SendRequestVote(int32_t peerId,const RequestVoteArgs& args,RequestVoteCallback cb)
{
    uint64_t reqId;
    muduo::net::TcpConnectionPtr conn;
    LengthHeaderCodec* codec = nullptr;
    bool ok = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = peers_.find(peerId);                  //! 使用find 避免避免不存在时插入默认值
        if(it!=peers_.end() && it->second->conn)        //! 1.连接存在  2.连接已建立
        {
            reqId = nextReqId_++;
            conn = it->second->conn;                    //! 提前拷贝需要的conn和codec，后续要发送不再需要加锁
            codec = it->second->codec.get();
            pendingVoteCallbacks_[reqId] = {peerId,std::move(cb)};
            ok = true;
        }
    }

    //无效连接
    if(!ok)
    {
        cb(RequestVoteReply());           //! 失败：直接回调 返回一个空的reply
        return;
    }

    //有效连接
    std::string msg = SerializeRequestVoteArgs(reqId,args);
    codec->send(conn.get(),msg);
    
}

void RpcClient::SendAppendEntries(int32_t peerId,const AppendEntriesArgs& args,AppendEntriesCallback cb)
{
    uint64_t reqId;
    muduo::net::TcpConnectionPtr conn;
    LengthHeaderCodec* codec = nullptr;
    bool ok = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = peers_.find(peerId);                  //! 使用find 避免避免不存在时插入默认值
        if(it!=peers_.end() && it->second->conn)        //! 1.连接存在  2.连接已建立
        {
            reqId = nextReqId_++;
            conn = it->second->conn;                    //! 提前拷贝需要的conn和codec，后续要发送不再需要加锁
            codec = it->second->codec.get();
            pendingAppendCallbacks_[reqId] = {peerId,std::move(cb)};     //! move避免拷贝开销
            ok = true;
        }
    }

    //无效连接
    if(!ok)
    {
        cb(AppendEntriesReply());           //! 失败：直接回调 返回一个空的reply
        return;
    }
    
    //有效连接
    std::string msg = SerializeAppendEntriesArgs(reqId,args);
    codec->send(conn.get(),msg);
    
}

//四种可能：1. 连接请求 2. 本端正常关闭 3. 本端异常关闭 4. 对端网络抖动
void RpcClient::OnConnection(int32_t peerId,peerConn* pc,const muduo::net::TcpConnectionPtr& conn)
{
    std::cout << "[RpcClient] peer " << peerId
              << (conn->connected() ? " CONNECTED" : " DISCONNECTED")
              << std::endl;
    if(conn->connected())
    {
        
        std::lock_guard<std::mutex> lock(mutex_);       //! pc->conn 会被SendRequestVote读取：加锁保护
        pc->conn = conn;                                //连接建立：保存连接
    }
    else
    {
        std::vector<RequestVoteCallback> failVoteCallbacks;
        std::vector<AppendEntriesCallback> failAppendCallbacks;
        {    
            std::lock_guard<std::mutex> lock(mutex_);
            pc->conn.reset();                               //连接断开：释放连接(shared_ptr)

            // 如果正在停止，不触发失败回调
            // 因为：RaftNode自身 也快销毁了，触发 cb 没意义且可能出错
            // 而且 Stop() 已经清空了 pendingCallbacks_，这里本来也没东西可收集
            if (stopping_) return;

            // 可选：处理 pendingCallbacks_（触发失败回调）
            for(auto it=pendingVoteCallbacks_.begin();it!=pendingVoteCallbacks_.end();)
            {
                if(it->second.peerId == peerId)
                {
                    failVoteCallbacks.push_back(std::move(it->second.cb));
                    it = pendingVoteCallbacks_.erase(it);
                }
                else
                {
                    it++;
                }
            }
            for(auto it=pendingAppendCallbacks_.begin();it!=pendingAppendCallbacks_.end();)
            {
                if(it->second.peerId == peerId)
                {
                    failAppendCallbacks.push_back(std::move(it->second.cb));
                    it = pendingAppendCallbacks_.erase(it);
                }
                else
                {
                    it++;
                }
            }
        }

        //锁外触发失败回调
        for(auto& cb : failVoteCallbacks) cb(RequestVoteReply());
        for(auto& cb : failAppendCallbacks) cb(AppendEntriesReply());
    }

    
}

void RpcClient::handleReply(int32_t peerId, const std::string& msg)
{
    std::cout << "[RpcClient] reply from peer " << peerId
              << ", raw=" << msg.substr(0, 60) << std::endl;
    std::istringstream iss(msg);
    uint32_t typeNum;
    uint64_t reqId;
    if(!(iss>>typeNum>>reqId)) return;

    RpcMessageType type = static_cast<RpcMessageType>(typeNum);


    //分发消息处理
    switch (type)
    {
        case RpcMessageType::kRequestVoteReply:
        {
            auto replyOpt = DeserializeRequestVoteReply(iss);
            if(!replyOpt.has_value()) return;

            RequestVoteCallback cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);               //!加锁保护临界区 callbacks_
                auto it = pendingVoteCallbacks_.find(reqId);        //!使用find，避免不存在时插入默认值
                if(it==pendingVoteCallbacks_.end()) return;         
                cb=std::move(it->second.cb);
                pendingVoteCallbacks_.erase(it);
            }
            cb(replyOpt.value());                                   //!锁外执行cb

            break;
        }
        case RpcMessageType::kAppendEntriesReply:
        {
            auto replyOpt = DeserializeAppendEntriesReply(iss);
            if(!replyOpt.has_value()) return;

            AppendEntriesCallback cb;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = pendingAppendCallbacks_.find(reqId);
                if(it==pendingAppendCallbacks_.end()) return;
                cb=std::move(it->second.cb);
                pendingAppendCallbacks_.erase(it);
            }
            cb(replyOpt.value());

            break;
        }
            
    default:
        break;
    }

}
/*
回调函数解析：
- 定义：回调 = 把"一段待执行的逻辑"作为参数传给某个函数，让它"在**合适的时机**调用这段逻辑"。
- 结构：bind或者lambda表达式
- lambda表达式：
    - []捕获当前的变量，()声明调用时需要传递的变量，{}定义执行逻辑。
    - !执行逻辑 -> 需求 -> 决定上述两处需要传递的变量
- bind：将函数+参数(+对象)打包，返回一个可调用对象
*/

