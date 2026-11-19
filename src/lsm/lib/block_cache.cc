// block_cache.cc
#include <stdexcept>

#include "block_cache.h"

bool BlockCacheKey::operator==(const BlockCacheKey& other) const noexcept 
{
    return this->fileNumber==other.fileNumber && this->offset==other.offset;
}

// ! 注意实现方法
size_t BlockCacheKeyHash::operator()(const BlockCacheKey& key) const noexcept 
{
    const size_t h1 = std::hash<uint64_t>{}(key.fileNumber);
    const size_t h2 = std::hash<uint64_t>{}(key.offset);

    return h1 ^ (h2 + static_cast<size_t>(0x9e3779b9u))
            + (h1 << 6) + (h1>>2);
}

BlockCache::BlockCache(size_t capacityBytes)
    : capacityBytes_(capacityBytes)
{
}

std::shared_ptr<const DataBlockReader> BlockCache::Get(const BlockCacheKey& key) 
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    
    // 未找到: 返回空指针
    if(it == index_.end()) return nullptr;
    // 找到了: 将相应Node提到链表头部 返回reader
    lru_.splice(lru_.begin(), lru_, it->second);

    return it->second->reader;
}

void BlockCache::Insert(const BlockCacheKey& key, std::shared_ptr<const DataBlockReader> reader, size_t charge) 
{
    // 参数合法性检查：避免插入空指针 查询时误认为未插入
    if(reader == nullptr) throw std::invalid_argument("BlockCache::Insert: reader is null");

    std::lock_guard<std::mutex> lock(mutex_);

    // ! 插入前先查重(不重复插入 避免usage_与实际不一致)
    auto it = index_.find(key);
    if(it != index_.end())      // 1. 该节点已存在：更新
    {
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }
    else    // 2. 该节点未存在：创建新节点和索引
    {
        // ! 2.1. 容量=0  ||  超大块直接跳过 而不应该淘汰已有缓存
        if(capacityBytes_ == 0 || charge > capacityBytes_) return;

        // 2.2. 先预淘汰(前提：链表不为空)：从链表尾部+缓存表中一起删除
        while(!lru_.empty() && (usage_ > capacityBytes_ - charge))
        {
            const auto& current = lru_.back();
            index_.erase(current.key);
            usage_ -= current.charge;
            lru_.pop_back();
        }

        // 2.3. 剩余容量足够：插入链表头部+插入缓存表
        if(usage_ + charge <= capacityBytes_)
        {
            lru_.push_front(Node{key, std::move(reader), charge});       // ! insert返回迭代器，emplace_front返回Node&
            try{
                index_[key] = lru_.begin();
            } catch (...){
                lru_.pop_front();       // ! 回滚：保持链表和缓存表为一致状态
                throw;
            }
            usage_ += charge;
        }   
    }
}

size_t BlockCache::CapacityBytes() const noexcept 
{
    return capacityBytes_;
}

size_t BlockCache::Usage() const 
{
    std::lock_guard<std::mutex> lock(mutex_);       // ! 共享变量读取：加锁
    return usage_;
}




