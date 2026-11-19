// db.cc
#include <unistd.h>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <limits>
#include <utility>
#include <algorithm>
#include <charconv>     // std::from_chars
#include <cstdio>
#include <stdexcept>
#include <errno.h>

#include "db.h"
#include "diagnostics.h"
#include "mem_table.h"
#include "wal.h"
#include "sstable.h"
#include "block_cache.h"


DB::DB(const std::string& directory, const DBOptions& options)
    : options_(options),
      directory_(directory),
      manifestPath_((std::filesystem::path(directory) / "MANIFEST").string()),
      walPath_((std::filesystem::path(directory) / "wal.log").string())        // 自动判断是否有 `/`
{
    // 1. 确认/创建目录  !当目录存在时create_directories()不会做任何事
    std::filesystem::create_directories(directory);                 // 区别于xxdirectory 一个是创建指定目录 一个是递归创建目录

    // 2. 创建MemTable (否则为空指针 无法使用)
    active_.table = std::make_shared<MemTable>();   // .walNumbers在Recover()中确认开启WAL后再恢复

    // 3. 创建BlockCache
    blockCache_ = std::make_shared<BlockCache>(options.BlockCacheCapacity);

    // 4. Recover() + 依据是否开启WAL->创建WAL相应成员
    Recover();             //! 先创建walWriter在第一次会创建一个空的日志文件 -> 混淆 创建新库 和 恢复旧库 
    
    // 5. 启动后台线程
    backgoundThread_ = std::thread([this]{
        this->BackgoundLoop();
    });
}

DB::~DB()
{
    // 加锁：设置停止状态
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        shuttingDown_ = true;
    }

    // 唤醒所有阻塞的线程
    backgroundCv_.notify_all();

    // 关闭后台线程
    if(backgoundThread_.joinable()) LSM_TIME(Join, backgoundThread_.join());

    //! 析构补保证所有数据都刷入SSTable，需要持久化全部数据时：先显式Flush
}

void DB::Put(std::string_view key, std::string_view value) 
{
    LSM_SCOPE(Api);
    lsm::Entry entry;
    entry.key.type = ValueType::kValue;
    entry.key.user_key = key;
    entry.value = value;

    Write(std::move(entry));        // ! 后续不再使用 将占有权交给Write() -> Write()使用值成员接收更好(引用则是表示二者共用该变量)
}

void DB::Delete(std::string_view key) 
{
    LSM_SCOPE(Api);
    lsm::Entry entry;
    entry.key.type = ValueType::kDeletion;
    entry.key.user_key = key;

    Write(std::move(entry));
}

std::optional<std::string> DB::Get(std::string_view key) const 
{
    LSM_SCOPE(Api);
    LSM_LOCK_START(diag_first);
    std::lock_guard<std::mutex> lock(dbMutex_);
    LSM_LOCK_END(diag_first, FirstLock);
    CheckHealthLocked();

    // 查询active
    LookupResult result =  LSM_TIME(ActiveGet, active_.table->Get(publishedSequence_, std::string(key)));            // TODO2
    if(result.state == LookupState::kValue) return result.value;
    if(result.state == LookupState::kDeleted) return std::nullopt;          //! 查到kDeleted：直接返回 而非继续查SSTable

    // 查询immutable(如果存在)
    if(immutable_.has_value())
    {
        result =  LSM_TIME(ImmutableGet, immutable_->table->Get(publishedSequence_, std::string(key)));            // TODO2
        if(result.state == LookupState::kValue) return result.value;
        if(result.state == LookupState::kDeleted) return std::nullopt;     
    }
         
    // 从SSTable中查找(逆向：后面的是更新的)
    for(auto it = tables_.rbegin(); it!=tables_.rend(); it++)
    {
        auto& reader = *it;     // reader: std::unique_ptr<SSTableReader>& 
        lsm_diag::count(lsm_diag::Tables);
        result = reader->Get(publishedSequence_, key);
        if(result.state == LookupState::kValue) return result.value;
        if(result.state == LookupState::kDeleted) return std::nullopt;      //! 查到kDeleted：直接返回 而非继续查更旧的SSTable
    }

    return std::nullopt;
}

