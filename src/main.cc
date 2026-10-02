// main.cc
#include<iostream>
#include<algorithm>
#include<cctype>
#include<cstdint>
#include<string>
#include<vector>
#include<csignal>
#include<memory>
#include<thread>
#include<atomic>
#include<chrono>
#include<filesystem>

#include<muduo/net/EventLoop.h>
#include<muduo/base/Logging.h>
#include<spdlog/spdlog.h>
#include<spdlog/sinks/stdout_color_sinks.h>
#include<spdlog/sinks/rotating_file_sink.h>

#include "common/config.h"
#include "kv/kv_store.h"
#include "raft/raft_storage.h"
#include "rpc/rpc_client.h"
#include "raft/raft_node.h"
#include "rpc/rpc_server.h"
#include "common/thread_pool.h"
#include "client_server/client_server.h"

//声明全局EventLoop + 信号处理函数
muduo::net::EventLoop* g_loop = nullptr;


/* ? ok
1. extern "C"
C++ 支持函数重载：void f() 和 void f(int) 可以共存，编译器会把函数名改成 _Z1fv  _Z1fi 这种修饰后的名字
**操作系统的信号注册机制是 C 接口：std::signal 底层是系统 C 库，它只认 C 风格的函数地址
extern "C" 告诉C++ 编译器：这个函数按 C 语言的函数命名规则 编译，不要做 C++ 名字修饰

2.信号处理函数的格式
返回类型必须 void，参数固定 int（收到的信号编号）
函数内部不能调用任何非异步安全函数
*/
extern "C" void OnSignal(int)
{
    if(g_loop) g_loop->quit();
}

// muduo日志输出回调：把muduo的日志字符串交给spdlog打印
void muduoLogForward(const char* msg, int len)
{
    std::string logStr(msg, len);
    // muduo自带完整日志前缀，直接交给spdlog输出，级别统一用info
    spdlog::info("[muduo] {}", logStr);
}


void InitSpdlog(int nodeId)
{
    //设置和创建日志目录
    std::filesystem::create_directories("logs");
    std::string logFile="logs/node"+std::to_string(nodeId)+".log";

    //创建终端sink和文件sink
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::warn);
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logFile,10*1024*1024,3);
    file_sink->set_level(spdlog::level::info);

    //创建logger 绑定上面2个sink
    std::vector<spdlog::sink_ptr> sinks{console_sink,file_sink};
    auto logger = std::make_shared<spdlog::logger>("raft",sinks.begin(),sinks.end());
    
    //设置为全局Logger，后续spdlog::info(msg)就走这个logger
    spdlog::set_default_logger(logger);                         //! spdlog 内部全局持有一份 shared_ptr<logger> 的拷贝 -> logger不会释放 -> logger又持有两个sink -> 两个sink也不会释放

    //设置日志格式
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [t%t] %v");
    
    //设置日志过滤级别
    logger->set_level(spdlog::level::debug);

    //设置刷盘策略
    logger->flush_on(spdlog::level::warn);
    spdlog::flush_every(std::chrono::seconds(1));
}


