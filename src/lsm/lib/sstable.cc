// sstable.cc
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <filesystem>
#include <algorithm>
#include <cstddef>      // std::byte
#include <climits>
#include <limits>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <new>          // std::bad_alloc
#include <cassert>

#include "sstable.h"
#include "diagnostics.h"

namespace 
{
    void WriteAll(int fd, std::string_view data)  // ! string_view > string：只借用字节，不修改、不复制，适合接收字符串或字节片段 
    {
        size_t done = 0;
        size_t totalLen = data.size();
        while(done < totalLen)
        {
            ssize_t n = write(fd, data.data()+done, totalLen-done);
            if(n < 0)
            {
                if(errno == EINTR) continue;
                
                // 系统调用失败  ! 注：此处不需要close(fd) 异常处理交给外层try_catch块
                throw std::system_error(errno,std::generic_category(),"WriteAll: write system call failed");
            }
            if(n == 0) throw std::system_error(std::make_error_code(std::errc::io_error), "WriteAll: no progress");
            done += static_cast<size_t>(n);
        }
    }

    // 所有块共用的外层格式 写入：[data][CRC32C:4]
    void WriteBlock(int fd, std::string_view data)
    {
        WriteAll(fd, data);
        std::string crc;
        lsm::AppendFixed32(crc, lsm::Crc32c(data));
        WriteAll(fd, crc);
    }

    // 根据指定偏移 offset 读取 size 字节的原始数据并返回
    // 由外部分配地址空间: char*->malloc  string->resize()
    void PreadAll(int fd, void* buf, size_t size, uint64_t offset)
    {
        std::byte* output = static_cast<std::byte*>(buf);
        size_t done = 0;
        while(done < size)
        {
            lsm_diag::count(lsm_diag::PreadCalls);
            ssize_t n = pread(fd, output+done, size-done, offset+done);
            if(n>0) lsm_diag::count(lsm_diag::PreadBytes, static_cast<uint64_t>(n));
            if(n == 0) 
            {
                throw std::runtime_error("PreadAll: unexpected EOF");
            }
            if(n < 0)
            {
                if(errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(), "PreadAll: pread system call failed");
            }
            done += static_cast<size_t>(n);
        }
    }
}

SSTableBuilder::SSTableBuilder(size_t expectedKeyCount, size_t targetBlockSize)
    : targetBlockSize_(targetBlockSize),
      bloomFilter_(expectedKeyCount == 0 ? 1 : expectedKeyCount)
{
    if(targetBlockSize == 0) throw std::invalid_argument("SSTableBuilder: targetBlockSize cannot be zero");
}

void SSTableBuilder::Add(const lsm::Entry& entry) 
{
    // 检查尚未Finish
    if(finished_) throw std::logic_error("SSTableBuilder: cannot Add entry after builder is finished");

    /* DataBlock也会校验 为什么这样也要再次校验呢?
     - 保证SSTable内记录顺序严格递增
     - DataBlockBuilder：当前块内部有序
     - SSTableBuilder：整张表有序，包括相邻两个块之间
    */ 
    if(hasLastKey_)
    {
        InternalKeyComparator cmp;    
        if(cmp.Compare(lastKey_, entry.key)>=0) throw std::invalid_argument("Add: DataBlock entry key out of order, key must be strictly increasing");
    }
    // !先检查顺序 再判断是否分块：避免逆序输入导致分块 然后又被拒绝

    // 校验并获取记录长度 -> 需要时分割块(!空块：可放入大记录、非空块：若放入新记录超出上限，则先分割) 
    size_t entrySize = lsm::EncodedEntrySize(entry);
    if(!currentBlock_.Empty() && (entrySize > targetBlockSize_ || (currentBlock_.ByteSize() > targetBlockSize_ - entrySize))) FinishCurrentBlock();
    
    // 提交entry -> DataBlock和BloomFilter  +  修改一些记录参数
    currentBlock_.Add(entry);
    bloomFilter_.Add(entry.key.user_key);

    hasLastKey_ = true;
    lastKey_ = entry.key;           // TODO:!? 与DataBlockBuilder::Add()相同的复制问题：先拷贝到newLastKey，此处再swap，可以避免内存分配失败导致的状态不一致
    entryCount_++;
}

