//kv_store.h

#pragma once
#include<unordered_map>
#include<string>
#include<shared_mutex>
#include<optional>
#include<memory>

struct Command;

class KVStore
{
public:
    KVStore()=default;
    ~KVStore()=default;

    //只写入内存
    void ApplyPut(const std::string& key,const std::string& value);
    std::optional<std::string> Get(const std::string& key) const;
    bool ApplyDelete(const std::string& key);
    void Apply(const Command& command);                         //用于重放日志(是否需要重放GET?)

private:
    std::unordered_map<std::string,std::string> storage_;
    mutable std::shared_mutex mutex_;                                   //const成员函数里修改成员变量->声明为mutable？
};