int main(int argc,char* argv[])         //! char* argv[] 二维数组接收参数
{
    //参数解析(需要输入当前node的ID)
    if(argc!=2)
    {
        std::cerr << "Usage: " << argv[0] << " <nodeId>\n";
        return 1;
    }

    //读取配置
    std::string arg1 = argv[1];
    if(arg1.empty() || !std::all_of(arg1.begin(),arg1.end(),::isdigit))     //参数为空 or 参数含有非数字 -> 返回错误
    {
        std::cerr << "Invalid nodeId\n";
        return 1;
    }
    int32_t nodeId = std::stoi(arg1);                            //! 用户输入 abc，atoi 返回 0 ，实际不是如此   |  atoi：const char* -> int   | stoi：string -> int
    const auto& cluster = config::kCluster;
    if(nodeId<0 || nodeId>=static_cast<int32_t>(cluster.size()))
    {
        std::cerr << "Invalid nodeId: " << nodeId << "\n";
        return 1;
    }

    //初始化spdlog日志
    InitSpdlog(nodeId);

    // 设置muduo日志输出回调，接管muduo所有LOG_*宏
    muduo::Logger::setOutput(muduoLogForward);
    // muduo的flush回调，一般不用处理
    muduo::Logger::setFlush([](){ spdlog::shutdown(); });
    
    //初始化loop + 信号
    muduo::net::EventLoop loop;         //!栈上：安全的，loop生命周期与main函数等同
    g_loop = &loop;
    std::signal(SIGINT,OnSignal);       //Ctrl + C
    std::signal(SIGTERM,OnSignal);      //kill


    //初始化KVStore
    std::shared_ptr<KVStore> kvStore = std::make_shared<KVStore>();

    //初始化RaftStorage + 创建raft日志的文件夹
    std::string dataDir = std::string(config::kDataDirPrefix) + "/node" + std::to_string(nodeId);
    std::filesystem::create_directories(dataDir);
    std::shared_ptr<RaftStorage> storage = std::make_shared<RaftStorage>(dataDir);

    //初始化RpcClient
    std::vector<PeerInfo> peers;
    uint16_t clientServerPort = 0;
    for(const config::NodeConfig& nodeConfig:cluster)
    {
        PeerInfo peer = nodeConfig.peerInfo;
        if(peer.peerId != nodeId)   peers.push_back(peer);        //除了自己以外的连接对象
        else                        clientServerPort = nodeConfig.clientServerPort;
    }
    std::shared_ptr<RpcClient> rpcClient = std::make_shared<RpcClient>(&loop,peers);

    //初始化RaftNode
    std::shared_ptr<RaftNode> raftNode = std::make_shared<RaftNode>(
        nodeId,
        static_cast<int32_t>(cluster.size()),
        rpcClient,
        storage,
        kvStore
    );


    //初始化RpcServer
    int32_t raftPort = cluster[nodeId].peerInfo.port;
    std::shared_ptr<RpcServer> rpcServer = std::make_shared<RpcServer>(&loop,raftPort,raftNode.get());
    rpcServer->Start();

    //初始化线程池
    std::shared_ptr<ThreadPool> threadPool = std::make_shared<ThreadPool>(4);

    //初始化ClientServer + 启动
    std::shared_ptr<ClientServer> clientServer = std::make_shared<ClientServer>(&loop,clientServerPort,kvStore,raftNode,threadPool);
    clientServer->Start();          

    //初始化Tick定时器线程
    std::atomic<bool> running{true};                    //! {} 避免了"most vexing parse"陷阱
    std::thread timerThread([&running, raftNode](){     //atomic不允许拷贝只能引用 | 拷贝raftnode，引用计数+1，保证raftnode比timer活的久
        std::this_thread::sleep_for(std::chrono::seconds(3));           //启动时先睡眠3s，避免未连接到peer导致的term疯涨   TODO：RpcClient成员方法，连接建立后再开始选举(最多等待3s)
        while(running.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            raftNode->Tick();
        }
    });
    
    //打印启动信息
    spdlog::info("============================================");
    spdlog::info("[Main] Node {} started",nodeId);
    spdlog::info("[Main] Raft port: {}, ClientServerPort: {}", raftPort, clientServerPort);
    spdlog::info("[Main] Data dir: {}",dataDir);
    for (const auto& nodeConfig : cluster) 
    {
        const auto& p = nodeConfig.peerInfo;
        if(p.peerId == nodeId)  spdlog::info("[Main]   peer {} -> {}:{}(self)",p.peerId,p.host,p.port);
        else                    spdlog::info("[Main]   peer {} -> {}:{}",p.peerId,p.host,p.port);
    }
    spdlog::info("============================================");

    //运行事件循环
    loop.loop();

    //优雅关闭(! 注意顺序：loop退出循环->停止Tick(停止任务产生)->停止RpcClient(断开连接)->释放组件)
    spdlog::info("[Main] Shutting down...");

    running.store(false);       //! 停止产生新的任务
    timerThread.join(); 

    rpcClient->Stop();          

    clientServer.reset();       //! 先销毁clientServer->不再提交新任务  再销毁线程池->执行完所有任务自动关闭
    threadPool.reset();         // 生命周期：ClientServer < ThreadPool
    
    rpcServer.reset();
    rpcClient.reset();
    raftNode.reset();           //▲! 生命周期要求：RaftNode>RpcServer(RpcServer持有node裸指针) && RaftNode>=RpcClient(RpcClient需要借助node运行回调) && RaftNode>timerThread(timer会一直调用node的成员函数)
    storage.reset();
    kvStore.reset();


    spdlog::shutdown();         //! 手动刷新所有日志，关闭sink，防止日志丢失
    return 0;
}

/*
▲! 注意顺序：
初始化顺序(从最小依赖开始)：
    - 无依赖：EventLoop、KVStore、RaftStorage
    - EventLoop -> RpcClient
    - EventLoop + KVStore + RaftStorage + RaftTransport(RpcClient) -> RaftNode
    - EventLoop + RaftNode -> RpcServer

销毁顺序(停止产生新的任务 -> 等待正在执行的任务完成 -> 释放资源)：


KVStore和RaftStorage在后续的ClientServer和快照中需要使用，所以在main函数创建，同时注入组件是标准写法(方便测试和替换)
*/