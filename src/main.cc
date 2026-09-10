#include<iostream>
#include<sstream>
#include "kv/kv_store.h"
#include "persist/persister.h"
#include "common/command.h"

int main(int argc,char* argv[])
{
    //设置日志输出路径(默认在my_kvraft/data/wal.log)
    std::string logPath="../data/";
    if(argc>1) logPath=argv[1];

    //初始化store和persister
    KVStore store(std::make_shared<Persister>(logPath+"wal.log"));

    std::string line;
    while(true)
    {
        std::cout<<"> ";    

        if(!std::getline(std::cin,line)) break;         //读取一行输入，若读到EOF或错误则退出
        if(line.empty()) 
        {
            std::cout<<"empty input"<<std::endl;
            continue;
        }
        
        //解析command
        std::optional<Command> command = ParseCommand(line);

        //执行并写入日志
        if(command.has_value()) store.Execute(command.value());
        else std::cout<<"invalid command"<<std::endl;

    }

    return 0;
}