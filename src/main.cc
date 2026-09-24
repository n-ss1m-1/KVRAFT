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

#include<muduo/net/EventLoop.h>


#include "common/config.h"
#include "kv/kv_store.h"
#include "raft/raft_storage.h"
#include "rpc/rpc_client.h"
#include "raft/raft_node.h"
#include "rpc/rpc_server.h"

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

    
    //初始化loop + 信号
    muduo::net::EventLoop loop;         //!栈上：安全的，loop生命周期与main函数等同
    g_loop = &loop;
    std::signal(SIGINT,OnSignal);       //Ctrl + C
    std::signal(SIGTERM,OnSignal);      //kill


    //初始化KVStore
    std::shared_ptr<KVStore> kvStore = std::make_shared<KVStore>();

    //初始化RaftStorage
    std::string dataDir = std::string(config::kDataDirPrefix) + "/node" + std::to_string(nodeId);
    std::shared_ptr<RaftStorage> storage = std::make_shared<RaftStorage>(dataDir);

    //初始化RpcClient
    std::vector<peerInfo> peers;
    for(const peerInfo& peer:cluster)
    {
        if(peer.peerId != nodeId) peers.push_back(peer);        //除了自己以外的连接对象
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
    int32_t raftPort = cluster[nodeId].port;
    std::shared_ptr<RpcServer> rpcServer = std::make_shared<RpcServer>(&loop,raftPort,raftNode.get());
    rpcServer->Start();

    //初始化Tick定时器线程
    std::atomic<bool> running{true};                    //! {} 避免了"most vexing parse"陷阱
    std::thread timerThread([&running, raftNode](){     //atomic不允许拷贝只能引用 | 拷贝raftnode，引用计数+1，保证raftnode比timer活的久
        while(running.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(3));           //启动时先睡眠3s，避免未连接到peer导致的term疯涨   TODO：RpcClient成员方法，连接建立后再开始选举(最多等待3s)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            raftNode->Tick();
        }
    });
    
    //打印启动信息
    std::cout << "============================================\n";
    std::cout << "[Main] Node " << nodeId << " started\n";
    std::cout << "[Main] Raft port: " << raftPort << "\n";
    std::cout << "[Main] Data dir:  " << dataDir << "\n";
    for (const auto& p : cluster) {
        std::cout << "[Main]   peer " << p.peerId << " -> "
                  << p.host << ":" << p.port
                  << (p.peerId == nodeId ? " (self)" : "") << "\n";
    }
    std::cout << "============================================\n";

    //运行事件循环
    loop.loop();

    //优雅关闭(! 注意顺序：loop退出循环->停止Tick(停止任务产生)->停止RpcClient(断开连接)->释放组件)
    std::cout << "\n[Main] Shutting down...\n";

    running.store(false);       //! 停止产生新的任务
    timerThread.join(); 

    rpcClient->Stop();          

    rpcServer.reset();
    rpcClient.reset();
    raftNode.reset();           //▲! 生命周期要求：RaftNode>RpcServer(RpcServer持有node裸指针) && RaftNode>=RpcClient(RpcClient需要借助node运行回调) && RaftNode>timerThread(timer会一直调用node的成员函数)
    storage.reset();
    kvStore.reset();

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