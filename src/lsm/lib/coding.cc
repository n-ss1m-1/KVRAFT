// coding.cc

#include <stdexcept>
#include <climits>   // UINT32_MAX
#include <array>

#include "coding.h"


namespace 
{
    constexpr uint32_t kCrc32cPolynomial = 0x82F63B78u;

    constexpr std::array<uint32_t, 256> MakeCrc32cTable() 
    {
        std::array<uint32_t, 256> table{};
        for(uint32_t i=0; i<256; i++)
        {
            uint32_t x = i;             // 使用时下标i = (crc ^ byte) & 0xFF
            for(int j=0; j<8; j++)
            {
                if(x&1) x = (x >> 1) ^ kCrc32cPolynomial;
                else    x >>= 1;
            }
            table[i] = x;               // table[i] = Step8(Low8)
        }

        return table;
    }

    constexpr auto kCrc32cTable = MakeCrc32cTable();
}


void lsm::AppendFixed32(std::string& out, uint32_t value) 
{
    for(int i=0; i<4; i++)
    {
        uint8_t byte = static_cast<uint8_t>(value & 0xFFu);        // &运算取最低8位 转为uint8_t
        out.push_back(static_cast<char>(byte));                 // uint8_t转char 插入out
        value >>= 8;                                               // 移除最低位
    }
}

void lsm::AppendFixed64(std::string& out, uint64_t value) 
{
    for(int i=0; i<8; i++)
    {
        uint8_t byte = static_cast<uint8_t>(value & 0xFFu);
        out.push_back(static_cast<char>(byte));
        value >>= 8;
    }
}

/* !为什么先取为unsigned char:
 char 在很多编译器是有符号的。
 如果字节是 0x80 ~ 0xFF（大于 127），有符号 char 是负数。
 直接拿去移位、转 uint32 会发生符号扩展，高位全补 1，直接污染结果。
 先转 unsigned char，把它当成纯 0~255 的无符号字节，消除负数问题。
*/
uint32_t lsm::DecodeFixed32(const char* data) 
{
    uint32_t value = 0;                                             // !一定要初始化为0 否则会有垃圾值
    for(int i=0; i<4; i++)
    {
        unsigned char byte = static_cast<unsigned char>(data[i]);       // 小端字节序 低位在左边     
        value |= static_cast<uint32_t>(byte) << (8*i);                  // 需要移位 所以用uint32_t 而非8位的
    }
    return value;
}

uint64_t lsm::DecodeFixed64(const char* data) 
{
    uint64_t value = 0;
    for(int i=0; i<8; i++)
    {
        unsigned char byte = static_cast<unsigned char>(data[i]);
        value |= static_cast<uint64_t>(byte) << (8*i);
    }
    return value;
}

// !注意实现思路
// uint32_t lsm::Crc32c(std::string_view bytes) 
// {
//     uint32_t crc = 0xFFFFFFFFu;                     // 中间结果 初始化为 0xFFFFFFFFu
//     for(char c : bytes)
//     {
//         crc ^= static_cast<unsigned char>(c);       // c先转成unsigned char 再与中间结果异或 (!避免有符号char)
//         for(int bit=0; bit<8; bit++)                // (每处理1字节 循环8次)中间结果最低位为1：先右移一位 再异或0x82F63B78u   中间结果最低位为0：仅右移一位  
//         {
//             if(crc & 1)                     // !注意:不能精简，先移位再判断(×)-先判断再移位(√) 结果不同
//             {
//                 crc >>= 1;
//                 crc ^= 0x82F63B78u;
//             }
//             else
//             {
//                 crc >>= 1;
//             }
//         }
//     }
//     return ~crc;            // 最后按位取反
// }

// 查表优化版
uint32_t lsm::Crc32c(std::string_view bytes) 
{
    uint32_t crc = 0xFFFFFFFFu;
    for(char ch : bytes)
    {
        uint32_t x = crc ^ static_cast<unsigned char>(ch);
        crc = (x >> 8) ^ (kCrc32cTable[x & 0xFFu]);
    }
    return ~crc;            // ! 最后取反
}

size_t lsm::EncodedInternalKeySize(const InternalKey& key)
{
    if(key.sequence == 0) throw std::invalid_argument("EncodeInternalKey: sequence cannot be zero");
    if(key.type != ValueType::kValue && key.type != ValueType::kDeletion) throw std::invalid_argument("EncodeInternalKey: invalid ValueType");
    if(key.user_key.size() > UINT32_MAX || key.user_key.size() > SIZE_MAX - kMinInternalKeySize) throw std::invalid_argument("EncodeInternalKey: key length overflow");
    
    return kMinInternalKeySize + key.user_key.size();
}

std::string lsm::EncodeInternalKey(const InternalKey& key)
{
    std::string encoded;
    encoded.reserve(EncodedInternalKeySize(key));

    AppendFixed64(encoded, key.sequence);
    encoded.push_back(static_cast<char>(key.type));
    AppendFixed32(encoded, static_cast<uint32_t>(key.user_key.size()));
    encoded.append(key.user_key);

    return encoded;
}

bool lsm::DecodeInternalKey(std::string_view input, InternalKey& key, size_t& bytesRead)
{
    if(input.size() < kMinInternalKeySize) return false;

    InternalKey decoded;

    decoded.sequence = DecodeFixed64(input.data());
    decoded.type = static_cast<ValueType>(static_cast<unsigned char>(input[8]));
    const uint32_t keyLength = DecodeFixed32(input.data() + 9);

    if(decoded.sequence == 0 || (decoded.type != ValueType::kValue && decoded.type != ValueType::kDeletion)) return false;
    if(keyLength > input.size() - kMinInternalKeySize) return false;

    decoded.user_key = input.substr(kMinInternalKeySize, keyLength);
    key = std::move(decoded);
    bytesRead = kMinInternalKeySize + static_cast<size_t>(keyLength);

    return true;
}

size_t lsm::EncodedEntrySize(const Entry& entry)
{
    const size_t keySize = EncodedInternalKeySize(entry.key);
    if(entry.key.type == ValueType::kDeletion && !entry.value.empty()) throw std::invalid_argument("EncodeEntry: deletion entry must have empty value");
    
    // ! 使用减法检查剩余空间，避免先相加发生长度溢出
    if(keySize > kWalMaxEntrySize - 4 || entry.value.size() > kWalMaxEntrySize - 4 - keySize) throw std::invalid_argument("EncodeEntry: payload exceeds kWalMaxEntrySize");
    
    return keySize + 4 + entry.value.size();
}

std::string lsm::EncodeEntry(const Entry& entry)
{
    const size_t size = EncodedEntrySize(entry);

    std::string encoded = EncodeInternalKey(entry.key);

    encoded.reserve(size);
    AppendFixed32(encoded, static_cast<uint32_t>(entry.value.size()));
    encoded.append(entry.value);

    return encoded;
}

bool lsm::DecodeEntry(std::string_view input, Entry& entry)
{
    if(input.size() < kWalMinEntrySize || input.size() > kWalMaxEntrySize) return false;
    
    Entry decoded;

    size_t keySize = 0;
    if(!DecodeInternalKey(input, decoded.key, keySize)) return false;

    if(input.size() - keySize < 4) return false;
    const uint32_t valueLength = DecodeFixed32(input.data() + keySize);

    if(valueLength != input.size() - keySize - 4) return false;
    if(decoded.key.type == ValueType::kDeletion && valueLength != 0) return false;
    decoded.value = input.substr(keySize + 4, valueLength);

    entry = std::move(decoded);
    
    return true;
}
