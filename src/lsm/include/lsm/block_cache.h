// block_cache.h
#pragma once
#include <cstdint>
#include <memory>
#include <list>
#include <unordered_map>
#include <mutex>

#include "data_block.h"
#include "noncopyable.h"


struct BlockCacheKey
{
    uint64_t fileNumber;
    uint64_t offset;

    bool operator==(const BlockCacheKey& other) const noexcept;
};

// unordered_map不清楚自定义结构体的hash计算方法，需要自己定义
struct BlockCacheKeyHash
{
    size_t operator()(const BlockCacheKey& key) const noexcept;
};


class BlockCache : private lsm::NonCopyable
{
public:
    explicit BlockCache(size_t capacityBytes);
    std::shared_ptr<const DataBlockReader> Get(const BlockCacheKey& key);
    void Insert(const BlockCacheKey& key, std::shared_ptr<const DataBlockReader> reader, size_t charge);

    size_t CapacityBytes() const noexcept;
    size_t Usage() const;

private:
    struct Node
    {
        BlockCacheKey key;                              // Node也需要key值，在淘汰的同时删除unordered_map的键
        std::shared_ptr<const DataBlockReader> reader;
        size_t charge;
    };

    std::list<Node> lru_;
    std::unordered_map<BlockCacheKey, std::list<Node>::iterator, BlockCacheKeyHash> index_;

    size_t capacityBytes_ = 0;
    size_t usage_ = 0;
    mutable std::mutex mutex_;
};

/*
新增块缓存后的db读取顺序：
- 块缓存在memtable后面才读取
- 每张 SSTable 按原顺序查询，在读取候选块时查缓存(判断是否命中)；避免更新的sstable/块内有相应记录，却读到了旧的块缓存

active → immutable → 从新到旧检查 SSTable
                       └─ 每张表：Bloom → 索引定位块
                                          └─ 查块缓存
                                              ├─ 命中：使用 Reader
                                              └─ 未命中：读文件、验证、放入缓存
*/