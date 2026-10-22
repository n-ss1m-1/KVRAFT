//persister.cc

#include<fstream>
#include<string.h>              //strerror
#include<sys/sendfile.h>
#include<spdlog/spdlog.h>

#include "persist/persister.h"


//初始化文件名+打开文件
Persister::Persister(const std::string& fileName, bool needCreate)
    :fileName_(fileName)
{
    fd_ = needCreate
        ? open(fileName_.c_str(),O_RDWR|O_APPEND|O_CREAT,0644)            // ▲创建需要指定权限0644，否则可能出错
        : open(fileName_.c_str(),O_RDWR|O_APPEND);                        // 用于快照，初始不需要创建空文件       注: 无O_CREAT不需要说明权限
    if(fd_==-1)
    {
        if(!needCreate && errno == ENOENT) return;      // 首次运行，尚无快照：正常 保持fd = -1

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
    const char* buf=data.c_str();
    size_t len = data.size();

    std::lock_guard<std::mutex> lock(mutex_);
    if(fd_ < 0) return std::nullopt;
    
    int64_t startOffset = size_;        // 本次写入的起始位置

    if(!WriteAllLocked(buf, len)) return std::nullopt;

    if(fsync(fd_)<0) 
    {
        spdlog::error("[Persister] AppendToFile: fsync failed, errno={}: {}",errno,strerror(errno));
        return std::nullopt;
    }

    //更新文件大小
    size_ += static_cast<int64_t>(len);

    return startOffset;
}

std::optional<std::vector<int64_t>> Persister::AppendBatchToFile(const std::vector<std::string>& lines)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 空的lines || fd<0 : 直接返回空offsets
    if(fd_ < 0 || lines.empty()) return std::vector<int64_t>{};

    // 初始化offsets、curOffset
    int64_t curOffset = size_;
    std::vector<int64_t> offsets;
    offsets.reserve(lines.size());              // 提前预留空间，避免频繁扩容
    

    // 循环写入文件
    for(const auto& line : lines)
    {
        std::string data = line + "\n";
        const char* buf = data.c_str();
        size_t len = data.size();
        if(!WriteAllLocked(buf, len)) return std::nullopt;
        offsets.push_back(curOffset);
        curOffset += static_cast<int64_t>(len);
    }

    // 组提交(一次)
    if(fsync(fd_)<0) 
    {
        spdlog::error("[Persister] AppendBatchToFile: fsync failed, errno={}: {}",errno,strerror(errno));
        return std::nullopt;
    }

    // 更新文件大小
    size_ = curOffset;

    return offsets;
}

/*
从日志文件中读取 原始行(包含空行) 并返回
怎么进行每行拆解？————使用ifstream和getline
*/
std::vector<std::string> Persister::LoadFromFile()      //▲注：返回值而非引用(cpp会自动移动拷贝)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> lines;

    std::string line;
    std::ifstream in(fileName_);
    while(getline(in,line))         //! getline 会自动去除行尾 '\n'  -> 调用方计算字节偏移需要 +1
    {
        lines.emplace_back(line);       //! 空行也原样返回 -> 方便在RaftStorage中复用
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
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

bool Persister::TruncateFront(int64_t bytesToRemove)
{
    // 1. 加锁
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 2. 参数正确性检查
    if(fd_ < 0) return false;
    if(bytesToRemove <= 0) return true;

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

bool Persister::OverwriteFile(const std::string& content)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. 打开tmp文件
    const std::string tmpPath = fileName_ + ".tmp";
    int tmpFd = open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if(tmpFd < 0)
    {
        spdlog::error("[Persister] OverwriteFile: open snap.bin.tmp failed, errno={}: {}",errno,strerror(errno));
        return false;
    }

    // 2. 循环写 + fsync + close
    size_t remaining = content.size();
    const char* p = content.data();     // const char*居然可以移动: 常量指针--指针地址的值不能改，指针指向可以改
    while(remaining > 0)
    {
        ssize_t n = write(tmpFd, p, remaining);
        if(n < 0)
        {
            if(errno == EINTR) continue;
            spdlog::error("[Persister] OverwriteFile: write failed, errno={}: {}", errno, strerror(errno));
            close(tmpFd);
            unlink(tmpPath.c_str());
            return false;
        }
        remaining -= n;
        p += n;
    }

    if(fsync(tmpFd) < 0)
    {
        spdlog::error("[Persister] OverwriteFile: fsync snap.bin.tmp failed, errno={}: {}",errno,strerror(errno));
        close(tmpFd);
        unlink(tmpPath.c_str());
        return false;
    }
    close(tmpFd);

    // 3. rename
    if(rename(tmpPath.c_str(), fileName_.c_str()) < 0)
    {
        spdlog::error("[Persister] OverwriteFile: rename failed, errno={}: {}", errno, strerror(errno));
        unlink(tmpPath.c_str());    //! unlink: 删除目录项，等待无进程拿着该文件fd，直接删除该文件
        return false;
    }
    // !TODO: rename只保证原子性，不保证持久性 -> 需要fsync父目录，确保rename刷盘

    // 4. ReopenLocked
    if(!ReopenLocked()) 
    {
        return false;
    }
    
    // 5. 更新文件大小
    size_ = static_cast<int64_t>(content.size());

    return true;
}

std::optional<std::string> Persister::ReadwholeFile() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    // !重新打开文件(无需lseek处理偏移量)
    int fd = open(fileName_.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0) 
    {
        return std::nullopt;     // 快照不存在是正常的，不打error
    }

    // 循环读 + close
    std::string buf;
    size_t total = size_;
    size_t offset = 0;
    buf.resize(total);
    while(offset < total)
    {
        ssize_t n = read(fd, buf.data()+offset, total-offset);
        if(n < 0)
        {
            if(errno == EINTR) continue;
            spdlog::error("[Persister] ReadWholeFile: read failed, errno={}: {}", errno, strerror(errno));
            close(fd);
            return std::nullopt;
        }
        if(n == 0) break;
        offset += n;
    }

    close(fd);

    return buf;
}

bool Persister::Exists() const
{
    return access(fileName_.c_str(),F_OK) == 0;
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

bool Persister::WriteAllLocked(const char* buf, size_t len)
{
    size_t remaining = len;
    while(remaining>0)
    {
        ssize_t n = write(fd_,buf,remaining);           //! 需要使用ssize_t：若使用size_t(无符号整型) 收到-1时会变为最大值 -> 无法走到错误处理
        if(n == -1)
        {
            if(errno==EINTR) continue;
            spdlog::error("[Persister] WriteAllLocked: write failed, errno={}: {}",errno,strerror(errno));
            return false;                   //通过返回值告知写入是否成功
        }
        remaining -= n;
        buf += n;                    
    }

    return true;
}