void DB::Flush() 
{
    LSM_SCOPE(Api);
    LSM_LOCK_START(diag_first);
    std::unique_lock<std::mutex> lock(dbMutex_);
    LSM_LOCK_END(diag_first, FirstLock);
    CheckHealthLocked();

    const uint64_t targetSequence = publishedSequence_;

    // !等待：调用Flush()时已发布的全部刷盘完成
    while(manifest_.FlushedSequence() < targetSequence)
    {
        LSM_TIME(FlushWait, backgroundCv_.wait(lock, [&](){
            return  manifest_.FlushedSequence() >= targetSequence ||    // 正常结束
                    !immutable_ ||                                      // 可切换 继续刷盘
                    failed_ || backgroundError_ || shuttingDown_;       // 异常结束
        }));

        CheckHealthLocked();
        if(manifest_.FlushedSequence() >= targetSequence) break;
        if(!immutable_ && !active_.table->Empty()) RotateMemTableLocked();      // 非空active才能切换为immutable，避免阈值为0时持续刷盘
    }
}

void DB::Write(lsm::Entry entry) 
{
    LSM_LOCK_START(diag_first);
    std::unique_lock<std::mutex> lock(dbMutex_);
    LSM_LOCK_END(diag_first, FirstLock);
    CheckHealthLocked();
    
    // 1. 检查 active非空(避免阈值=0持续切换空表) && active达到阈值
    while(!active_.table->Empty() && active_.table->ApproximateMemoryUsage() >= options_.memTableSizeLimit)
    {
        // 如果immutable存在 -> 等待刷盘完成，然后需要再次 CheckHealth + check阈值(!因为等待期间其他前台线程可能又更换了active)
        if(immutable_)
        {
            LSM_TIME(Backpressure, backgroundCv_.wait(lock, [&](){
                return  !immutable_ || 
                        failed_ || backgroundError_ || shuttingDown_;
            }));
            CheckHealthLocked();
            continue;
        }
        else  // imuutable不存在 -> 直接切换
        {
            RotateMemTableLocked();    // ! 内部会notify_all() 此处不需要重复唤醒后台线程
        }
    }

    // 2. 分配sequence + 检查entry是否合法
    if(lastSequence_ == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("DB::Write: sequence exhausted");
    entry.key.sequence = lastSequence_ + 1;
    lsm::EncodedEntrySize(entry);           // 检查entry参数是否合法

    // 3. !开启WAL时：预写日志+刷盘
    if(options_.enableWal)
    {
        // !怎么处理error_code：ec非空 -> 标记failed + 抛系统异常
        std::error_code ec = LSM_TIME(Append, walWriter_->Append(entry));
        if(ec)
        {
            failed_ = true;
            backgroundCv_.notify_all();
            throw std::system_error(ec, "DB::Write: WAL append failed");
        }
        ec = LSM_TIME(Sync, walWriter_->Sync());
        if(ec)
        {
            failed_ = true;
            backgroundCv_.notify_all();
            throw std::system_error(ec, "DB::Write: WAL sync failed");
        }
    }

    // 4. 写入 MemTable
    // ! 先写入memTable 再发布sequence：避免发布的sequence读不到
    // ! 此处也需要捕获Add抛出的异常，设置failed_状态，因为插入异常后暂不保证容器可继续使用
    try 
    {
        if(!LSM_TIME(Add, active_.table->Add(entry.key.sequence, entry.key.type, entry.key.user_key, entry.value)))  // TODO1
        {
            failed_ = true;
            backgroundCv_.notify_all();
            throw std::runtime_error("DB::Write: MemTable insert failed");
        } 
    } 
    catch (...) 
    {
        failed_ = true;
        backgroundCv_.notify_all();
        throw;
    }
    

    // 5. 统一发布sequence
    lastSequence_++;
    publishedSequence_ = entry.key.sequence;       

}

void DB::Recover() 
{
    // ! 仅当manifest文件存在时才需要LoadManifest，否则会产生错误(区分首次启动与再次启动)
    // 先恢复Manifest和SSTable 同时更新sequence(sequence崭新程度：Manifest < WAL)
    if(std::filesystem::exists(manifestPath_))
    {
        manifest_ = Manifest::Load(manifestPath_);
        
        for(const auto& meta : manifest_.Tables())
        {
            // ! 先检查SSTable实际大小与meta记录的大小是否匹配 再进行恢复
            std::string path = TablePath(meta.fileNumber);
            if(std::filesystem::file_size(path) != meta.fileSize) throw std::runtime_error("DB::Recover: SSTable file size mismatch");
            tables_.push_back(std::make_unique<SSTableReader>(path, meta.fileNumber, blockCache_));
        }

        lastSequence_ = manifest_.LastSequence();
    }
    
    // 获取下一个可使用的SSTable文件编号
    InitializeNextFileNumber();


    // WAL的恢复前提：开启了WAL
    if(options_.enableWal)
    {
        // !previousSequence：用于保证wal中的sequence单调递增
        // 如果使用lastSequence_进行比较：
        // 如果上次Flush()时 SSTable已保存+MANIFEST已发布+WAL尚未清空 -> 那么此处if(entry.key.sequence <= lastSequence_)会直接抛异常 但其实日志没有错 它只是还没被清理
        // ! 放外层：严格递增需要跨段检查
        uint64_t previousSequence = 0;      

        active_.walNumbers = DiscoverWalFiles();            // 此处确认开启wal后再恢复
        for(size_t i=0; i<active_.walNumbers.size(); i++)
        {
            uint64_t walNumber = active_.walNumbers[i];

            // ! WalReader 只在恢复时使用，可以放在 Recover 的局部作用域中(不需要做为成员变量)
            std::string walPath = WalPath(walNumber);
            WalReader walReader(walPath);

            lsm::Entry entry;
            WalReadStatus status;
            while(WalReadStatus::kRecord == (status = walReader.ReadNext(entry)))
            {
                // ! 检查sequence是否严格递增
                uint64_t sequence = entry.key.sequence;
                if(sequence <= previousSequence) throw std::runtime_error("DB::Recover: WAL sequence out of order");
                previousSequence = sequence;

                lastSequence_ = std::max(lastSequence_, sequence);      // 保证lastSequence_是最新的 max->不能倒退

                if(sequence <= manifest_.FlushedSequence()) continue;        // !针对上面(previousSequence)的异常情况：如果该日志已持久化到SSTable 则不需要重复插入MenTable

                // TODO1
                if(!active_.table->Add(sequence, entry.key.type, entry.key.user_key, entry.value))            
                {
                    throw std::runtime_error("DB::Recover: MemTable insert failed");
                }
                // ! 此处与Write()中的Add()抛异常的处理方式不同：Write()后续db还会被使用，故需要设置failed；此处直接将异常传出，db构造失败，由外部处理异常
            }
            
            // 处理其他错误相关的WalReadStatus   !不设置failed_也没关系 因为直接抛异常 db构造也不会完成 failed状态后续也不会再使用
            switch (status) 
            {
                // ! 最新段可修复，需要截断末尾的半截记录：临时打开 fd 进行截断
                // ! 不是最新段：属于中间格式错误 -> 报错，停止打开，不截断
                case WalReadStatus::kTruncatedTail:     
                {
                    if(i == active_.walNumbers.size() - 1)
                    {
                        int fd = open(walPath.c_str(), O_WRONLY | O_CLOEXEC);
                        if(fd < 0) throw std::system_error(errno, std::generic_category(), "DB::Recover: open wal for truncate failed");
                        if(ftruncate(fd, static_cast<off_t>(walReader.LastValidOffset())) < 0)
                        {
                            int err = errno;
                            close(fd);
                            throw std::system_error(err, std::generic_category(), "DB::Recover: ftruncate wal failed");
                        }
                        while(fsync(fd) < 0)        // 循环重试 避免被EINTR打断
                        {
                            int err = errno;
                            if(err == EINTR) continue;
                            close(fd);
                            throw std::system_error(err, std::generic_category(), "DB::Recover: fsync wal after truncate failed");
                        }

                        // ! 成功路径：也需要关闭该临时fd
                        if(close(fd) < 0) throw std::system_error(errno, std::generic_category(),"DB::Recover: close repaired wal failed");
                    
                        break;
                    }
                    else    // 不是最新段
                    {
                        throw std::runtime_error("DB::Recover: truncated tail on non-active wal segment, corruption");
                    }
                }
                case WalReadStatus::kCorruption:    // 发现格式错误的记录：报错，停止打开，不截断
                {
                    throw std::runtime_error("DB::Recover: wal record corruption, cannot recover");
                }
                case WalReadStatus::kIOError:   // IO系统调用错误：报错，停止打开
                {
                    throw std::runtime_error("DB::Recover: wal read IO error");
                }
                case WalReadStatus::kEndOfFile:     // 正常结束
                {
                    break;
                }
                default:        // 异常的格式
                {
                    throw std::runtime_error("DB::Recover: unknown wal read status");
                }
            }
        }

        // 创建新的WAL
        if(nextWalNumber_ == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("DB::Recover: wal number overflow, cannot allocate new wal segment");
        active_.walNumbers.reserve(active_.walNumbers.size()+1);
        walWriter_ = CreateWal(nextWalNumber_);
        active_.walNumbers.push_back(nextWalNumber_);
        nextWalNumber_++;
    }

    publishedSequence_ = lastSequence_;      // ! publishedSequence_最后再恢复
}

void DB::CheckHealth() const 
{
    // ! 故障状态应该抛异常 而非静默返回
    std::lock_guard<std::mutex> lock(dbMutex_);
    CheckHealthLocked();
}

void DB::InitializeNextFileNumber()  
{
    uint64_t maxNumber = 0;     // 最后再更新nextFileNumber_

    // 1. 找MANIFEST中的最大编号
    for(const auto& meta : manifest_.Tables()) 
    {
        maxNumber = std::max(maxNumber, meta.fileNumber);
    }

    // 2. 遍历目录 从文件名提取编号
    //! C++17没有endwith() 此处手搓一个：用于后缀匹配
    const auto endWith = [](std::string_view text, std::string_view suffix)
    {
        return  text.size() >= suffix.size() && 
                text.substr(text.size()-suffix.size()) == suffix;       //! 注：substr(pos,n)
    };

    for(const auto& entry : std::filesystem::directory_iterator(directory_))
    {
        // entry.path(): /home/truth/db/000008.sst.tmp
        // entry.path().filename(): 000008.sst.tmp
        const std::string fileName = entry.path().filename().string(); 
        
        std::string_view numberText = fileName;
        if(endWith(numberText, ".sst.tmp"))     // 000008.sst.tmp -> 000008
        {
            numberText.remove_suffix(8);                // !移除后缀n个字符
        }
        else if(endWith(numberText, ".sst"))   
        {
            numberText.remove_suffix(4);
        }
        else continue;      // 不是sstable文件

        if(numberText.empty()) continue;
        uint64_t number = 0;
        const char* begin = numberText.data();
        const char* end = begin + numberText.size();
        const auto result = std::from_chars(begin, end, number);    //!作用：将[begin,end)范围内的字符串转换为数字(去除前导0) 由number传出

        // !from_chars() 的结果有两个字段：result.ec(错误码)  result.ptr(解析停止的位置)
        if(result.ptr != end) continue;     // 包含非数字内容
        if(result.ec == std::errc::result_out_of_range) throw std::overflow_error("DB::InitializeNextFileNumber: SSTable file number overflow");
        if(result.ec != std::errc{}) continue;      // 成功时: ec == std::errc{}

        maxNumber = std::max(maxNumber, number);
    }

    // 3. 更新nextFileNumber_
    //避免溢出
    if(maxNumber == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("DB::InitializeNextFileNumber: SSTable file number exhausted");

    nextFileNumber_ = maxNumber + 1;        // ! +1
}

std::string DB::TablePath(uint64_t fileNumber) const 
{
    // 拼接固定格式的SSTable文件名
    char fileName[32];
    std::snprintf(fileName, sizeof(fileName), "%06llu.sst", static_cast<unsigned long long>(fileNumber));
    
    return std::filesystem::path(directory_) / fileName;
}

void DB::SyncDirectory() const 
{
    int dirFd = open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(dirFd < 0) throw std::system_error(errno, std::generic_category(), "DB::SyncDirectory: open parent dir failed");

    while(fsync(dirFd) < 0)
    {
        if(errno == EINTR) continue;
        int err = errno;
        close(dirFd);
        throw std::system_error(err, std::generic_category(), "DB::SyncDirectory: fsync parent dir failed");
    }
    if(close(dirFd) < 0) throw std::system_error(errno, std::generic_category(), "DB::SyncDirectory: close parent dir failed");
}

// void DB::ResetWal() 
// {
//     // 打开 wal.log.tmp 临时文件
//     std::string tmpPath = walPath_ + ".tmp";
//     int tmpFd = open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
//     if(tmpFd < 0) throw std::system_error(errno, std::generic_category(), "DB::ResetWal: open wal.log.tmp failed");

//     // fsync -> close -> rename
//     try 
//     {
//         while(fsync(tmpFd) < 0)
//         {
//             if(errno == EINTR) continue;
//             throw std::system_error(errno, std::generic_category(), "DB::ResetWal: fsync tmpFd failed");
//         }

//         int result = close(tmpFd);
//         tmpFd = -1;
//         if(result < 0) throw std::system_error(errno, std::generic_category(), "DB::ResetWal: close tmpFd failed");

//         if(rename(tmpPath.c_str(), walPath_.c_str()) < 0) throw std::system_error(errno, std::generic_category(), "DB::ResetWal: rename system call failed");
//     } 
//     catch(...) 
//     {
//         if(tmpFd >= 0) close(tmpFd);     // 不能重复关闭相同fd
//         unlink(tmpPath.c_str());      // 删除临时文件  
//         throw;
//     }

//     // 刷新父目录
//     SyncDirectory();

//     // 重新打开walWriter
//     walWriter_ = std::make_unique<WalWriter>(walPath_);
// }

void DB::CheckHealthLocked() const 
{
    if(backgroundError_) std::rethrow_exception(backgroundError_);
    if(failed_) throw std::runtime_error("DB::CheckHealth: DB is in failed state");
    if(shuttingDown_) throw std::runtime_error("DB::CheckHealth: DB is in shuttingdown state");
}

std::string DB::WalPath(uint64_t walNumber) const 
{
    char fileName[32];
    snprintf(fileName, sizeof(fileName), "%06llu.wal", static_cast<unsigned long long>(walNumber));

    return std::filesystem::path(directory_) / fileName;
}

std::vector<uint64_t> DB::DiscoverWalFiles() 
{
    uint64_t maxNumber = 0;
    std::vector<uint64_t> walNumbers;

    const auto endWith = [](std::string_view text, std::string_view suffix)
    {
        return  text.size() >= suffix.size() && 
                text.substr(text.size()-suffix.size()) == suffix;       //! 注：substr(pos,n)
    };

    for(const auto& entry : std::filesystem::directory_iterator(directory_))
    {
        // entry.path(): /home/truth/db/000008.wal
        // entry.path().filename(): 000008.wal
        const std::string fileName = entry.path().filename().string(); 
        
        std::string_view numberText = fileName;
        if(endWith(numberText, ".wal"))     
        {
            numberText.remove_suffix(4);                // !移除后缀n个字符: 000008.wal -> 000008 
        }
        else continue;      // 不是wal文件

        if(numberText.empty()) continue;
        uint64_t number = 0;
        const char* begin = numberText.data();
        const char* end = begin + numberText.size();
        const auto result = std::from_chars(begin, end, number);    //!作用：将[begin,end)范围内的字符串转换为数字(去除前导0) 由number传出

        // !from_chars() 的结果有两个字段：result.ec(错误码)  result.ptr(解析停止的位置)
        if(result.ptr != end) continue;     // 包含非数字内容
        if(result.ec == std::errc::result_out_of_range) throw std::overflow_error("DB::DiscoverWalFiles: wal file number overflow");
        if(result.ec != std::errc{}) continue;      // 成功时: ec == std::errc{}

        maxNumber = std::max(maxNumber, number);
        walNumbers.push_back(number);
    }

    // 3. 更新nextWalNumber_
    //避免溢出
    if(maxNumber == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("DB::DiscoverWalFiles: wal file number exhausted");

    nextWalNumber_ = maxNumber + 1;      

    sort(walNumbers.begin(), walNumbers.end());
    return walNumbers; 
}

std::unique_ptr<WalWriter> DB::CreateWal(uint64_t walNumber) 
{
    LSM_SCOPE(CreateWal);
    // ! 使用新的独占创建文件方式(kCreateNew)
    auto writer =  std::make_unique<WalWriter>(WalPath(walNumber), WalWriterOpenMode::kCreateNew);

    // 同步空文件及数据库目录
    const std::error_code ec = LSM_TIME(NewWalSync, writer->Sync());
    if(ec) throw std::system_error(ec, "DB::CreateWal: sync new WAL failed");

    LSM_TIME(DirectorySync, SyncDirectory());

    return writer;
}

void DB::DeleteObsoleteWals(const std::vector<uint64_t>& walNumbers) 
{
    for(const uint64_t number : walNumbers)
    {
        // ! unlink: 删除文件
        if(unlink(WalPath(number).c_str()) < 0)
        {   
            //! ENOENT: 文件不存在
            if(errno != ENOENT) throw std::system_error(errno, std::generic_category(),"DB::DeleteObsoleteWals: unlink failed");        
        }
    }

    //! 删除文件时 一定要刷新父目录
    SyncDirectory();
}

void DB::BackgoundLoop() 
{
    while(true)         // 不能在这里访问共享变量：shuttingDown_ || failed_ || backgroundError_
    {
        try 
        {
            // 1. 持锁等待! 取任务副本
            MemTableState task;
            {
                LSM_LOCK_START(diag_task);
                std::unique_lock<std::mutex> lock(dbMutex_);        // unique_lock才能配合condition_variable使用
                LSM_LOCK_END(diag_task, TaskLockWait);
                backgroundCv_.wait(lock, [&](){
                    return  immutable_.has_value() || 
                            shuttingDown_ || failed_ || backgroundError_;
                            
                });
                if(shuttingDown_ || failed_ || backgroundError_) return;    //! 锁内访问共享成员：这里判断退出条件
                task = immutable_.value();
            }

            // 2. 解锁执行
            FlushImmutable(task);
        } 
        catch(...) 
        {
            // 3. 捕获失败、持锁保存错误、通知前台线程
            {
                std::lock_guard<std::mutex> lock(dbMutex_);
                backgroundError_ = std::current_exception();        //! 使用 current_exception 避免 make_exception_ptr 传入基类引用(exception&)造成异常切片，保留原始异常类型
            }
            backgroundCv_.notify_all();     //! better: 锁外唤醒
            return;
        }
    }
}

void DB::RotateMemTableLocked() 
{
    LSM_SCOPE(Rotate);
    // 1. 准备阶段
    // 创建新的MemTableState
    MemTableState newState;
    newState.table = std::make_shared<MemTable>();

    // 若开启WAL：检查并分配walNumber + 为newState.walNumbers分配空间 + CreateWal()创建新段(先用变量接收)
    std::unique_ptr<WalWriter> writer;
    if(options_.enableWal)
    {
        if(nextWalNumber_ == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("DB::RotateMemTableLocked: wal number overflow, cannot allocate new wal segment");
        uint64_t number = nextWalNumber_++;
        newState.walNumbers.reserve(1);
        writer = CreateWal(number);
        newState.walNumbers.push_back(number);
    }

    // 2. 提交阶段
    // 冻结active
    active_.table->Freeze();

    // 将active移入immutable
    immutable_ = std::move(active_);
#ifdef LSM_DIAGNOSTICS
    immutable_->diagnosticOrigin = lsm_diag::phase;
#endif

    // 将newState移入active
    active_ = std::move(newState);

    // 若开启WAL：新Writer替换当前Writer
    if(options_.enableWal) walWriter_ = std::move(writer);

    // 新immutable任务已就绪，通知后台线程
    backgroundCv_.notify_all();
}

void DB::FlushImmutable(const MemTableState& state) 
{
#ifdef LSM_DIAGNOSTICS
    lsm_diag::Context taskContext(static_cast<lsm_diag::Phase>(state.diagnosticOrigin), lsm_diag::Background);
    lsm_diag::TaskTrace diagnosticTask;
#endif
    uint64_t fileNumber;
    {
        //! 锁内分配下一个SSTable的编号：现在暂时没有竞争，后续添加的Compaction模块也会合并和生成SSTable->竞争
        LSM_LOCK_START(diag_number);
        std::lock_guard<std::mutex> lock(dbMutex_);
        LSM_LOCK_END(diag_number, FileNumberWait);
        LSM_SCOPE(FileNumberHold);

        // 分配fileNumber后递增 后续出错导致跳号也没关系 Recover()会进行扫描恢复
        if(nextFileNumber_ == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("DB::FlushImmutable: file number exhausted");
        fileNumber = nextFileNumber_++;      
    }

    // 1. 锁外构建
    // build SSTable
    LSM_LOCK_START(diag_build);
    SSTableBuilder builder(state.table->Size());
    uint64_t minSequence = std::numeric_limits<uint64_t>::max(), maxSequence = 0;
    InternalKey smallestKey = state.table->Begin().Key(), largestKey;
    for(auto it = state.table->Begin(); it.Valid(); it.Next())
    {
        minSequence = std::min(minSequence, it.Key().sequence);
        maxSequence = std::max(maxSequence, it.Key().sequence);
        largestKey = it.Key();                                              // 这里的记录方法有没有更好的? (可以使用一个延迟指针来记录前一个位置，暂时没必要增加复杂度)
        builder.Add(lsm::Entry{it.Key(),it.Value()});
    }
    builder.Finish();
    LSM_LOCK_END(diag_build, Build);

    // write SSTable(持久化)
    std::string sstablePath = TablePath(fileNumber);
    uint64_t fileSize = LSM_TIME(Writer, SSTableWriter::Write(sstablePath, builder));

    // build TableMeta
#ifdef LSM_DIAGNOSTICS
    diagnosticTask.file(fileNumber, fileSize);
#endif
    TableMeta meta;
    meta.fileNumber = fileNumber;
    meta.level = 0;
    meta.fileSize = fileSize;               // ! 让SSTableWriter::Write()返回其中的offset 即文件大小 
    meta.minSequence = minSequence;
    meta.maxSequence = maxSequence;
    meta.smallest = std::move(smallestKey);
    meta.largest = std::move(largestKey);

    // 准备新SSTableReader
    std::unique_ptr<SSTableReader> reader = LSM_TIME(ReaderInit, std::make_unique<SSTableReader>(sstablePath, fileNumber, blockCache_));

    // 2. 锁内发布
    {
        LSM_LOCK_START(diag_publish);
        std::lock_guard<std::mutex> lock(dbMutex_);
        LSM_LOCK_END(diag_publish, PublishWait);
        LSM_SCOPE(PublishHold);
        tables_.reserve(tables_.size()+1);              // 提前分配内存 避免Save()后插入失败->状态不一致

        // 构造候选manifest
        Manifest tmpManifest = manifest_;                               //! 直接拷贝manifest_ 然后再进行变更(不容易漏字段  TableMetas也不需要新增方法进行拷贝)
        tmpManifest.SetLastSequence(lastSequence_);     // ! 此处不能使用(旧的)manifest_.LastSequence()  而是使用(新的)当前db的lastSequence_
        tmpManifest.SetFlushedSequence(maxSequence);
        if(manifest_.Generation() == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("DB::FlushImmutable: Manifest generation overflow, cannot increment");
        tmpManifest.SetGeneration(manifest_.Generation()+1);
        tmpManifest.AddTable(std::move(meta));

        // 持久化候选 Manifest(内部会先写tmp 再原子替换)
        LSM_TIME(Manifest, tmpManifest.Save(manifestPath_));

        // 更新内存中的 tables_、发布manifest_
        tables_.push_back(std::move(reader));
        manifest_ = std::move(tmpManifest);    
        
        // 通知前台Flush()
        immutable_.reset();                     // ! state复制的是 shared_ptr 和 walNumbers: 此处不会释放immutable_ 而是在FlushImmutable()结束时再释放
#ifdef LSM_DIAGNOSTICS
        diagnosticTask.release();
#endif
        backgroundCv_.notify_all();
    }

    // 3. 锁外清理
    // 删除immutable_对应的旧WAL日志
    if(options_.enableWal)           // ! 仅开启wal时才使用
    {
        // ! WAL 清理异常单独捕获: 记录清理失败，允许日志残留，不能把已经完成的刷盘改判为失败
        try 
        {
            LSM_TIME(Cleanup, DeleteObsoleteWals(state.walNumbers));       //! BackgroundLoop持有任务副本；即使上面immutable_已移除，仍可读取来源WAL列表
        } 
        catch(const std::exception& e) 
        {
            // WAL清理失败，保留残留日志
            std::fprintf(stderr, "Failed to clean WAL, residual logs remain: %s\n", e.what());
        }
        catch(...)      // 未知异常也捕获 避免将WAL异常视为刷盘失败
        {
            std::fprintf(stderr, "Failed to clean WAL, residual logs remain: unknown exception\n");
        }
    }
    // 构建和发布异常由BackgroundLoop保存，前台在健康检查时重新抛出。
    // WAL清理异常在本函数内记录，不影响已完成的持久化。
}



