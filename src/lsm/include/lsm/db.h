// db.h
#pragma once

#include <vector>
#include <deque>
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
class BlockCache;
class WriteBatch;

struct DBOptions
{
    bool enableWal = true;
    size_t memTableSizeLimit = 4 * 1024 * 1024;         // 禁用 0
    size_t blockCacheCapacity = 8 * 1024 * 1024;        // 0 表示不使用 BlockCache
    size_t maxGroupRequests = 32;                       // 禁用 0
    size_t maxGroupBytes = 1024 * 1024;                 // 每组预计 WAL 编码字节的最大预算
    size_t targetSSTableDataSize = 4 * 1024 * 1024;     // Compaction 专用：记录编码字节目标、不是文件大小硬限制 -> 估计expectedKeyCount
    size_t maxLevels = 7;                               // SSTable最大层数(而非层号)
    size_t maxCompactionSourceBytes = 4 * 1024 * 1024;  // Compaction：源表的最大预算(仅单表可超)
    bool enableAutoCompaction = true;                   // 自动合并
    size_t level0CompactionTrigger = 4;                 // L0 达到多少张表时触发自动合并
    size_t level1TargetBytes = 16 * 1024 * 1024;      // L1 容量目标
    size_t levelSizeMultiplier = 10;                  // 层间目标容量倍率
    size_t maxCompactionSourceExpansionFactor = 2;    // 目标输入不增加时有限扩展源输入：最多拓展为基础源预算的x倍
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

    // 同步提交非空批次；入队后由组长合并请求、执行并通知结果，异常不代表已回滚
    // 整批统一可见；开启WAL时同组共享Sync，关闭WAL时需Flush保证持久化
    // 内部修改sequence但不清空batch；调用期间不得并发修改或重复提交同一batch
    void Write(WriteBatch& batch);

    /*
     全程持锁，依据 publishedSequence_ 进行查找
     查找顺序：active->immutable(若存在)->SSTable
     返回值:  kValue->value  kDeleted->nullopt  kNotFound->继续找下一层
    */
    std::optional<std::string> Get(std::string_view key) const;

    // 等待：调用Flush()时已发布的全部写入被SSTable和MANIFEST可靠保存
    // active为空但immutable仍含目标数据时，仍需等待
    // 后台失败或关闭时抛异常；不等待旧WAL清理成功
    void Flush();

    void CompactLevel(uint32_t sourceLevel);
private:
    struct MemTableState
    {

#ifdef LSM_DIAGNOSTICS
        int diagnosticOrigin = 0;
#endif
        std::shared_ptr<MemTable> table;        // shared_ptr: 让后台能保留稳定引用 而不提前析构
        std::vector<uint64_t> walNumbers;       // (分段式WAL)应对恢复时一个MemTable对应多个wal文件的情况 方便分批次清理wal文件
    };

    // ! 将sstable的元数据和Reader绑定，从而查询、恢复时不需要依赖manifest中储存的元数据的顺序
    struct SSTableState
    {
        TableMeta meta;
        std::shared_ptr<SSTableReader> reader;
    };

    struct PendingWrite
    {
        WriteBatch* batch;              // 借用调用方的批次，Write()返回前有效
        size_t estimatedBytes = 0;      // 该 pending 写入 WAL 预计的字节预算
        bool done = false;              // 请求是否处理完成，成功或失败均为true
        std::exception_ptr error;       // 写入失败的错误原因
        std::condition_variable cv;     // 检查：done / 成为队首
    };
    
    // 在线程启动前恢复MANIFEST和有效SSTable，并初始化文件编号
    // 开启WAL时按段号重放日志，跳过已刷盘记录，仅允许修复最新段残缺尾部
    // 创建新的写入段，登记active的全部来源WAL，最后恢复publishedSequence_
    void Recover();

    // 内部加锁检查健康状态；已持dbMutex_时不能调用
    void CheckHealth() const;
    // 调用者已持dbMutex_；重新抛出后台异常，或报告故障、关闭状态
    void CheckHealthLocked() const;
    // 调用者已持dbMutex_；只检查failed_、backgroundError_
    void CheckFailureLocked() const;

