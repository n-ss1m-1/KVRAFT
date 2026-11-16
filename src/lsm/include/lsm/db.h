// db.h
#pragma once

#include <vector>
#include <string>
#include <string_view>
#include <cstdint>
#include <memory>
#include <optional>
#include <condition_variable>
#include <thread>
#include <mutex>
#include <exception>

#include "manifest.h"
#include "coding.h"
#include "noncopyable.h"

class MemTable;
class WalWriter;
class SSTableReader;

struct DBOptions
{
    bool enableWal = true;
    size_t memTableSizeLimit = 4 * 1024 * 1024;
};

class DB : private lsm::NonCopyable
{
public:
    // 创建目录和active MemTable；
    // Recover(): 恢复Manifest_、SSTableReaders及重做Wal段、恢复成功后创建新的写入WAL段
    // 后台线程接入后，在全部初始化完成的最后启动
    DB(const std::string& directory, const DBOptions& options = {});
    

    // 不承诺自动刷完active；需要持久化全部数据时先显式Flush
    // 后台线程接入后：设置关闭标志并通知，解锁后join
    // 调用者必须保证析构期间没有其他前台调用
    ~DB();

   // 构造Entry并交给Write；sequence由Write在锁内统一分配
    void Put(std::string_view key, std::string_view value);
    void Delete(std::string_view key);

    /*
     全程持锁，依据 publishedSequence_ 进行查找
     查找顺序：active->immutable(若存在)->SSTable
     返回值:  kValue->value  kDeleted->nullopt  kNotFound->继续找下一层
    */
    std::optional<std::string> Get(std::string_view key) const;

    // 等待：调用Flush()时已发布的全部写入被SSTable和MANIFEST可靠保存。
    // active为空但immutable仍含目标数据时，仍需等待。
    // 后台失败或关闭时抛异常；不等待旧WAL清理成功。
    void Flush();

private:
    struct MemTableState
    {

#ifdef LSM_DIAGNOSTICS
        int diagnosticOrigin = 0;
#endif
        std::shared_ptr<MemTable> table;        // shared_ptr: 让后台能保留稳定引用 而不提前析构
        std::vector<uint64_t> walNumbers;       // (分段式WAL)应对恢复时一个MemTable对应多个wal文件的情况 方便分批次清理wal文件
    };

    // Put/Delete的共同入口，全程持dbMutex_，条件变量等待期间释放锁。
    // 写入前检查阈值：必要时等待immutable完成，再切换active和WAL。
    // 分配sequence、校验Entry、Append/Sync WAL、插入active，最后发布sequence。
    // 写入故障时设置failed_并通知等待者。
    void Write(lsm::Entry entry);
    
    // 在线程启动前恢复MANIFEST和有效SSTable，并初始化文件编号。
    // 开启WAL时按段号重放日志，跳过已刷盘记录，仅允许修复最新段残缺尾部。
    // 创建新的写入段，登记active的全部来源WAL，最后恢复publishedSequence_。
    void Recover();

    // 内部加锁检查健康状态；已持dbMutex_时不能调用。
    void CheckHealth() const;
    // 调用者已持dbMutex_；重新抛出后台异常，或报告故障、关闭状态。
    void CheckHealthLocked() const;

    // 扫描正式和临时SSTable文件名，确认下一个可用编号
    void InitializeNextFileNumber();

    // 根据文件编号生成数据库目录内SSTable的路径
    std::string TablePath(uint64_t fileNumber) const;
    std::string WalPath(uint64_t walNumber) const;          // 分段式wal文件


    // 持久化数据库目录项变更，用于新WAL创建和旧WAL删除。
    // SSTableWriter和Manifest::Save已自行同步目录，无需重复调用。
    void SyncDirectory() const;

    // 将WAL原子替换为空文件，同步目录并重新打开WalWriter
    // 仅在开启WAL且MANIFEST发布成功后调用
    // void ResetWal();

    // 扫描数据库目录中的正式WAL段 按编号升序返回
    // 同时确定nextWalNumber_
    std::vector<uint64_t> DiscoverWalFiles();

    // 创建新的WAL段，同步文件和目录
    // 成功后返回Writer，不直接修改active_和walWriter_
    std::unique_ptr<WalWriter> CreateWal(uint64_t walNumber);

    // 删除已经被有效SSTable覆盖的WAL段             obsolete: 过期的
    // 清理失败不撤销Manifest发布，后续可重试
    void DeleteObsoleteWals(const std::vector<uint64_t>& walNumbers);

    // 后台线程入口
    // 捕获异常、保存错误并通知前台 异常不能逃出线程入口，否则会触发std::terminate
    void BackgoundLoop();


    // 调用者已持dbMutex_，且DB健康、active非空、immutable不存在。
    // 先准备新MemTable和WAL，再冻结并移交旧active，最后通知后台。
    // 由Write或公共Flush调用；内部不等待刷盘。
    void RotateMemTableLocked();

    // 实际执行后台任务：锁外构建SSTable并持久化，锁内发布manifest_、tables_
    void FlushImmutable(const MemTableState& state);

private:


    DBOptions options_;
    std::string directory_;
    std::string manifestPath_;
    std::string walPath_;

    mutable std::mutex dbMutex_;                            // mutable: Get()const 也能加锁使用
    std::condition_variable backgroundCv_;
    std::thread backgoundThread_;
    std::exception_ptr backgroundError_;
    
    Manifest manifest_;
    std::vector<std::unique_ptr<SSTableReader>> tables_;    // 与manifest.Tables()一一对应，按刷盘顺序从旧到新保存 -> Get()反向遍历 优先查询最新表

    MemTableState active_;
    std::optional<MemTableState> immutable_;
    std::unique_ptr<WalWriter> walWriter_;          // !可选延迟构造 在不需要wal时不构造

    uint64_t lastSequence_ = 0;                     // 编号分配进度，恢复时结合MANIFEST与WAL确定
    uint64_t publishedSequence_ = 0;                // 已对查询可见的最大sequence；未开启WAL时不含持久化承诺
    uint64_t nextFileNumber_ = 1;                  // ! Recover()扫描初始化，运行时分配后递增
    uint64_t nextWalNumber_ = 1;

    bool failed_ = false;
    bool shuttingDown_ = false;
};

/*
TODO：
1. memTable_->Add()接受散装字段：后续可以新增 Add(const lsm::Entry&) 重载
2. DB::Get(string_view) 与 MemTable::Get(const string&) 不匹配：后续可以将memtable、skiplist等只借用查询参数的接口统一改为 string_view

3. 
    - WAL 写了一部分后失败：截断到本次写入前的位置，恢复成功后继续服务
    - WAL Sync 失败：首版进入故障状态，重启恢复
    - WAL 已 Sync，MemTable 插入失败：优先考虑补写 MemTable，而不是撤销 WAL

*/

/*
数据库目录/
├── MANIFEST
├── 000001.wal
├── 000002.wal
├── 000001.sst
└── 000002.sst
*/