void SSTableBuilder::Finish() 
{
    if(finished_) return;

    FinishCurrentBlock();
    finished_ = true;
}

bool SSTableBuilder::Empty() const noexcept 
{
    return entryCount_ == 0;
}

bool SSTableBuilder::IsFinished() const noexcept
{
    return finished_;
}

size_t SSTableBuilder::BlockCount() const noexcept 
{
    return blocks_.size();
}

const BloomFilter& SSTableBuilder::Filter() const
{
    if(!finished_) throw std::logic_error("SSTableBuilder::Index(): bloom filter not available, builder not finished");
    return bloomFilter_;
}

const std::vector<std::string>& SSTableBuilder::Blocks() const
{
    if(!finished_) throw std::logic_error("SSTableBuilder::Index(): blocks not available, builder not finished");
    return blocks_;
}

const std::vector<BlockIndexEntry>& SSTableBuilder::Index() const
{
    if(!finished_) throw std::logic_error("SSTableBuilder::Index(): block index not available, builder not finished");
    return index_;
}

void SSTableBuilder::FinishCurrentBlock() 
{
    // 检查当前块是否为空
    if(currentBlock_.Empty()) return;

    // 准备块字节副本 + 索引项(检查数据安全不溢出)
    const std::string_view block = currentBlock_.Data();

    BlockIndexEntry index;
    index.firstKey = currentBlock_.FirstKey();
    if(block.size() > UINT32_MAX) throw std::invalid_argument("SSTableBuilder::FinishCurrentBlock(): block size exceeds uint32_t max limit");
    index.offset = dataOffset_;
    index.size = block.size();

    // 偏移溢出检查(插入vector之前)
    if(dataOffset_ > std::numeric_limits<uint64_t>::max() - index.size) throw std::length_error("SSTableBuilder::FinishCurrentBlock(): data offset overflow");

    // 将块和索引放入两个vector
    blocks_.emplace_back(block);    // 原地构造 string_view->string
    index_.push_back(index);        // !TODO: try catch捕获异常，避免blocks_和index_状态不一致

    // 更新dataOffset_ + Reset
    dataOffset_ += index.size;
    currentBlock_.Reset();
}

SSTableReader::SSTableReader(const std::string& path)
    : SSTableReader(path, 0, nullptr)   //! 不重复实现 将单参数构造委托给三参数构造
{}

SSTableReader::SSTableReader(const std::string& path, uint64_t fileNumber, std::shared_ptr<BlockCache> blockCache)
    : fileNumber_(fileNumber),
      blockCache_(std::move(blockCache))
{
    // 1. 打开文件 初始化fd_
    fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd_ < 0) throw std::system_error(errno, std::generic_category(), "SSTableReader: open failed");

    try
    {
        // 2. 获取文件大小
        struct stat st{};
        if(fstat(fd_, &st) < 0) 
        {
            throw std::system_error(errno, std::generic_category(), "SSTableReader: fstat failed");
        }
        fileSize_ = static_cast<uint64_t>(st.st_size);

        // 3. 加载 Footer、BloomFilter、Index
        LoadFooter();
        LoadIndex();
        LoadBloomFilter();
    }
    catch(...)      // ! 构造函数中途抛异常时，不会调用本对象的析构函数 -> 手动关闭
    {
        close(fd_);
        fd_ = -1;
        throw;
    }

}

SSTableReader::~SSTableReader()
{
    if(fd_ >= 0)
    {
        close(fd_);
        fd_ = -1;
    }
}

