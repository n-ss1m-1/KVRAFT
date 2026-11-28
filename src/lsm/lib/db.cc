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
#include "write_batch.h"
#include "compaction.h"


DB::DB(const std::string& directory, const DBOptions& options)
    : options_(options),
      directory_(directory),
      manifestPath_((std::filesystem::path(directory) / "MANIFEST").string()),
      walPath_((std::filesystem::path(directory) / "wal.log").string())        // 自动判断是否有 `/`
{
    // 0. 检查 option 是否合法
    if(options_.memTableSizeLimit <= 0) throw std::invalid_argument("DB: memTableSizeLimit must be greater than 0");
    if(options_.maxGroupRequests <= 0) throw std::invalid_argument("DB: maxGroupRequests must be greater than 0");
    if(options_.maxGroupBytes <= 0) throw std::invalid_argument("DB: maxGroupBytes must be greater than 0");
    if(options_.targetSSTableDataSize <= 0) throw std::invalid_argument("DB: targetSSTableDataSize must be greater than 0");
    if(options_.targetSSTableDataSize / lsm::kWalMinEntrySize + 1 /*expectedKeyCount*/ > std::numeric_limits<uint32_t>::max() / 10) throw std::invalid_argument("DB: targetSSTableDataSize exceeds Bloom capacity");
    if(options_.maxLevels <= 0) throw std::invalid_argument("DB: maxLevels must be greater than 0");
    if(options_.maxCompactionSourceBytes <= 0) throw std::invalid_argument("DB: maxCompactionSourceBytes must be greater than 0");
    if(options_.level0CompactionTrigger <= 0) throw std::invalid_argument("DB: level0CompactionTrigger must be greater than 0");
    if(options_.level1TargetBytes <= 0) throw std::invalid_argument("DB: level1TargetBytes must be greater than 0");
    if(options_.levelSizeMultiplier < 2) throw std::invalid_argument("DB: levelSizeMultiplier must be at least 2");
    if(options_.maxCompactionSourceExpansionFactor <= 0) throw std::invalid_argument("DB: maxCompactionSourceExpansionFactor must be greater than 0");

    // 1. 确认/创建目录  !当目录存在时create_directories()不会做任何事
    std::filesystem::create_directories(directory);                 // 区别于xxdirectory 一个是创建指定目录 一个是递归创建目录

    // 2. 创建MemTable (否则为空指针 无法使用)
    active_.table = std::make_shared<MemTable>();   // .walNumbers在Recover()中确认开启WAL后再恢复

    // 3. 创建BlockCache
    blockCache_ = std::make_shared<BlockCache>(options.blockCacheCapacity);

    // 4. Recover() + 依据是否开启WAL->创建WAL相应成员
    Recover();             //! 先创建walWriter在第一次会创建一个空的日志文件 -> 混淆 创建新库 和 恢复旧库 
    
    // 5. 启动后台线程(flush 和 compaction)
    try 
    {
        flushThread_ = std::thread([this]{ this->FlushLoop(); });
        compactionThread_ = std::thread([this]{ this->CompactionLoop(); });
    } 
    catch(...)      // ! 避免flush启动成功+compaction启动失败 -> DB构造失败 -> 不析构 -> 无法回收线程
    {
        // 加锁：设置停止状态
        {
            std::lock_guard<std::mutex> lock(dbMutex_);
            shuttingDown_ = true;
        }

        // 唤醒所有阻塞的线程
        backgroundCv_.notify_all();

        // 关闭后台线程
        if(flushThread_.joinable()) LSM_TIME(Join, flushThread_.join());
        if(compactionThread_.joinable()) LSM_TIME(Join, compactionThread_.join());

        throw;
    }
    
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
    if(flushThread_.joinable()) LSM_TIME(Join, flushThread_.join());
    if(compactionThread_.joinable()) LSM_TIME(Join, compactionThread_.join());

    //! 析构补保证所有数据都刷入SSTable，需要持久化全部数据时：先显式Flush
}

void DB::Put(std::string_view key, std::string_view value) 
{
    LSM_SCOPE(Api);
    WriteBatch batch;
    batch.Put(key, value);

    Write(batch);        //entry: ! 后续不再使用 将占有权交给Write() -> Write()使用值成员接收更好(引用则是表示二者共用该变量)
}

void DB::Delete(std::string_view key) 
{
    LSM_SCOPE(Api);
    WriteBatch batch;
    batch.Delete(key);

    Write(batch);
}

