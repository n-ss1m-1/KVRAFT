// bloom_filter.h
#pragma once

#include <vector>
#include <string_view>
#include <cstdint>

#include "noncopyable.h"

// 判断某个SSTable中是否存在某个user_key(一定不存在 or 可能存在)
class BloomFilter : private lsm::NonCopyable
{
public:
    // 创建一个 全新的空过滤器
    BloomFilter(size_t keyCount, size_t bitsPerKey = 10, size_t hashCount = 3);
    // 恢复已有过滤器
    BloomFilter(uint32_t bitCount, uint32_t hashCount, std::vector<uint8_t> bits);

    ~BloomFilter() = default;

    void Add(std::string_view userKey);
    bool MayContain(std::string_view userKey) const;

    // bitCount() = bitCount_ = keyCount * bitsPerKey
    uint32_t BitCount() const noexcept;
    uint32_t HashCount() const noexcept;
    // ByteSize() = bits_.size() = (bitCount()+7)/8;
    size_t ByteSize() const noexcept;
    const std::vector<uint8_t>& Bits() const noexcept;

private:
    uint64_t Fnv1a(std::string_view userKey, uint64_t seed) const;
    
    uint32_t bitCount_ = 0;           // 对于每个 key -> 新增 10bit 到位图中
    uint32_t hashCount_ = 0;
    std::vector<uint8_t> bits_;
};