SSTableReader::Iterator SSTableReader::Begin() const        // ! 类外(返回值)：必须指明SSTableReader作用域
{
    return Iterator(this);
}

LookupResult SSTableReader::Get(uint64_t readSequence, std::string_view userKey) const 
{
    LookupResult result;        // 默认为 kNotFound

    // 1. 检查是否为空表
    if(index_.empty()) return result;

    // 2. 检查布隆过滤器
    lsm_diag::count(lsm_diag::BloomChecks);
    if(!bloomFilter_->MayContain(userKey)) {lsm_diag::count(lsm_diag::BloomRejects); return result;}

#ifdef LSM_DIAGNOSTICS
    struct QueryOutcome {LookupResult& result;uint64_t blocks=0;int exceptions=std::uncaught_exceptions();~QueryOutcome(){if(std::uncaught_exceptions()!=exceptions)return;bool found=result.state!=LookupState::kNotFound;lsm_diag::count(found?lsm_diag::BloomFound:lsm_diag::BloomNotFound);lsm_diag::count(found?lsm_diag::FoundBlocks:lsm_diag::NotFoundBlocks,blocks);if(blocks>1)lsm_diag::count(lsm_diag::CrossBlocks,blocks-1);}} diagnosticQuery{result};
#endif
    // 3. 构造target
    InternalKey target;
    target.sequence = readSequence;
    target.user_key = userKey;

    // 4. FindStartBlock() 寻找最适合开始顺序查找的块下标
    lsm_diag::count(lsm_diag::IndexLookups);
    size_t blockIndex = FindStartBlock(target);

    // 5. 依次读取块: DataBlockReader::Get()
    while(blockIndex < index_.size())
    {
        const auto& idx = index_[blockIndex];
#ifdef LSM_DIAGNOSTICS
        ++diagnosticQuery.blocks;
#endif
        std::shared_ptr<const DataBlockReader> blockReader = LoadDataBlock(idx.offset, idx.size);

        // 5.1 找到kValue or kDeletion -> 立刻返回
        result = LSM_TIME(BlockGet, blockReader->Get(readSequence, userKey));
        if(result.state != LookupState::kNotFound) return result;

        /*
         5.2 当前块未找到 依据下一个块的 firstKey.user_key 判断是否需要继续查找   
            - target.user_key == 下一个块的 firstKey.user_key -> 继续找     
            - target.user_key <= 下一个块的 firstKey.user_key -> kNotFound
            - !注意：跨块只需要比较user_key是否相同 下一个块的sequence更小是可以接受的
        */
        ++blockIndex;
        if(blockIndex >= index_.size()) break;       

        if(target.user_key < index_[blockIndex].firstKey.user_key) return result;       
        // ! 此处不能直接+1 需要先判断是否越界     
        // ! user_key的比较不能依据是否等于：可能下一个块的首个user_key仍比target.user_key小
    }

    return result;
}

std::string SSTableReader::ReadBlock(uint64_t offset, uint64_t size) const 
{
    LSM_SCOPE(ReadBlock);
    // 1. 检查 offset + size + 4 合法 (! 逐渐添加减法元素 避免溢出)
    if(offset > fileSize_ || size > fileSize_ - offset || 4 > fileSize_ - offset - size) throw std::runtime_error("SSTableReader::ReadBlock: block out of file range");

    // 2. 循环读取content
    std::string content(size,'\0');
    LSM_TIME(Pread, PreadAll(fd_, content.data(), size, offset));

    // 3. 循环读取CRC
    char rawCrc[4];
    LSM_TIME(Pread, PreadAll(fd_, rawCrc, 4, offset+size));

    // 4. 重新计算CRC + 校验 + 校验失败处理
    if(LSM_TIME(Crc, lsm::Crc32c(content) != lsm::DecodeFixed32(rawCrc))) throw std::runtime_error("SSTableReader::ReadBlock: block crc check failed");

    // 5. 返回不含 CRC 的 block 内容
    return content;
}

