#include<iostream>
#include<sstream>


#include "kv_store.h"
#include "../persist/persister.h"
#include "../common/command.h"



KVStore::KVStore(std::shared_ptr<Persister> persiser):
        persister_(persiser)
{
}


void KVStore::Put(const std::string& key,const std::string& value)
{
    persister_->AppendToFile(std::string("PUT")+" "+key+" "+value+"\n");    //不要忘记换行
    ApplyPut(key,value);
}

std::optional<std::string> KVStore::Get(const std::string& key) 
{
    std::shared_lock<std::shared_mutex> lock(mutex_);       //读共享
    auto it=storage_.find(key);
    if(it==storage_.end()) return std::nullopt;             //表示没有值：optional可以简洁清晰地表达是否有该值，无需`多返回一个bool`或在不存在时不清晰地返回空值`
    return it->second;
}

bool KVStore::Delete(const std::string& key)
{
    persister_->AppendToFile(std::string("DEL")+" "+key+"\n");
    return ApplyDelete(key);
}

void KVStore::Execute(const Command& command)
{
    switch(command.type)
    {
        case Command::Type::PUT: 
        {
            Put(command.key,command.value);
            break;
        }
        case Command::Type::GET: 
        {   
            std::optional<std::string> value=Get(command.key);                          //▲给 case 内部代码加大括号 `{ }`，否则value不允许在此处定义初始化
            if(value.has_value()) std::cout<<"Value = "<<value.value()<<std::endl;
            else                  std::cout<<"This key isn't exist"<<std::endl;
            break;
        }
        case Command::Type::DEL: 
        {    
            bool result=Delete(command.key);
            if(!result) std::cout<<"This key isn't exist"<<std::endl;
            break;
        }
    }
}

void KVStore::Execute(const Command& command,std::string& reply)
{
    switch(command.type)
    {
        case Command::Type::PUT: 
        {
            Put(command.key,command.value);
            reply="PUT success\n";
            break;
        }
        case Command::Type::GET: 
        {   
            std::optional<std::string> value=Get(command.key);                          //▲给 case 内部代码加大括号 `{ }`，否则value不允许在此处定义初始化
            if(value.has_value()) 
            {
                std::ostringstream oss;
                oss<<"GET success: Value = "<<value.value()<<std::endl;
                reply=oss.str();
            }
            else reply="This key isn't exist\n";
            break;
        }
        case Command::Type::DEL: 
        {    
            bool result=Delete(command.key);
            if(!result) reply="This key isn't exist\n";
            else        reply="DELETE success\n";
            break;
        }
    }
}



void KVStore::ApplyPut(const std::string& key,const std::string& value)
{
    std::lock_guard<std::shared_mutex> lock(mutex_);        //写独占
    storage_[key]=value;
}

bool KVStore::ApplyDelete(const std::string& key)
{
    std::lock_guard<std::shared_mutex> lock(mutex_);      
    return storage_.erase(key);                         //存在：删除成功 返回1； 不存在：不做任何事 返回0
}

void KVStore::Apply(const Command& command)
{
    switch(command.type)
    {
        case Command::Type::PUT: 
        {
            ApplyPut(command.key,command.value);
            break;
        }
        case Command::Type::GET:        //重放时不需要GET？
            break;
        case Command::Type::DEL: 
        {
            bool result=ApplyDelete(command.key);
            if(!result) std::cout<<"This key isn't exist"<<std::endl;
            break;
        }
    }
}

void KVStore::Load()
{
    //从文件读取command
    std::vector<std::string> lines = persister_->LoadFromFile();

    //解析command
    std::vector<Command> commands = ParseCommand(lines);

    //重放command(不需要写入日志)
    for(const Command& command : commands) Apply(command);
}


