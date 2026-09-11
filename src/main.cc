#include<iostream>
#include<sstream>
#include "kv/kv_store.h"
#include "persist/persister.h"
#include "common/command.h"
#include "network/tcp_server.h"

int main(int argc,char* argv[])
{
    //设置日志输出路径(默认在my_kvraft/data/wal.log)
    std::string logPath="../data/";
    if(argc>1) logPath=argv[1];

    //初始化store和persister
    std::shared_ptr<Persister> persister = std::make_shared<Persister>(logPath+"wal.log");
    std::shared_ptr<KVStore> store = std::make_shared<KVStore>(persister);

    //重放日志到内存中
    store->Load();

    TcpServer server(store);        //▲默认参数都靠右边

    server.Start();

    return 0;
}