std::optional<std::string> DB::Get(std::string_view key) const 
{
    LSM_SCOPE(Api);
    LSM_LOCK_START(diag_first);
    std::lock_guard<std::mutex> lock(dbMutex_);
    LSM_LOCK_END(diag_first, FirstLock);
    CheckHealthLocked();

    // 1. 查询active
    LookupResult result =  LSM_TIME(ActiveGet, active_.table->Get(publishedSequence_, std::string(key)));            // TODO2
    if(result.state == LookupState::kValue) return result.value;
    if(result.state == LookupState::kDeleted) return std::nullopt;          //! 查到kDeleted：直接返回 而非继续查SSTable

    // 2. 查询immutable(如果存在)
    if(immutable_.has_value())
    {
        result =  LSM_TIME(ImmutableGet, immutable_->table->Get(publishedSequence_, std::string(key)));            // TODO2
        if(result.state == LookupState::kValue) return result.value;
        if(result.state == LookupState::kDeleted) return std::nullopt;     
    }
         
    // 3. 从SSTable中查找
    // 3.1 L0：逆向查找(后面的是更新的) + meta范围检查
    for(auto it = levels_[0].rbegin(); it!=levels_[0].rend(); it++)
    {
        const auto& meta = it->meta;
        if(key < meta.smallest.user_key || meta.largest.user_key < key) continue;   // ! 先进行范围检查，target不在此范围直接跳过

        const auto& reader = it->reader;     
        lsm_diag::count(lsm_diag::Tables);
        result = reader->Get(publishedSequence_, key);
        if(result.state == LookupState::kValue) return result.value;
        if(result.state == LookupState::kDeleted) return std::nullopt;      //! 查到kDeleted：直接返回 而非继续查更旧的SSTable
    }
    // 3.2 L1+：meta范围 -> 二分查找定位
    for(size_t i=1; i<levels_.size(); i++)
    {
        const auto& level = levels_[i];

        // 找到第一张 largest >= key 的表
        auto it = std::lower_bound(level.begin(), level.end(), key,
        [](const SSTableState& table, std::string_view target)
            {
                return table.meta.largest.user_key < target;
            });
        // 该层不存在该key or 该key落在表的空隙中
        if(it == level.end() || key < it->meta.smallest.user_key) continue;  
        // 找到了目标sstable
        lsm_diag::count(lsm_diag::Tables);
        result = it->reader->Get(publishedSequence_, key);
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

    // 自动检查是否需要Compaction
    backgroundCv_.notify_all();
}

void DB::CompactLevel(uint32_t sourceLevel) 
{
    std::unique_lock<std::mutex> lock(dbMutex_);
    CheckHealthLocked();

    // 检查sourceLevel是否合法
    if(sourceLevel >= levels_.size() - 1) throw std::invalid_argument("DB::CompactLevel: sourceLevel has no next level");
    // ! 等待空闲的任务栏位(避免任务覆盖 暂时不需要任务队列)
    backgroundCv_.wait(lock, [&](){
        return  compactionRequested_ == compactionCompleted_ ||
                failed_ || backgroundError_ || shuttingDown_;
    });
    CheckHealthLocked();

    if(compactionRequested_ == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("DB::CompactLevel: compactionRequested exhausted");
    uint64_t myRequest = ++compactionRequested_;
    sourceLevel_ = sourceLevel;

    backgroundCv_.notify_all();
    backgroundCv_.wait(lock, [&](){
        return  compactionCompleted_ >= myRequest ||
                failed_ || backgroundError_ || shuttingDown_;
    });

    CheckHealthLocked();
}

void DB::Recover() 
{
    // ! 仅当manifest文件存在时才需要LoadManifest，否则会产生错误(区分首次启动与再次启动)
    // 先恢复Manifest和SSTable 同时更新sequence(sequence崭新程度：Manifest < WAL)
    levels_.resize(options_.maxLevels);     // levels_初始化一步扩容到位
    if(std::filesystem::exists(manifestPath_))
    {
        manifest_ = Manifest::Load(manifestPath_);
        
        for(const auto& meta : manifest_.Tables())
        {
            // ! 先检查SSTable实际大小与meta记录的大小是否匹配 再进行恢复
            std::string path = TablePath(meta.fileNumber);
            if(std::filesystem::file_size(path) != meta.fileSize) throw std::runtime_error("DB::Recover: SSTable file size mismatch");
            if(meta.level >= options_.maxLevels) throw std::runtime_error("DB::Recover: manifest table level exceeds supported limit");
            levels_[meta.level].push_back(SSTableState{meta, std::make_shared<SSTableReader>(path, meta.fileNumber, blockCache_)});
        }

        // L1+：排序 + 相邻表范围严格不重叠
        for(size_t l = 1; l<levels_.size(); l++) 
        {
            // 排序
            auto& level = levels_[l];
            std::sort(level.begin(), level.end(), 
            [](const SSTableState& a, const SSTableState& b)
            {
                return a.meta.smallest.user_key < b.meta.smallest.user_key; 
            });

            // 检查相邻表范围严格不重叠
            for(size_t i=1; i<level.size(); i++)
            {
                if(level[i-1].meta.largest.user_key >= level[i].meta.smallest.user_key)
                    throw std::runtime_error("DB::Recover: overlapping SSTables in level");
            }
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

            std::vector<lsm::Entry> entries;
            lsm::Entry entry;
            WalReadStatus status;
            while(WalReadStatus::kRecord == (status = walReader.ReadNext(entries)))
            {
                for(const auto& entry : entries)
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

void DB::CheckHealthLocked() const 
{
    CheckFailureLocked();
    if(shuttingDown_) throw std::runtime_error("DB::CheckHealth: DB is in shuttingdown state");
}

void DB::CheckFailureLocked() const 
{
    if(backgroundError_) std::rethrow_exception(backgroundError_);
    if(failed_) throw std::runtime_error("DB::CheckHealth: DB is in failed state");
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

void DB::FlushLoop() 
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
                if(immutable_.has_value()) task = immutable_.value();
            }

            // 2. 解锁执行  ! 注意这里不能再访问immutable_
            FlushImmutable(task);
        } 
        catch(...) 
        {
            // 3. 捕获失败、持锁保存错误、通知前台线程
            {
                std::lock_guard<std::mutex> lock(dbMutex_);
                //! 使用 current_exception 避免 make_exception_ptr 传入基类引用(exception&)造成异常切片，保留原始异常类型
                //! 双后台线程：仅保留第一个后台异常
                if(!backgroundError_) backgroundError_ = std::current_exception();        
            }
            backgroundCv_.notify_all();     //! better: 锁外唤醒
            return;
        }
    }
}

void DB::CompactionLoop() 
{
    while(true)         // 不能在这里访问共享变量：shuttingDown_ || failed_ || backgroundError_
    {
        try 
        {
            // 1. 持锁等待! 取任务副本
            uint64_t taskRequest = 0;
            uint32_t sourceLevel = 0;
            {
                LSM_LOCK_START(diag_task);
                std::unique_lock<std::mutex> lock(dbMutex_);        // unique_lock才能配合condition_variable使用
                LSM_LOCK_END(diag_task, TaskLockWait);
                backgroundCv_.wait(lock, [&](){
                    return  shuttingDown_ || failed_ || backgroundError_ ||
                            compactionRequested_ > compactionCompleted_ ||
                            PickAutoCompactionLevelLocked().has_value();
                });
                if(shuttingDown_ || failed_ || backgroundError_) return;    //! 锁内访问共享成员：这里判断退出条件
                if(compactionRequested_ > compactionCompleted_) 
                {
                    taskRequest = compactionRequested_;
                    sourceLevel = sourceLevel_;
                }
                else
                {
                    taskRequest = 0;                    // ! taskRequest = 0 表示自动任务
                    const auto selected = PickAutoCompactionLevelLocked();
                    sourceLevel = selected.value();
                }
            }

            // 2. 解锁执行  ! 注意这里不能再访问immutable_、compactionRequested_、compactionCompleted_
            CompactTables(taskRequest, sourceLevel);
        } 
        catch(...) 
        {
            // 3. 捕获失败、持锁保存错误、通知前台线程
            {
                std::lock_guard<std::mutex> lock(dbMutex_);
                //! 使用 current_exception 避免 make_exception_ptr 传入基类引用(exception&)造成异常切片，保留原始异常类型
                //! 双后台线程：仅保留第一个后台异常
                if(!backgroundError_) backgroundError_ = std::current_exception();        
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

    // 准备新SSTableState
    SSTableState newSSTable;
    newSSTable.meta = std::move(meta);
    newSSTable.reader = LSM_TIME(ReaderInit, std::make_shared<SSTableReader>(sstablePath, fileNumber, blockCache_));

    // 2. 锁内发布
    {
        LSM_LOCK_START(diag_publish);
        std::lock_guard<std::mutex> lock(dbMutex_);
        LSM_LOCK_END(diag_publish, PublishWait);
        LSM_SCOPE(PublishHold);
        CheckFailureLocked();               // ! 发布前检查db是否故障 避免另一个线程发生了故障 而此处又覆盖失败后尚不确定的磁盘状态
        levels_[0].reserve(levels_[0].size()+1);              // 提前分配内存 避免Save()后插入失败->状态不一致

        // 构造候选manifest
        Manifest tmpManifest = manifest_;                               //! 直接拷贝manifest_ 然后再进行变更(不容易漏字段  TableMetas也不需要新增方法进行拷贝)
        tmpManifest.SetLastSequence(lastSequence_);     // ! 此处不能使用(旧的)manifest_.LastSequence()  而是使用(新的)当前db的lastSequence_
        tmpManifest.SetFlushedSequence(maxSequence);
        if(manifest_.Generation() == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("DB::FlushImmutable: Manifest generation overflow, cannot increment");
        tmpManifest.SetGeneration(manifest_.Generation()+1);
        tmpManifest.AddTable(newSSTable.meta);

        // 持久化候选 Manifest(内部会先写tmp 再原子替换)
        try
        {
            LSM_TIME(Manifest, tmpManifest.Save(manifestPath_));
        }
        catch (...)     
        {
            if (!backgroundError_)
                backgroundError_ = std::current_exception();

            backgroundCv_.notify_all();
            throw;
        }
        /* !
        必须在持有dbMutex_时设置error，否则可能发生：
        1.刷盘线程：Save 抛异常 → 发布锁释放
        2.合并线程：取得发布锁 → 故障尚未登记 → 检查通过并发布
        3.刷盘线程：进入循环 catch → 才登记故障
        */

        // 更新内存中的 levels_[0]、发布manifest_
        levels_[0].push_back(std::move(newSSTable));
        manifest_ = std::move(tmpManifest);    
        
        // 通知前台Flush() + 后台Compaction自动检查
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

void DB::Write(WriteBatch& batch)
{
    // 入队前先预留空间，避免pending插入后分配内存失败，导致数据残留的不一致
    std::vector<PendingWrite*> group;
    group.reserve(options_.maxGroupRequests);

    // 1. 在栈上创建 PendingWrite
    PendingWrite pending;
    pending.batch = &batch;
    pending.estimatedBytes = batch.EstimatedWalRecordSize();

    // 2. 获取队列锁，将节点加入队尾
    std::unique_lock<std::mutex> writeQueueLock(writeQueueMutex_);
    writeQueue_.push_back(&pending);

    // 3. 等待：自己已完成 || 自己成为队首
    // ! 不检查error，避免请求提前返回、销毁pending，导致后续组长访问悬空指针
    pending.cv.wait(writeQueueLock, [&,this](){
        return pending.done || writeQueue_.front() == &pending;
    });

    // 3.1 已完成：组员检查是否出错(向外抛出)
    if(pending.done)
    {
        if(pending.error) std::rethrow_exception(pending.error);
        return;         // 无异常：返回
    }

    // 3.2.1 成为组长: 选定本组候选请求(WriteGroup()内校验是否合法)
    size_t groupBytes = 0;
    for(auto request : writeQueue_)
    {
        if(group.size() >= options_.maxGroupRequests) break;       // ! 设置数量上限

        // 先插入首个请求，然后限制写入字节预算
        const size_t requestBytes = request->estimatedBytes;
        if(!group.empty() &&
           (groupBytes >= options_.maxGroupBytes ||                        // ! 第一个条件 避免：首个请求允许超过预算，之后计算如果只用第二个条件，则右边的减法会下溢回绕(->max)，导致之后该检查无效
            requestBytes > options_.maxGroupBytes - groupBytes)) break;
        group.push_back(request);
        groupBytes += requestBytes;
    }

    // 3.2.2 释放队列锁：WriteGroup期间可新增PendingWrite到队列中
    writeQueueLock.unlock();

    // 3.2.3 WriteGroup(): 校验 + 实际执行组提交
    // ! group 部分在执行期间由组长独占处理 -> 不需要加 writeQueueMutex_ 锁
    std::exception_ptr groupError;
    try
    {
        WriteGroup(group);
    }
    catch(...)
    {
        // ! 此处只需要保存异常，然后继续执行下面的队列收尾，不能重复抛出
        // 避免其他线程会一直等待该组从队列移除，但是一直等不到
        groupError = std::current_exception();
    }

    // 3.2.4 再次加锁(使用writeQueue_?)
    writeQueueLock.lock();

    // 通知本组 batch 检查结果
    for(size_t i=0; i<group.size(); i++)
    {
        if(!group[i]->error && groupError) group[i]->error = groupError;
        writeQueue_.pop_front();
        group[i]->done = true;
        group[i]->cv.notify_one();         // ! 锁内唤醒是为了避免：解锁后组员提前苏醒取得锁 -> 发现done=true -> 提前返回、销毁pending -> 组长悬空访问pending
    }
    if(!writeQueue_.empty()) writeQueue_.front()->cv.notify_one();      // ! 通知新组长交接

    // ! 组长也需要报告错误
    if(pending.error) std::rethrow_exception(pending.error);
    //writeQueueLock.unlock();      // 自动析构解锁
}

// ! group 部分在执行期间由组长独占处理 -> 不需要加 writeQueueMutex_ 锁
void DB::WriteGroup(const std::vector<PendingWrite*>& group)
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

    // 2. 检查batch是否合法 + 检查sequence容量 + 分配sequence
    uint64_t lastPlannedSequence = lastSequence_;
    for(auto p : group)
    {
        try
        {
            WriteBatch& batch = *(p->batch);
            if(batch.Empty()) throw std::invalid_argument("DB::Write: empty batch");
            if(batch.Count() > std::numeric_limits<uint64_t>::max() - lastPlannedSequence) throw std::overflow_error("DB::Write: sequence exhausted");
            for(size_t i=0; i< batch.Count(); i++)
            {
                lsm::Entry& entry = batch.operations_[i];
                entry.key.sequence = lastPlannedSequence + i + 1;
            }
            // !同时检查批次和entry是否合法，相比原来在循环中lsm::EncodeEntrySize()更严谨
            // !这里检查是为了避免：关闭WAL -> 不执行walWriter_->Append -> 未检查 -> Memtable写入非法或超大批次
            lsm::EncodeBatchPayloadSize(batch.operations_);

            lastPlannedSequence += batch.Count();           // 逐batch推进预计的lastSequence (! 如果上面抛出错误 则不会推进游标)
        }
        catch(...)
        {
            // ! batch检查出错：不认为DB故障 -> 不把异常向外抛出
            // 保存该batch的错误
            p->error = std::current_exception();
        }
    }
    if(lastPlannedSequence == lastSequence_) return;        // 没有合法的 batch 直接返回即可

    // ! 处理抛出的异常：walWriter_->Append、walWriter_->Sync、active_.table->Add
    try
    {
        // 3. !开启WAL时：预写日志+刷盘
        if(options_.enableWal)
        {
            // !怎么处理error_code：ec非空 -> 标记failed + 抛系统异常
            std::error_code ec;
            for(auto p : group)
            {
                if(p->error) continue;                      // ! 如果上面检查发现 batch 格式错误，则跳过写入流程
                auto& batch = *(p->batch);
                ec = LSM_TIME(Append, walWriter_->Append(batch.operations_));
                if(ec)
                {
                    failed_ = true;
                    backgroundCv_.notify_all();
                    throw std::system_error(ec, "DB::Write: WAL append failed");
                }
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
        for(auto p : group)
        {
            if(p->error) continue;                      // ! 如果上面检查发现 batch 格式错误，则跳过写入流程
            auto& batch = *(p->batch);
            for(const auto& entry : batch.operations_)
            {
                if(!LSM_TIME(Add, active_.table->Add(entry.key.sequence, entry.key.type, entry.key.user_key, entry.value)))  // TODO1
                {
                    failed_ = true;
                    backgroundCv_.notify_all();
                    throw std::runtime_error("DB::Write: MemTable insert failed");
                }
            }
        }
    }
    catch(...)
    {
        failed_ = true;
        backgroundCv_.notify_all();
        throw;
    }

    // 5. 统一发布sequence
    publishedSequence_ = lastSequence_ = lastPlannedSequence;
}
std::vector<TableMeta> DB::BuildCompactionOutputs(const std::vector<const SSTableReader*>& tables, uint32_t targetLevel)
{
    // 1. 声明builder、currentDataBytes、outputs、currentMeta
    // ! 以最多记录的情况(兜底)，保证过滤的效果，而且默认4MB目标 Bloom需要占用300kb；后续可以优化为流式Write统计精确的entry数 or 使用分块过滤器
    size_t expectedKeyCount = options_.targetSSTableDataSize / lsm::kWalMinEntrySize + 1;
    std::optional<SSTableBuilder> builder;      // 达标时写入并reset 下一轮重新构造
    size_t currentDataBytes = 0;

    std::vector<TableMeta> outputs;
    std::optional<TableMeta> currentMeta;

    // 2. 合并
    MergeTables(tables, [&](const lsm::Entry& entry){
        // 构造
        if(!builder.has_value()) builder.emplace(expectedKeyCount);
        if(!currentMeta.has_value())
        {
            currentMeta.emplace();
            currentMeta->level = targetLevel;
            currentMeta->minSequence = currentMeta->maxSequence = entry.key.sequence;   // ! 这里[min/max]sequence、smallest、largest必须用第一条entry初始化，否则最小 sequence 和键边界可能保留错误的默认值
            currentMeta->smallest = currentMeta->largest = entry.key;
            std::lock_guard<std::mutex> lock(dbMutex_);
            if(nextFileNumber_ == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("DB::BuildCompactionOutputs: file number exhausted");
            currentMeta->fileNumber = nextFileNumber_++;    // 临时持锁 分配sst编号
        }

        // 计算databytes、向builder插入entry、更新meta
        const size_t entryBytes =  lsm::EncodedEntrySize(entry);
        if(entryBytes > std::numeric_limits<size_t>::max() - currentDataBytes) throw std::length_error("compaction output size overflow");
        currentDataBytes += entryBytes;

        builder->Add(entry);

        currentMeta->minSequence = std::min(currentMeta->minSequence, entry.key.sequence);
        currentMeta->maxSequence = std::max(currentMeta->maxSequence, entry.key.sequence);
        currentMeta->largest = entry.key;       // MergeTales已按InternalKey递增输出：smallest=第一条即可 largest=最后一条即可

        // 判断是否达到builder写入阈值：达到则写入sstable、记录meta、更新databytes
        if(currentDataBytes >= options_.targetSSTableDataSize)
        {
            builder->Finish();                                      // 必须先Finish() 否则Writer会报错
            currentMeta->fileSize = SSTableWriter::Write(TablePath(currentMeta->fileNumber), builder.value());      // meta必须使用Writer返回的实际文件大小，因为currentDataBytes不包含Bloom、Index、Footer、CRC的大小
            builder.reset();

            outputs.push_back(currentMeta.value());
            currentMeta.reset();

            currentDataBytes = 0;
        }
    });

    // ! 3. 处理最后一张 非空 && 未达到阈值的SSTable
    if(builder.has_value() && currentMeta.has_value())
    {
        builder->Finish();
        currentMeta->fileSize = SSTableWriter::Write(TablePath(currentMeta->fileNumber), builder.value());
        outputs.push_back(currentMeta.value());
    }

    return outputs;
}

void DB::CompactTables(uint64_t taskRequest, uint32_t sourceLevel)
{
    // 1. 锁内：选定输入的SSTable
    LevelTables sourceInputs;
    LevelTables targetInputs;
    std::vector<const SSTableReader*> inputs;
    std::vector<TableMeta> inputMetas;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);

        const auto& source = levels_[sourceLevel];
        const auto& target = levels_[sourceLevel + 1];

        if(source.empty())        // ! 无需合并也要完成本次请求(1张表也需要下推与目标层合并)
        {
            if(taskRequest != 0) compactionCompleted_ = taskRequest;        // 自动任务不改变compactionCompleted_
            backgroundCv_.notify_all();
            return;
        }

        // ! 锁外声明 锁内复制
        std::string smallest=source[0].meta.smallest.user_key, largest=source[0].meta.largest.user_key;
        size_t sourceBytes = 0;
        // 根据原预算选择基础源前缀
        for(const auto& state : source)
        {
            const uint64_t fileBytes = state.meta.fileSize;
            if(!sourceInputs.empty() && 
              (sourceBytes >= options_.maxCompactionSourceBytes || 
                fileBytes > options_.maxCompactionSourceBytes - sourceBytes)) break;
    
            sourceInputs.push_back(state);
            smallest = std::min(smallest, state.meta.smallest.user_key);    // 确定sourceInputs的范围
            largest = std::max(largest, state.meta.largest.user_key);
            sourceBytes += state.meta.fileSize;
        }
        // 源输入范围 -> 目标输入
        for(const auto& state : target)
        {
            if(state.meta.smallest.user_key <= largest && smallest <= state.meta.largest.user_key)
                targetInputs.push_back(state);
        }
        // 目标输入不为空 -> 尝试拓展源前缀(!前提：源字节不超过拓展上限 + 目标文件集合保持不变)
        if(!targetInputs.empty())
        {
            const size_t limit = std::numeric_limits<size_t>::max();
            const size_t factor = options_.maxCompactionSourceExpansionFactor;
            const size_t budget = options_.maxCompactionSourceBytes;
            const size_t expandedSourceLimit = budget > limit / factor ? limit : budget * factor;

            for(size_t i=sourceInputs.size(); i<source.size(); i++)
            {
                const auto& sourceState = source[i];
                const uint64_t fileBytes = sourceState.meta.fileSize;

                // 检查拓展后的字节预算是否充足(避免溢出版)
                if(sourceBytes >= expandedSourceLimit || fileBytes > expandedSourceLimit - sourceBytes) break;

                // 添加新的源表后的候选范围
                const std::string candidateSmallest = std::min(smallest, sourceState.meta.smallest.user_key);
                const std::string candidateLargest = std::max(largest, sourceState.meta.largest.user_key);

                // ! 预设加入当前源表后 sourceInput_new 与 target 的重叠个数
                size_t overlapCount = 0;            
                for(const auto& targetState : target)
                {
                    if(targetState.meta.smallest.user_key <= candidateLargest && candidateSmallest <= targetState.meta.largest.user_key)
                        overlapCount++;
                }

                // ! 扩展后引入了新的目标文件 -> 停止 不能加入当前源表
                if(overlapCount != targetInputs.size()) break;

                sourceInputs.push_back(sourceState);
                sourceBytes += sourceState.meta.fileSize;
                smallest = candidateSmallest;
                largest = candidateLargest;
            }
        }
        // 初始化input和inputMetas
        inputs.reserve(sourceInputs.size() + targetInputs.size());
        inputMetas.reserve(sourceInputs.size() + targetInputs.size());
        for(const auto& state : sourceInputs) 
        {
            inputs.push_back(state.reader.get());
            inputMetas.push_back(state.meta);
        }
        for(const auto& state : targetInputs) 
        {
            inputs.push_back(state.reader.get());
            inputMetas.push_back(state.meta);
        }
    }

    // 2. 锁外：BuildCompactionOutputs()
    std::vector<TableMeta> outputMetas = BuildCompactionOutputs(inputs, sourceLevel+1);

    // 3. 锁外：准备新的SSTableState
    LevelTables outputStates;
    for(const auto& meta : outputMetas)
    {
        outputStates.push_back(SSTableState{meta, std::make_shared<SSTableReader>(TablePath(meta.fileNumber), meta.fileNumber, blockCache_)});
    }

    // 4. 锁内：发布
    std::vector<LevelTables> candidateLevels;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        CheckFailureLocked();               // ! 发布前检查db是否故障 避免另一个线程发生了故障 而此处又覆盖失败后尚不确定的磁盘状态

        // 依据fileNumber核验：sourceInputs中每张表仍存在于源层 + targetInputs中每张表仍存在于目标层
        // 遍历的同时：去除作为input的SSTableState
        auto removeInputs = [](LevelTables& level, const LevelTables& selected)
        {
            for(const auto& input : selected)
            {
                auto it = std::find_if(
                    level.begin(), level.end(), 
                    [&](const SSTableState& current)
                    {
                        return current.meta.fileNumber == input.meta.fileNumber;
                    });
                if(it == level.end()) throw std::runtime_error("DB::CompactTables: input table missing");   
                
                level.erase(it);
            }
        };
        // ! 复制的是发布时的当前集合 -> 期间 Flush 新增的 L0 表自然被保留下来。移除元素也不会改变其他 L0 表的相对顺序。
        candidateLevels = levels_;
        removeInputs(candidateLevels[sourceLevel], sourceInputs);
        removeInputs(candidateLevels[sourceLevel + 1], targetInputs);

        // 加入输出，排序，检查目标层
        auto& target = candidateLevels[sourceLevel + 1];
        target.insert(target.end(), outputStates.begin(), outputStates.end());
        std::sort(target.begin(), target.end(), 
        [](const SSTableState& a, const SSTableState& b)
            {
                return a.meta.smallest.user_key < b.meta.smallest.user_key;
            });
        for(size_t i=1; i<target.size(); i++)
        {
            if(target[i-1].meta.largest.user_key >= target[i].meta.smallest.user_key)
                throw std::runtime_error("DB::CompactTables: overlapping SSTables in level");
        }

        // 构造候选 MANIFEST
        Manifest tmpManifest;
        tmpManifest.SetLastSequence(lastSequence_);
        tmpManifest.SetFlushedSequence(manifest_.FlushedSequence());
        if(manifest_.Generation() == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("DB::CompactTables: Manifest generation overflow, cannot increment");
        tmpManifest.SetGeneration(manifest_.Generation()+1);
        for(const auto& level: candidateLevels)
        {
            for(const auto& state : level) tmpManifest.AddTable(state.meta);
        }
        
        // 持久化 MANIFEST
        try
        {
            tmpManifest.Save(manifestPath_);
        }
        catch (...)     
        {
            if (!backgroundError_)
                backgroundError_ = std::current_exception();

            backgroundCv_.notify_all();
            throw;
        }
        /* !
        必须在持有dbMutex_时设置error，否则可能发生：
        1.刷盘线程：Save 抛异常 → 发布锁释放
        2.合并线程：取得发布锁 → 故障尚未登记 → 检查通过并发布
        3.刷盘线程：进入循环 catch → 才登记故障
        */

        // 替换内存集合
        manifest_ = std::move(tmpManifest);
        levels_.swap(candidateLevels);        // swap：让旧的Reader到锁外再释放

        // 更新已完成的请求编号 + 通知CompactLevel()
        if(taskRequest != 0) compactionCompleted_ = taskRequest;    // 自动任务不改变compactionCompleted_
        backgroundCv_.notify_all();
    }

    // 5. 锁外：释放旧的SSTableReader、清理旧文件
    inputs.clear();             // 不再使用旧 Reader 的借用指针
    candidateLevels.clear();    // 锁外释放旧旧集合的引用

    try             // ! 删除文件失败：不视为db故障
    {
        for(const auto& meta : inputMetas)
        {
            if(unlink(TablePath(meta.fileNumber).c_str()))
            {
                //! ENOENT: 文件不存在
                if(errno != ENOENT) throw std::system_error(errno, std::generic_category(),"DB::CompactTables: unlink failed");
            }
        }

        SyncDirectory();        // ! 删除文件：一定要同步父目录
    }
    catch(const std::exception& e)
    {
        std::fprintf(stderr,
            "Failed to clean SSTables, residual files remain: %s\n",
            e.what());
    }
    catch(...)
    {
        std::fprintf(stderr,
            "Failed to clean SSTables: unknown exception\n");
    }
}

std::optional<uint32_t> DB::PickAutoCompactionLevelLocked() const 
{
    if(!options_.enableAutoCompaction || levels_.size() < 2) return std::nullopt;

    double maxScore = 0;
    double curScore = 0;
    std::optional<uint32_t> selectedLevel;     
    
    // L0：score = 总文件数/阈值文件数
    curScore = 1.0 * levels_[0].size() / options_.level0CompactionTrigger;
    if(curScore >= 1)
    {
        maxScore = curScore;
        selectedLevel = 0;
    }
    // L1+：score = 总文件大小/阈值文件大小
    uint64_t targetBytes = options_.level1TargetBytes;
    const uint64_t limit = std::numeric_limits<uint64_t>::max();
    for(size_t i=1; i<levels_.size()-1; i++)            // ! 最后一层不能作为源层
    {
        uint64_t sumFileSize = 0;
        for(const auto& state : levels_[i]) 
        {
            if(state.meta.fileSize > limit - sumFileSize) sumFileSize = limit;      // ! 饱和式计算 避免溢出
            else sumFileSize += state.meta.fileSize;
        }
        curScore = 1.0 * sumFileSize / targetBytes;
        if(curScore >= 1 && curScore > maxScore)
        {
            maxScore = curScore;
            selectedLevel = i;
        }
        if(targetBytes > limit / options_.levelSizeMultiplier) targetBytes = limit;     // ! 饱和式计算 避免溢出
        else targetBytes *= options_.levelSizeMultiplier;
    }
    
    return selectedLevel;
}