std::shared_ptr<const DataBlockReader> SSTableReader::LoadDataBlock(uint64_t offset, uint64_t size) const
{
    // 查询块缓存
    BlockCacheKey cacheKey{fileNumber_, offset};
    std::shared_ptr<const DataBlockReader> reader = nullptr;
    if(blockCache_) reader = blockCache_->Get(cacheKey);            // ! 有缓存才尝试从缓存读取

    // 未命中：ReadBlock -> 构造并验证DataBlockReader -> 插入缓存
    if(reader == nullptr)
    {
        // 只统计实际加载；缓存命中不重复读取、CRC 和构造验证。
        lsm_diag::count(lsm_diag::Blocks);
        lsm_diag::count(lsm_diag::Bytes, size + 4);
        std::string rawBlock = ReadBlock(offset, size);
        reader = LSM_TIME(Validate, std::make_shared<const DataBlockReader>(std::move(rawBlock)));
        if(blockCache_)             // ! 有缓存才插入缓存
        {
            try
            {
                blockCache_->Insert(cacheKey, reader, size);
            }
            catch (const std::bad_alloc&)
            {
                // 缓存分配失败：仍使用当前已验证的 reader 完成查询
            }
        }
    }
    
    // 命中：直接返回
    return reader;
}


/*
[magic: 8]            
[version: 4]          
[bloom_offset: 8]
[bloom_size: 8]
[index_offset: 8]
[index_size: 8]
[footer_crc: 4]
*/
void SSTableReader::LoadFooter() 
{
    // 1. 校验 fileSize_ 是否足够 48 字节(Footer 一共48字节)
    if(fileSize_ < sstable_format::kFooterSize + 4) throw std::runtime_error("SSTableReader::LoadFooter: file too small, not a valid sstable");

    // 2. 读取 Footer 并校验 crc
    std::string footerRaw = ReadBlock(fileSize_ - sstable_format::kFooterSize - 4, sstable_format::kFooterSize);

    // 3. 校验 magic 和 version
    uint64_t offset = 0;

    std::string magic = footerRaw.substr(offset,8);
    offset += magic.size();
    if(magic != sstable_format::kMagic) throw std::runtime_error("SSTableReader::LoadFooter(): footer magic mismatch");

    uint32_t version = lsm::DecodeFixed32(footerRaw.data() + offset);
    if(version != sstable_format::kVersion) throw std::runtime_error("SSTableReader::LoadFooter(): footer version mismatch");
    offset += sizeof(version);

    // 4. 读取+解析：2组 offset和size
    bloomOffset_ = lsm::DecodeFixed64(footerRaw.data() + offset);
    offset += sizeof(bloomOffset_);

    bloomSize_ = lsm::DecodeFixed64(footerRaw.data() + offset);
    offset += sizeof(bloomSize_);

    indexOffset_ = lsm::DecodeFixed64(footerRaw.data() + offset);
    offset += sizeof(indexOffset_);

    indexSize_ = lsm::DecodeFixed64(footerRaw.data() + offset);
    offset += sizeof(indexSize_);
}

