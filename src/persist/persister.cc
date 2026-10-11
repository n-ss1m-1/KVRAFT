//persister.cc

#include<fstream>
#include<string.h>              //strerror
#include <sys/sendfile.h>
#include<spdlog/spdlog.h>

#include "persist/persister.h"


//初始化文件名+打开文件
Persister::Persister(const std::string& fileName)
    :fileName_(fileName)
{
    fd_=open(fileName_.c_str(),O_RDWR|O_APPEND|O_CREAT,0644);           //▲创建需要指定权限0644，否则可能出错
    if(fd_==-1)
    {
        spdlog::error("[Persister] Persister: construct failed, errno={}: {}",errno,strerror(errno));
    }
    else
    {
        struct stat st;
        if(fstat(fd_,&st) < 0)      //! 通过fd读取文件元信息，元信息存于st中，
        {
            spdlog::error("[Persister] Persister: get fileSize failed, errno={}: {}",errno,strerror(errno));
        }
        size_ = static_cast<int64_t>(st.st_size);    //  读取文件元信息：文件字节数 st_size
    }
}

Persister::~Persister()
{
    //析构时其他线程不应该再访问它，不需要加锁
    if(fd_ >= 0)
    {
        fsync(fd_);             //先强制刷新缓冲区 再关闭文件
        close(fd_);
        fd_ = -1;
    }
    
}

/*
将一条指令记录到日志文件
1. 一次写即可完成：O_APPEND保证原子性，不需要互斥锁
2. 多次写才能完成：需要互斥锁+循环写
▲注意每次写日志都需要刷盘，否则突然断电会导致数据丢失
*/
std::optional<int64_t> Persister::AppendToFile(const std::string& line)  
{
    std::string data=line+'\n';                     // !在此处添加'\n'，与下面的同类，而非在其他地方额外处理
    size_t remaining=data.size();
    const char* buf=data.c_str();

    std::lock_guard<std::mutex> lock(mutex_);
    if(fd_ < 0) return std::nullopt;
    
    int64_t startOffset = size_;        // 本次写入的起始位置

    while(remaining>0)
    {
        ssize_t n=write(fd_,buf,remaining);
        if(n==-1)
        {
            if(errno==EINTR) continue;
            spdlog::error("[Persister] AppendToFile: write failed, errno={}: {}",errno,strerror(errno));
            return std::nullopt;                   //通过返回值告知写入是否成功
        }
        remaining-=n;
        buf+=n;                    
    }

    if(fsync(fd_)<0) 
    {
        spdlog::error("[Persister] AppendToFile: fsync failed, errno={}: {}",errno,strerror(errno));
        return std::nullopt;
    }

    //更新文件大小
    size_ += static_cast<int64_t>(data.size());

    return startOffset;
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


bool Persister::Truncate(int64_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if(fd_ < 0) return 0;

    //!将文件截断到size字节
    if(ftruncate(fd_,size) < 0)
    {
        spdlog::error("[Persister] AppendToFile: ftruncate failed, errno={}: {}",errno,strerror(errno));
        return false;
    }
    //!强制把内核缓冲区的数据刷到磁盘
    if(fsync(fd_) < 0)
    {
        spdlog::error("[Persister] AppendToFile: fsync failed, errno={}: {}",errno,strerror(errno));
        return false;
    }

    size_ = size;

    return true;
}

const std::string& Persister::GetFileName() const
{
    return fileName_;
}

int64_t Persister::Size()
{
    return size_;
}

bool Persister::TruncateFront(int64_t bytesToRemove)
{
    // 1. 加锁
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 2. 参数正确性检查
    if(fd_ < 0) return false;
    if(bytesToRemove <=0) return true;

    // 3. 全截断: TruncateLocked
    if(bytesToRemove >= size_)
    {
        return TruncateLocked(0);           //! return之后，后面不再考虑，故后续不需要划分作用域
    }

    // 4. 部分截断【零拷贝】: 把[bytesToRemove, size_]拷贝到临时文件 
    std::string srcPath = fileName_;
    std::string dstPath = srcPath + ".tmp";

    // 4.1 打开源文件和目标文件 -> 获取fd
    int srcFd = open(srcPath.c_str(), O_RDONLY | O_CLOEXEC);
    if(srcFd < 0) 
    {
        spdlog::error("[Persister] open src failed: errno={}: {}", errno, strerror(errno));
        return false;
    }

    int dstFd = open(dstPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if(dstFd < 0) 
    { 
        spdlog::error("[Persister] open dst failed: errno={}: {}", errno, strerror(errno));
        close(srcFd); 
        return false; 
    }

    // 4.2 while sendfile 注意错误处理
    int64_t offset = bytesToRemove;
    int64_t remaining = size_ - bytesToRemove;
    while(remaining > 0)
    {
        ssize_t n = sendfile(dstFd, srcFd, &offset, remaining);     //! #include<sys/sendfile.h>  sendfile(输出目标，输出源，内部自动更改的偏移量，剩余字节数)
        if(n < 0)
        {
            if(errno == EINTR) continue;
            spdlog::error("[Persister] sendfile failed: errno={}: {}", errno, strerror(errno));
            close(srcFd);
            close(dstFd);
            return false;
        }
        if(n == 0) break;       //! 到达EOF
        remaining -= n;
    }
    
    // 刷盘 关闭文件
    if(fsync(dstFd) < 0)
    {
        spdlog::error("[Persister] fsync dst failed: errno={}: {}", errno, strerror(errno));
        close(srcFd);
        close(dstFd);
        return false;
    }
    close(srcFd);
    close(dstFd);

    // 6. 原子替换(rename)
    if(std::rename(dstPath.c_str(),srcPath.c_str()) < 0)
    {
        spdlog::error("[Persister] rename failed: errno={}: {}", errno, strerror(errno));
        return false;
    }

    // 7. 重开fd
    if(!ReopenLocked()) return false;
    
    // 8. 更新文件大小
    size_ -= bytesToRemove;

    return fd_ >= 0;
}

bool Persister::TruncateLocked(int64_t size)
{
    if(fd_ < 0) return false;

    //!将文件截断到size字节
    if(ftruncate(fd_,size) < 0)
    {
        spdlog::error("[Persister] AppendToFile: ftruncate failed, errno={}: {}",errno,strerror(errno));
        return false;
    }
    //!强制把内核缓冲区的数据刷到磁盘
    if(fsync(fd_) < 0)
    {
        spdlog::error("[Persister] AppendToFile: fsync failed, errno={}: {}",errno,strerror(errno));
        return false;
    }

    size_ = size;

    return true;
}

bool Persister::ReopenLocked()
{
    if(fd_ >= 0)
    {
        fsync(fd_);
        close(fd_);
        fd_ = -1;
    }

    fd_ = open(fileName_.c_str(), O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if(fd_ < 0)
    {
        spdlog::error("[Persister] reopen {} failed, errno={}: {}",fileName_,errno,strerror(errno));
        return false;
    }

    return true;
}

