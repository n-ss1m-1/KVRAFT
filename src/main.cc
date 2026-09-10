#include<iostream>
#include<sstream>
#include "kv/kv_store.h"

int main()
{
    kvStore store;
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

        std::istringstream iss(line);                          //使用istringstream来解析输入
        std::string op;
        iss>>op;
        //std::cout<<"op = "<<op<<std::endl;
        
        if(op=="PUT")
        {
            std::string key,value;
            iss>>key>>value;
            store.put(key,value);
            std::cout<<"PUT SUCCESS"<<std::endl;
        }
        else if(op=="GET")
        {
            std::string key;
            iss>>key;
            std::optional<std::string> value=store.get(key);
            if(value.has_value())
            {
                std::cout<<"Found value = "<<value.value()<<" "<<std::endl;
            }
            else 
            {
                std::cout<<"Value not found"<<std::endl;
            }
        }
        else if(op=="DEL")
        {
            std::string key;
            iss>>key;
            if(store.remove(key))
            {
                std::cout<<"Delete success"<<std::endl;
            }
            else
            {
                std::cout<<"This value isn't exist"<<std::endl;
            }
        }
        else
        {
            std::cout<<"无效的operation"<<std::endl;
        }
    }

    return 0;
}