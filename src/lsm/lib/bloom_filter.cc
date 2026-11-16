// bloom_filter.cc
#include <stdexcept>
#include <limits>
#include <utility>          // std::move()

#include "bloom_filter.h"

namespace
{
    constexpr uint64_t kFnv64Prime = 1099511628211ull;           // FNV-1a 64bit 固定乘法素数
    constexpr uint64_t kFnv64Seed1 = 14695981039346656037ull;
    constexpr uint64_t kFnv64Seed2 = 0x9AE66E45F7CB4548ull;
}

BloomFilter::BloomFilter(size_t keyCount, size_t bitsPerKey, size_t hashCount)
{
    /* !
     keyCount == 0：允许，创建空过滤器；Add 不应使用，MayContain 返回 false
     bitsPerKey == 0：非法，抛 std::invalid_argument
     hashCount == 0：非法，抛 std::invalid_argument
    */
    if(bitsPerKey == 0) throw std::invalid_argument("bloom filter: bitsPerKey cannot be zero");
    if(hashCount == 0) throw std::invalid_argument("bloom filter: hashCount cannot be zero");

    // 检查size_t乘法是否溢出
    if(keyCount > (std::numeric_limits<uint32_t>::max()) / bitsPerKey) throw std::length_error("bloom filter: keyCount * bitsPerKey overflow, too large");
    if(hashCount > std::numeric_limits<uint32_t>::max()) throw std::length_error("BloomFilter: hashCount exceeds uint32_t limit");

    bitCount_ = static_cast<uint32_t>(keyCount * bitsPerKey);
    hashCount_ = static_cast<uint32_t>(hashCount);
    bits_.resize(bitCount_ / 8 + (bitCount_ % 8 != 0),0);           //! 需要初始化分配空间 否则后续下标访问越界  注：byte 向上取余 |  此处计算方法相较于加减法 可以避免异常
}  

BloomFilter::BloomFilter(uint32_t bitCount, uint32_t hashCount, std::vector<uint8_t> bits)      // ! 如果使用const& 则后续move无法使用移动拷贝(const要求源对象不能被修改 - 即不能被移动)
    : bitCount_(bitCount),
      hashCount_(hashCount)
{
    if(hashCount == 0) throw std::invalid_argument("BloomFilter: hashCount cannot be zero");
    size_t expectedByteSize = bitCount / 8 + (bitCount % 8 != 0);
    if(expectedByteSize != bits.size()) throw std::invalid_argument("BloomFilter: bits buffer size mismatch");

    bits_ = std::move(bits);
}

void BloomFilter::Add(std::string_view userKey) 
{
    //! keyCount == 0 : 空过滤器 Add不应该使用
    if(bitCount_ == 0) return;

    uint64_t h1 = Fnv1a(userKey, kFnv64Seed1);
    uint64_t h2 = Fnv1a(userKey, kFnv64Seed2);
    h2 |= 1u;                                           //!▲ 保证不为0 且为奇数

    for(size_t i=0; i<hashCount_; i++)                                  // !统一使用size_t ?
    {
        size_t bitPos = static_cast<size_t>((h1 + i * h2) % bitCount_); // !统一使用size_t
        size_t byteIndex = bitPos / 8;
        size_t bitOffset = bitPos % 8;

        bits_[byteIndex] |= (1u<<bitOffset);
    }
}

bool BloomFilter::MayContain(std::string_view userKey) const 
{
    //! keyCount == 0 : 空过滤器 MayContain一定为false
    if(bitCount_ == 0) return false;

    // 使用和Add相同的办法 计算: hash、index、offset
    uint64_t h1 = Fnv1a(userKey, kFnv64Seed1);
    uint64_t h2 = Fnv1a(userKey, kFnv64Seed2);
    h2 |= 1u;                                           

    for(size_t i=0; i<hashCount_; i++)
    {
        size_t bitPos = (h1 + i * h2) % bitCount_;
        size_t byteIndex = bitPos / 8;
        size_t bitOffset = bitPos % 8;

        if((bits_[byteIndex] & (1u<<bitOffset)) == 0) return false;     // 只要有一位不存在 则该key不存在
    }

    return true;
}

uint32_t BloomFilter::BitCount() const noexcept 
{
    return bitCount_;
}

uint32_t BloomFilter::HashCount() const noexcept
{
    return hashCount_;
}

size_t BloomFilter::ByteSize() const noexcept 
{
    return bits_.size();
}

const std::vector<uint8_t>& BloomFilter::Bits() const noexcept
{
    return bits_;
}

uint64_t BloomFilter::Fnv1a(std::string_view userKey, uint64_t seed) const
{
    uint64_t hash = seed;
    for(unsigned char byte : userKey)       //!? 转换为unsigned char，避免有符号char的问题
    {
        hash ^= byte;
        hash *= kFnv64Prime;            //! 使用Fnv1a固定乘法素数
    }
    return hash;
}