// sstable.h
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <optional>

#include "internal_key.h"
#include "data_block.h"
#include "bloom_filter.h"
#include "block_cache.h"
#include "coding.h"
#include "noncopyable.h"


namespace sstable_format 
{
    inline constexpr std::string_view kMagic = "MYLSM001";      // ! inline: 允许头文件多次被包含，多个翻译单元都看到这个定义，不会报多重定义链接错误
    inline constexpr uint32_t kVersion = 2; // ! 新版Entry布局：InternalKey + value_len + value
    inline constexpr size_t kFooterSize = 44;                   // ! [footer: 44][CRC: 4]
}


struct BlockIndexEntry
{
    InternalKey firstKey;
    uint64_t offset;    // 在所有块拼接后的数据区内的偏移
    uint32_t size;      // 当前块的字节数
};


class SSTableBuilder : private lsm::NonCopyable
{
public:
    /*
     expectedKeyCount = 0
       → Bloom 没有空间
       → Add("apple")：数据块成功保存 apple，Bloom 却忽略它
       → 查询 apple：Bloom 返回 false
       → 跳过 SSTable，漏掉真实存在的数据
     为了保证 Bloom 不漏判，仍分配至少一个 key 的过滤空间
    */
    SSTableBuilder(size_t expectedKeyCount, size_t targetBlockSize = 4 * 1024);  

    //检查全表顺序、判断是否切块、追加记录、更新 Bloom
    void Add(const lsm::Entry& entry);

    // 冻结当前的SSTable 可重复调用
    void Finish();

    // entryCount_ == 0 
    bool Empty() const noexcept;
    bool IsFinished() const noexcept;
    size_t BlockCount() const noexcept;

    // 以下访问接口 在Finish()之后才能使用：避免使用未完成的SSTable
    const BloomFilter& Filter() const;
    const std::vector<std::string>& Blocks() const;
    const std::vector<BlockIndexEntry>& Index() const;
    
private:
    // 保存当前块和索引 + Reset Builder
    void FinishCurrentBlock();

    size_t targetBlockSize_ = 0;        // DataBlock的大小(非硬上限)
    uint64_t dataOffset_ = 0;         // 当前构造的DataBlock起始偏移
    size_t entryCount_ = 0;         // SSTable内已追加的记录数
    bool finished_ = false;

    InternalKey lastKey_;           // 开始构造新块时用于顺序检查：Reset后，前一个块最后的InternalKey无法直接获取
    bool hasLastKey_ = false;       

    DataBlockBuilder currentBlock_;
    BloomFilter bloomFilter_;

    std::vector<std::string> blocks_;
    std::vector<BlockIndexEntry> index_;
};



class SSTableReader : private lsm::NonCopyable
{
public:
    class Iterator
    {
    public:
        bool Valid() const;
        const InternalKey& Key() const;
        const std::string& Value() const;
        void Next();
    private:
        friend SSTableReader;

        explicit Iterator(const SSTableReader* owner);
        const SSTableReader* owner_ = nullptr;
        size_t blockIndex_ = 0;                                     // 当前datablock的索引下标
        std::shared_ptr<const DataBlockReader> currentBlock_;       // 保证其生命周期 >= 顺序读取过程
        std::optional<DataBlockReader::Iterator> blockIterator_;    // ! 声明是公开的，构造是私有的(获取有效迭代器必须通过Begin())      // ! 无默认构造函数，无法在SSTableReader::Iterator的构造函数初始化列表中完成构造 -> optional延迟构造
    };

    /* 
     打开文件并加载 Footer、BloomFilter、Index
     系统调用失败抛 std::system_error
     文件格式或校验和错误抛 std::runtime_error
    */
    explicit SSTableReader(const std::string& path);
    SSTableReader(const std::string& path, uint64_t fileNumber, std::shared_ptr<BlockCache> blockCache);
    ~SSTableReader();
    
    // 返回 SSTableReader 的起始迭代器
    Iterator Begin() const;

    // 按照 readSequence 查找 userKey 的可见版本记录
    LookupResult Get(uint64_t readSequence, std::string_view userKey) const;

private:
    /* 
     - 加载 BloomFilter、Index、DataBlock的公共入口
     - pread() 从指定 offset 读取块内容，并校验其尾部 CRC
     - size 只包含块内存，不包含尾部 4 字节 CRC
     - 返回值只包含块内存，不包含 CRC
    */
    std::string ReadBlock(uint64_t offset, uint64_t size) const;
    std::shared_ptr<const DataBlockReader> LoadDataBlock(uint64_t offset, uint64_t size) const;

    void LoadFooter();
    void LoadIndex();
    void LoadBloomFilter();


    /* 找到最适合开始顺序查找的块 (先找到第一个 firstKey>target 的块) -> upper_bound()
        1. pos = begin():   从 begin 开始顺序查找
        2. pos = 中间:      从 pos-1 开始查找
        3. pos = end():     从 最后一块 开始查找
    */ 
    size_t FindStartBlock(const InternalKey& target) const;

private:
    int fd_ = -1;
    uint64_t fileNumber_ = 0;            // db中SSTable的文件编号
    uint64_t fileSize_ = 0;             // fstat获取 用于定位末尾Footer

    // 读取Footer   Footer是否需要封装为一个struct呢?
    uint64_t bloomOffset_ = 0;
    uint64_t bloomSize_ = 0;        // content内容 不包含CRC
    uint64_t indexOffset_ = 0;
    uint64_t indexSize_ = 0;        // content内容 不包含CRC

    std::unique_ptr<BloomFilter> bloomFilter_;      // ! 打开文件后才能知道构造参数 -> unique_ptr延后构造
    std::vector<BlockIndexEntry> index_;
    std::shared_ptr<BlockCache> blockCache_;
};



class SSTableWriter : private lsm::NonCopyable
{
public:
    // 向path中写入，成功后即为完整的SSTable
    // 会同步父目录
    static uint64_t Write(const std::string& path, const SSTableBuilder& builder);

private:
    SSTableWriter() = delete;
};



/*
先做内存版，是为了把“查询算法”与“文件读取问题”分开验证。

- 如果直接加入文件操作，出现错误时很难判断原因：
    - 查询算法错误？
    - 索引定位错误？
    - 文件 offset 错误？
    - 文件格式解析错误？
    - 短读处理错误？
    - 缓存生命周期错误？

- 因此先用内存版验证算法逻辑是否正确：
    Bloom 过滤 → 索引二分 → DataBlock 查询 → 跨块继续

- 等这个算法正确后，再把 Builder 的数据替换成文件中的索引、Bloom 和 Block。

- 这样文件版主要增加：
    文件布局、offset、CRC、Reader、缓存，而查询逻辑可以保持不变。
*/