#pragma once
#include<string>
#include<fcntl.h>
#include<unistd.h>
#include<mutex>
#include<vector>



class Persister
{
public:
    Persister(const std::string& fileName);
    ~Persister();

    //将一条指令记录到日志
    bool AppendToFile(const std::string& line);

    //从日志文件中读取所有指令并返回
    std::vector<std::string> LoadFromFile();

private:
    const std::string fileName_;
    int fd_;
    std::mutex mutex_;
};