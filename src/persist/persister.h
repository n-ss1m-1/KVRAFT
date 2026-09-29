//persister.h

#pragma once
#include<string>
#include<fcntl.h>
#include<sys/stat.h>
#include<unistd.h>
#include<stdio.h>
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

    int64_t CurrentSize() const;                    // 文件当前的字节数
    bool Truncate(int64_t size);                    // ftruncate 到 size字节 + fsync刷盘
    const std::string& GetFileName() const;         // RaftStorage在Load时调用?

private:
    const std::string fileName_;
    int fd_;
    mutable std::mutex mutex_;          //需要在const成员方法中使用
};