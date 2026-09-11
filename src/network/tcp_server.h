#pragma once
#include<string>
#include<memory>

#include "../kv/kv_store.h"
#include "../common/config.h"

class TcpServer
{
public:
    //▲默认参数都靠右边
    TcpServer(std::shared_ptr<KVStore> store,const std::string& ip=SERV_IP,int port=SERV_PORT);
    ~TcpServer();

    void Start();

    void Stop();

    void HandleClient();

private:
    bool started_;
    std::string ip_;
    int port_;
    int listenFd_;
    int connFd_;
    std::shared_ptr<KVStore> store_;            //为什么使用shared_ptr？
};


