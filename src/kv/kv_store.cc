//kv_store.cc

#include<iostream>
#include<sstream>

#include "kv_store.h"
#include "../common/command.h"


void KVStore::ApplyPut(const std::string& key,const std::string& value)
{
    std::lock_guard<std::shared_mutex> lock(mutex_);        //写独占
    storage_[key]=value;
}

std::optional<std::string> KVStore::Get(const std::string& key) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);       //读共享
    auto it=storage_.find(key);
    if(it==storage_.end()) return std::nullopt;             //表示没有值：optional可以简洁清晰地表达是否有该值，无需`多返回一个bool`或在不存在时不清晰地返回空值`
    return it->second;
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
        case Command::Type::GET:        
            //Get(command.key);
            break;
        case Command::Type::DEL: 
        {
            bool result=ApplyDelete(command.key);
            //if(!result) std::cout<<"This key isn't exist"<<std::endl;
            break;
        }
    }
}