/*
[block_count: 4]
重复 block_count 次：
    [user_key_len: 4]
    [user_key]
    [sequence: 8]
    [type: 1]
    [offset: 8]
    [size: 4]
[CRC32C: 4]
*/
void SSTableReader::LoadIndex() 
{
    // ! 检查长度是否合法(...)
    // 1. 读取content + 校验CRC
    std::string indexRaw = ReadBlock(indexOffset_, indexSize_);
    
    uint64_t offset = 0;
    // 2. 读取block_count
    if(indexRaw.size() - offset < 4) throw std::runtime_error("SSTableReader::LoadIndex: truncated block count");
    uint32_t blockCount = lsm::DecodeFixed32(indexRaw.data() + offset);
    offset += sizeof(blockCount);

    for(uint32_t i=0; i<blockCount; i++)
    {
        // 3. 循环读取index 并插入
        struct BlockIndexEntry idx;

        if(indexRaw.size() - offset < 4) throw std::runtime_error("SSTableReader::LoadIndex: truncated user key length");
        uint32_t userKeyLen = lsm::DecodeFixed32(indexRaw.data() + offset);
        offset += sizeof(userKeyLen);

        if(indexRaw.size() - offset < userKeyLen) throw std::runtime_error("SSTableReader::LoadIndex: truncated user key");
        idx.firstKey.user_key = indexRaw.substr(offset, userKeyLen);
        offset += userKeyLen;

        if(indexRaw.size() - offset < 21) throw std::runtime_error("SSTableReader::LoadIndex: truncated internal key metadata");
        idx.firstKey.sequence = lsm::DecodeFixed64(indexRaw.data() + offset);
        offset += sizeof(idx.firstKey.sequence);

        idx.firstKey.type = static_cast<ValueType>(indexRaw[offset]);
        offset += sizeof(idx.firstKey.type);

        idx.offset = lsm::DecodeFixed64(indexRaw.data() + offset);
        offset += sizeof(idx.offset);

        idx.size = lsm::DecodeFixed32(indexRaw.data() + offset);
        offset += sizeof(idx.size);

        index_.push_back(std::move(idx));
    }

    if(indexRaw.size() != offset)  throw std::runtime_error("SSTableReader::LoadIndex: extra trailing garbage in index block");
}

/*
[bit_count: 4]
[hash_count: 4]
[bits_byte_size: 8]
[bits: bits_byte_size]
[CRC32C: 4]
*/
void SSTableReader::LoadBloomFilter() 
{
    // ! 检查长度是否合法(...)
    // 1. 读取content + 校验CRC
    std::string bloomFilterRaw = ReadBlock(bloomOffset_, bloomSize_);
    if (bloomFilterRaw.size() < 16) throw std::runtime_error("SSTableReader::LoadBloomFilter: truncated bloom header");

    // 2. 读取: bitCount、hashCount、byteSize、bits
    uint64_t offset = 0;
    uint32_t bitCount = lsm::DecodeFixed32(bloomFilterRaw.data() + offset);
    offset += sizeof(bitCount);

    uint32_t hashCount = lsm::DecodeFixed32(bloomFilterRaw.data() + offset);
    offset += sizeof(hashCount);

    uint64_t byteSize = lsm::DecodeFixed64(bloomFilterRaw.data() + offset);
    offset += sizeof(byteSize);
    if(byteSize != bloomFilterRaw.size() - offset) throw std::runtime_error("SSTableReader::LoadBloomFilter: bloom byte size mismatch");

    std::vector<uint8_t> bits(bloomFilterRaw.begin()+offset,bloomFilterRaw.end());
    offset += bits.size();

    bloomFilter_ = std::make_unique<BloomFilter>(bitCount,hashCount,std::move(bits));
}

size_t SSTableReader::FindStartBlock(const InternalKey& target) const 
{
    InternalKeyComparator cmp;

    /* !▲
     std::upper_bound()语义：找第一个 满足   value < element 的元素(找第一个 >value 的元素)  
     其中cmp(value,element)=true 即表示 value < element  ->  cmp返回true时停下

     std::lower_bound()语义：找第一个 不满足 element < value 的元素(找第一个 >=value 的元素)
     其中cmp(element,value)=true 即表示 element < value  ->  cmp返回false时停下
    */
    auto it = std::upper_bound(index_.begin(),index_.end(),target,
        [&cmp](const InternalKey& target,const BlockIndexEntry& idx){
            return cmp(target,idx.firstKey);
        });

    if(it == index_.begin()) return 0;
    if(it == index_.end()) return index_.size()-1;
    return static_cast<size_t>(it-index_.begin()-1);
}



