// coding.h
#pragma once
#include <string>
#include <string_view>      // 直接包含这些头文件 避免依赖其他头文件间接引入
#include <cstdint>
#include <cstddef>

#include "internal_key.h"

namespace lsm 
{
    struct Entry
    {
        InternalKey key;            // sequence + type + user_key
        std::string value;
    };

    // ! Entry采用新版布局，不兼容旧版无版本号的WAL payload
    constexpr uint32_t kWalMinEntrySize = 17;
    constexpr uint32_t kWalMaxEntrySize = 16 * 1024 * 1024;

    // 追加x位二进制数据(小端字节序 原低位字节放左边)
    void AppendFixed32(std::string& out, uint32_t value);
    void AppendFixed64(std::string& out, uint64_t value);

    // 解析x位二进制数据
    uint32_t DecodeFixed32(const char* data);           // 调用方保证data >= 4字节
    uint64_t DecodeFixed64(const char* data);           // 调用方保证data >= 8字节

    // 生成校验码
    uint32_t Crc32c(std::string_view bytes);        // string_view借用


    /* 
    二进制布局：
        - InternalKey：[sequence：8 B][type：1 B][key_len：4 B][user_key]
        - Entry：      [sequence：8 B][type：1 B][key_len：4 B][user_key][value_len：4 B][value]
    
    编码 or 解析：
        - 编码原则：先验证字段，再检查长度，最后按序拼接
        - 解析原则：先检查长度，再读取字段，再验证字段；全部验证成功，再修改输出
    */
    constexpr size_t kMinInternalKeySize = 13;              // sequence + type + key_len

    size_t EncodedInternalKeySize(const InternalKey& key);  // sequence + type + key_len + user_key.size()

    std::string EncodeInternalKey(const InternalKey& key);
    bool DecodeInternalKey(std::string_view input, InternalKey& key, size_t& bytesRead);
    // 从input开头解析一条InternalKey，剩余字节由调用方继续解析
    // ! 返回false时，不修改key和bytesRead；成功时bytesRead表示本条编码占用的字节数

    
    // 校验字段并安全计算编码大小；非法输入抛 invalid_argument
    size_t EncodedEntrySize(const Entry& entry);            // (sequence + type + key_len + user_key.size()) + value_len + value.size()

    std::string EncodeEntry(const Entry& entry);
    bool DecodeEntry(std::string_view input, Entry& entry);     // string_view借用

    // 批量解析专用
    // 一批WAL的格式(taggedLength首位1 旧版为0)： [taggedLength: 4 B][CRC32C: 4 B][count: 4 B][entry][entry][entry]...
    bool DecodeEntryPrefix(std::string_view input, Entry& entry, size_t& bytesRead);
} // namespace lsm
