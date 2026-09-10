#include "kv_store.h"


void kvStore::put(const std::string& key,const std::string& value)
{
    std::lock_guard<std::shared_mutex> lock(mutex_);        //写独占
    storage_[key]=value;
}

std::optional<std::string> kvStore::get(const std::string& key) 
{
    std::shared_lock<std::shared_mutex> lock(mutex_);       //读共享
    auto it=storage_.find(key);
    if(it==storage_.end()) return std::nullopt;             //表示没有值：optional可以简洁清晰地表达是否有该值，无需`多返回一个bool`或在不存在时不清晰地返回空值`
    return it->second;
}

bool kvStore::remove(const std::string& key)
{
    std::lock_guard<std::shared_mutex> lock(mutex_);      
    return storage_.erase(key);                         //存在：删除成功 返回1； 不存在：不做任何事 返回0
}
