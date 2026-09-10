#pragma once
#include<unordered_map>
#include<string>
#include<shared_mutex>
#include<optional>
#include<memory>
#include<iostream>


class Persister;
struct Command;

class KVStore
{
public:
    KVStore()=default;
    KVStore(std::shared_ptr<Persister> persiser);
    ~KVStore()=default;

    //写入日志 + apply
    void Put(const std::string& key,const std::string& value);
    std::optional<std::string> Get(const std::string& key);
    bool Delete(const std::string& key);
    void Execute(const Command& command);                       //读取用户输入并执行

    //只写入内存
    void ApplyPut(const std::string& key,const std::string& value);
    bool ApplyDelete(const std::string& key);
    void Apply(const Command& command);                         //用于重放日志(是否需要重放GET?)

    //重放日志 加载到内存中
    void Load();

private:
    std::unordered_map<std::string,std::string> storage_;
    std::shared_mutex mutex_;                                   //const成员函数里修改成员变量->声明为mutable？
    std::shared_ptr<Persister> persister_;
};