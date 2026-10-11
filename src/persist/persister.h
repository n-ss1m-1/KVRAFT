//persister.h

#pragma once
#include<string>
#include<fcntl.h>
#include<sys/stat.h>
#include<unistd.h>
#include<stdio.h>
#include<mutex>
#include<vector>
#include<optional>


class Persister
{
public:
    Persister(const std::string& fileName);
    ~Persister();

    //将一条指令记录到日志 (返回起始偏移)
    std::optional<int64_t> AppendToFile(const std::string& line);

    //从日志文件中读取所有指令并返回
    std::vector<std::string> LoadFromFile();

    bool Truncate(int64_t size);                    // ftruncate 到 size字节 + fsync刷盘
    const std::string& GetFileName() const;         // 无锁：RaftStorage在Load时调用

    int64_t Size();                                 // 无锁：获取文件大小 单写者场景下安全 可优化：atomic

    // 截断前面K个字节
    bool TruncateFront(int64_t bytesToRemove);

private:
    // ============= 内部辅助(需要外层调用加锁) ==============
    bool TruncateLocked(int64_t size);
    bool ReopenLocked();

    const std::string fileName_;
    int fd_;
    int64_t size_ = 0;                  //缓存文件大小 (此处不是raftStorage的职责)
    mutable std::mutex mutex_;          //需要在const成员方法中使用
};