    // 扫描正式和临时SSTable文件名，确认下一个可用编号
    void InitializeNextFileNumber();

    // 根据文件编号生成数据库目录内SSTable的路径
    std::string TablePath(uint64_t fileNumber) const;
    std::string WalPath(uint64_t walNumber) const;          // 分段式wal文件

    // 持久化数据库目录项变更，用于新WAL创建和旧WAL删除
    // SSTableWriter和Manifest::Save已自行同步目录，无需重复调用
    void SyncDirectory() const;

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
    void FlushLoop();

    // 以下时刻进行检查：恢复完成、合并线程启动时 || Flush 发布新 L0 表并通知时 || 一次 Compaction 完成、后台回到循环时 || 手动请求登记并通知时
    void CompactionLoop();

    // 调用者已持dbMutex_，且DB健康、active非空、immutable不存在
    // 先准备新MemTable和WAL，再冻结并移交旧active，最后通知后台
    // 由Write或公共Flush调用；内部不等待刷盘
    void RotateMemTableLocked();

    // 实际执行后台任务：锁外构建SSTable并持久化，锁内发布manifest_、tables_
    void FlushImmutable(const MemTableState& state);

    // 调用者不持队列锁或DB锁；内部持DB锁检查健康、处理背压、校验batch(非db故障)、预分配sequence
    // 开启WAL时逐批Append、共享一次Sync，写入MemTable后统一发布sequence
    // WAL或MemTable写入阶段出错则标记DB故障并抛异常；队列收尾和完成通知由Write负责
    void WriteGroup(const std::vector<PendingWrite*>& group);

    // 锁外： build SStable + 写文件 + 记录TableMeta
    std::vector<TableMeta> BuildCompactionOutputs(const std::vector<const SSTableReader*>& tables, uint32_t targetLevel);

    // 仅供合并线程调用；可与 FlushImmutable 并行构建，发布由 dbMutex_ 串行保护
    // 选择源层有限输入及目标层重叠文件进行合并，不处理 active/immutable
    void CompactTables(uint64_t taskRequest, uint32_t sourceLevel);

    // 调用者持有 dbMutex_
    // 返回需要自动合并的源层 不存在则返回 nullopt
    std::optional<uint32_t> PickAutoCompactionLevelLocked() const;

private:
    using LevelTables = std::vector<SSTableState>;

    DBOptions options_;
    std::string directory_;
    std::string manifestPath_;
    std::string walPath_;

    mutable std::mutex dbMutex_;                            // mutable: Get()const 也能加锁使用
    std::condition_variable backgroundCv_;
    std::thread flushThread_;
    std::thread compactionThread_;
    std::exception_ptr backgroundError_;

    std::mutex writeQueueMutex_;
    std::deque<PendingWrite*> writeQueue_;                  // group commit
    
    Manifest manifest_;
    //std::vector<std::shared_ptr<SSTableReader>> tables_;    // 与 manifest_.Tables() 一一对应 Compaction输出位于保留的新刷盘表之前 Get 逆序查询
    std::vector<LevelTables> levels_;

    MemTableState active_;
    std::optional<MemTableState> immutable_;
    std::unique_ptr<WalWriter> walWriter_;          // !可选延迟构造 在不需要wal时不构造
    std::shared_ptr<BlockCache> blockCache_;

    uint64_t lastSequence_ = 0;                     // 编号分配进度，恢复时结合MANIFEST与WAL确定
    uint64_t publishedSequence_ = 0;                // 已对查询可见的最大sequence；未开启WAL时不含持久化承诺
    uint64_t compactionRequested_ = 0;              // Compaction：已提交的最大请求编号
    uint64_t compactionCompleted_ = 0;              // Compaction：已完成的最大请求编号
    uint32_t sourceLevel_ = 0;                      // 用于前台->后台Compaction传递(单任务栏位-条件变量等待)
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

4. 研究option的上限在不同情况下的最佳配置
5. 组提交：研究主动攒组等待是否值得

*/

/*
数据库目录/
├── MANIFEST
├── 000001.wal
├── 000002.wal
├── 000001.sst
└── 000002.sst
*/