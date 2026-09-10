#include "persister.h"
#include<fstream>


//初始化文件名+打开文件
Persister::Persister(const std::string& fileName)
    :fileName_(fileName)
{
    fd_=open(fileName_.c_str(),O_RDWR|O_APPEND|O_CREAT,0644);           //▲创建需要指定权限0644，否则可能出错
    if(fd_==-1)
    {
        perror("Persister");
        close(fd_);
    }
}

Persister::~Persister()
{
    fsync(fd_);             //先强制刷新缓冲区 再关闭文件
    close(fd_);
}

/*
将一条指令记录到日志文件
1. 一次写即可完成：O_APPEND保证原子性，不需要互斥锁
2. 多次写才能完成：需要互斥锁+循环写
▲注意每次写日志都需要刷盘，否则突然断电会导致数据丢失
*/
bool Persister::AppendToFile(const std::string& line)  
{
    ssize_t remaining=line.size();
    const char* buf=line.c_str();

    std::lock_guard<std::mutex> lock(mutex_);

    while(remaining>0)
    {
        int n=write(fd_,buf,remaining);
        if(n==-1)
        {
            perror("appendToFile:write");
            return false;                   //通过返回值告知写入是否成功
        }
        remaining-=n;
        buf+=n;                    
    }

    if(fsync(fd_)<0) 
    {
        perror("appendToFile:fsync");
        return false;
    }

    return true;
}

/*
从日志文件中读取所有指令并返回
怎么进行每行拆解？————使用ifstream和getline
*/
std::vector<std::string> Persister::LoadFromFile()      //▲注：返回值而非引用(cpp会自动移动拷贝)
{
    std::vector<std::string> lines;

    std::string line;
    std::ifstream in(fileName_);
    while(getline(in,line))
    {
        if(!line.empty()) lines.emplace_back(line);
    }
    return lines;
}
