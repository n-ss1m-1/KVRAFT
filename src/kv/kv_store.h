#pragma once
#include<unordered_map>
#include<string>
#include<shared_mutex>
#include<optional>


class kvStore
{
public:
    //kvStore();
    //~kvStore();
    void put(const std::string& key,const std::string& value);
    std::optional<std::string> get(const std::string& key);
    bool remove(const std::string& key);

private:
    std::unordered_map<std::string,std::string> storage_;
    std::shared_mutex mutex_;                                   //const成员函数里修改成员变量->声明为mutable？
};