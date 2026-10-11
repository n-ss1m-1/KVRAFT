//kv_store.cc

#include<sstream>

#include "kv/kv_store.h"
#include "common/command.h"


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
            break;
        }
    }
}

std::string KVStore::Serialize() const
{
    std::string buf;
    buf.reserve(storage_.size()*32);            //!避免后续反复扩容

    std::shared_lock<std::shared_mutex> lock(mutex_);   
    for(const auto& [k,v]: storage_)
    {
        buf.append(k);
        buf.append("\n");               // !▲ key和value各占一行 -> 保证key和value允许空格存在 -> 同时也比长度头协议更容易解析
        buf.append(v);
        buf.append("\n");
    }
    return buf;
}

void KVStore::Deserialize(const std::string& data)
{
    // 将string转换为输入流
    std::istringstream iss(data);
    std::string key,value;
    std::unordered_map<std::string,std::string> tmp;            // !▲ 先解析到临时表中 最后再move/swap：避免中途解析失败导致数据丢失 + 减小锁的粒度

    // 读取一行数据
    while(std::getline(iss,key) && std::getline(iss,value))
    {
        tmp[key] = std::move(value);
    }

    // 原子替换到真表
    std::lock_guard<std::shared_mutex> lock(mutex_);  
    storage_ = std::move(tmp);
}