uint64_t SSTableWriter::Write(const std::string& path, const SSTableBuilder& builder)
{
    // 1. 检查Builder是否已完成
    if(!builder.IsFinished()) throw std::logic_error("SSTableWriter::Write: write not available, builder not finished");

    if(builder.Filter().BitCount() > UINT32_MAX || builder.Filter().HashCount() > UINT32_MAX || builder.BlockCount() > UINT32_MAX)
        throw std::length_error("SSTableWriter::Write: format field exceeds uint32_t");

    // 2. 创建临时SSTable文件
    const std::string pathTmp = path + ".tmp";
    int fd = open(pathTmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if(fd < 0)
    {
        throw std::system_error(errno, std::generic_category(), "SSTableWriter::Write: open temp file failed: " + pathTmp);
    }

    // ! 捕获异常
    uint64_t fileOffset = 0;
    try
    {
        // 3. 写入 Data Blocks
        const auto& blocks = builder.Blocks();
        for(const auto& content : blocks)
        {
            WriteBlock(fd, content);
            fileOffset += content.size() + 4;
        }

        // 4. 写入 Bloom Filter     ! size_t在当前Linux下为64位，bitCount_和hashCount_已由size_t更改为uint32_t
        uint64_t bloomOffset = fileOffset;
        std::string bloomBlock;
        // 写content                                  TODO：有比拼接string更好的办法吗? 重载Crc32c接口?   [solution：增量CRC]  但是目前拼接str + 生成一个CRC更直观，暂时不用拓展
        const auto& bloomFilter = builder.Filter();
        lsm::AppendFixed32(bloomBlock, bloomFilter.BitCount());
        lsm::AppendFixed32(bloomBlock, bloomFilter.HashCount());
        lsm::AppendFixed64(bloomBlock, bloomFilter.ByteSize());
        bloomBlock.append(reinterpret_cast<const char*>(bloomFilter.Bits().data()), bloomFilter.ByteSize());   //! vector<uint8_t> -> .data()指针 -> unsigned char*  // !▲ 二进制数据必须带长度，避免按 C 字符串读取导致截断或越界。
        const uint64_t bloomSize = bloomBlock.size();
        WriteBlock(fd, bloomBlock);
        fileOffset += bloomSize + 4;

        // 5. 写入 Index
        uint64_t indexOffset = fileOffset;
        std::string indexBlock;

        // 写blockCount
        uint32_t blockCount = blocks.size();
        lsm::AppendFixed32(indexBlock, blockCount);

        // 循环blockCount次 写index
        const auto& index = builder.Index();
        uint64_t dataBlockOffset = 0;
        for(const auto& idx : index)
        {
            const auto& firstKey = idx.firstKey;
            lsm::AppendFixed32(indexBlock, firstKey.user_key.size());
            indexBlock.append(firstKey.user_key);
            lsm::AppendFixed64(indexBlock, firstKey.sequence);
            indexBlock.push_back(static_cast<char>(firstKey.type));
            lsm::AppendFixed64(indexBlock, dataBlockOffset);
            lsm::AppendFixed32(indexBlock, idx.size);
            dataBlockOffset += static_cast<uint64_t>(idx.size) + 4;
        }
        const uint64_t indexSize = indexBlock.size();
        WriteBlock(fd, indexBlock);
        fileOffset += indexSize + 4;

        // 6. 写入 Footer
        // 写footer
        std::string footer;

        footer.append(sstable_format::kMagic);
        lsm::AppendFixed32(footer, sstable_format::kVersion);
        lsm::AppendFixed64(footer, bloomOffset);
        lsm::AppendFixed64(footer, bloomSize);
        lsm::AppendFixed64(footer, indexOffset);
        lsm::AppendFixed64(footer, indexSize);
        const uint64_t footerSize = footer.size();
        WriteBlock(fd, footer);
        fileOffset += footerSize + 4;

        // 7. 发布
        int syncResult;
        do { syncResult = fsync(fd); } while(syncResult < 0 && errno == EINTR);
        if(syncResult < 0)
        {
            throw std::system_error(errno, std::generic_category(), "SSTableWriter::Write: fsync failed");
        }
        const int closeResult = close(fd);
        const int closeError = errno;
        fd = -1; // Linux 上 close 后不重试，也不在异常清理中再次关闭
        if(closeResult != 0)
            throw std::system_error(closeError, std::generic_category(), "SSTableWriter::Write: close tmp fd failed");
        if(rename(pathTmp.c_str(),path.c_str()) < 0)
        {
            throw std::system_error(errno, std::generic_category(), "SSTableWriter::Write: rename tmp to target failed");
        }
    }
    catch(...)
    {
        // 任意系统调用异常：关闭fd + 清理tmp文件
        if(fd >= 0) close(fd);

        unlink(pathTmp.c_str());       // 删除tmp文件
        throw;                              // 继续向外抛出原来的异常
    }

    // ! fsync父目录
    std::filesystem::path parentDir = std::filesystem::path(path).parent_path();
    if(parentDir.empty()) parentDir = ".";

    int dirFd = open(parentDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);              // ! 目录是特殊文件，只能以只读方式打开  |  fsync()也不需要写权限
    if(dirFd < 0) throw std::system_error(errno, std::generic_category(), "SSTableWriter::Write: open parent dir failed");
    while(fsync(dirFd) < 0)
    {
        if(errno == EINTR) continue;
        int err = errno;
        close(dirFd);
        throw std::system_error(err, std::generic_category(), "SSTableWriter::Write: fsync parent dir failed");
    }
    
    if(close(dirFd) < 0) throw std::system_error(errno, std::generic_category(), "SSTableWriter::Write: close parent dir failed");

    return fileOffset;
}

SSTableReader::Iterator::Iterator(const SSTableReader* owner)
    : owner_(owner)
{
    // 空sstable直接跳过
    if(owner_->index_.empty()) return;
    const auto& idx = owner_->index_[blockIndex_];
    auto block = owner_->LoadDataBlock(idx.offset, idx.size);
    auto iter = block->Begin();

    // 空块报错
    if(!iter.Valid()) throw std::runtime_error("SSTable::iterator::Iterator: empty data block");

    // 成功才整体移动发布
    currentBlock_ = std::move(block);
    blockIterator_ = std::move(iter);
}

bool SSTableReader::Iterator::Valid() const
{
    return blockIterator_.has_value() && blockIterator_->Valid();       //  当前没有块可读时：Next()中置空blockIterator_    当前块读完了：blockIterator_无效
}

const InternalKey& SSTableReader::Iterator::Key() const
{
    assert(Valid());
    return blockIterator_->Key();
}

const std::string& SSTableReader::Iterator::Value() const
{
    assert(Valid());
    return blockIterator_->Value();
}

// ! 注意实现思路
void SSTableReader::Iterator::Next()
{
    // 当前的迭代器是有效的 -> 移动到下一节点
    assert(Valid());
    blockIterator_->Next();
    if(blockIterator_->Valid()) return;     // 仍有效 不需要更换datablock

    // 当前块结束，先撤下块内迭代器
    blockIterator_.reset();
    blockIndex_++;

    // 已经没有块可读：撤下DataBlockReader引用
    if(blockIndex_ >= owner_->index_.size())
    {
        currentBlock_.reset();
        return;
    }

    // 仍有块可读
    const auto& idx = owner_->index_[blockIndex_];
    auto block = owner_->LoadDataBlock(idx.offset, idx.size);
    auto iter = block->Begin();

    // 空块报错
    if(!iter.Valid()) throw std::runtime_error("SSTableReader::Iterator::Next: empty data block");

    // 成功才整体移动发布
    currentBlock_ = std::move(block);
    blockIterator_ = std::move(iter);
}

