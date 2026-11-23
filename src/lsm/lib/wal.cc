// wal.cc
#include <limits>

#include "wal.h"


// ! 检查：批次非空、条数可编码、sequence 连续且不溢出、每条 Entry 合法、总 payload 不超长度限制
size_t lsm::EncodeBatchPayloadSize(const std::vector<lsm::Entry>& entries)
{
    if(entries.empty()) throw std::invalid_argument("EncodeBatchPayload: empty batch");
    if(entries.size() > std::numeric_limits<uint32_t>::max()) throw std::invalid_argument("EncodeBatchPayload: entry count overflow uint32");

    uint32_t payloadSize = sizeof(uint32_t);    // count的大小
    uint64_t previous = entries.front().key.sequence - 1;
    for(const auto& e : entries)
    {
        if(previous == std::numeric_limits<uint64_t>::max() || e.key.sequence != previous + 1) throw std::invalid_argument("EncodeBatchPayload: batch sequence not contiguous");
        previous = e.key.sequence;
        const uint32_t entrySize = lsm::EncodedEntrySize(e);
        if(entrySize > kWalMaxBatchPayloadSize - payloadSize) throw std::invalid_argument("EncodeBatchPayload: payload too large");
        payloadSize += entrySize;
    }

    return payloadSize;
}

namespace
{
    // ! 编码：[taggedLength: 4 B][CRC32C: 4 B] + 组合已编码的 payload([entry] / [count][entries])
    std::string EncodeWalRecord(std::string payload, lsm::WalRecordKind kind)
    {
        std::string encoded;
        uint32_t taggedLength = payload.size();
        if(kind == lsm::WalRecordKind::kBatch) taggedLength |= lsm::kWalBatchFlag;         // ! 长度最高位=1：批量WAL

        encoded.reserve(lsm::kWalHeaderSize + payload.size());       // ! Batch模式：taggedLength != payload.size()
        lsm::AppendFixed32(encoded, taggedLength);
        lsm::AppendFixed32(encoded, lsm::Crc32c(encoded + payload));        //! CRC32C检查: length+payload
        encoded.append(payload);
        
        return encoded;
    }

    /*
     二进制布局：
        - Batch:  [taggedLength: 4 B][CRC32C: 4 B][count: 4 B][entry][entry][entry]...
        - Single: [taggedLength: 4 B][CRC32C: 4 B][entry]
        - Entry： [sequence：8 B][type：1 B][key_len：4 B][user_key][value_len：4 B][value]
     ! 这里负责编码：[count: 4B][entry][entry]...
    */
    std::string EncodeBatchPayload(const std::vector<lsm::Entry>& entries)
    {
        // 检查batch 并计算payload长度
        size_t payloadSize = EncodeBatchPayloadSize(entries);

        // reserve + 拼接二进制数据
        std::string payload;
        payload.reserve(payloadSize);
        lsm::AppendFixed32(payload, entries.size());
        for(const auto& e : entries) payload.append(lsm::EncodeEntry(e));

        return payload;
    }

    // ! 读取count + 解析entries
    bool DecodeBatchPayload(std::string_view payload, std::vector<lsm::Entry>& entries)
    {
        // ! 检查：entryCount不为0，剩余payload可装下这么多entry，批次Sequence连续且不回绕、解析完成没有多余字节
        // 读取count
        if(payload.size() < 4) return false;
        uint32_t entryCount = lsm::DecodeFixed32(payload.data());
        if(entryCount == 0 || entryCount > (payload.size()-4) / lsm::kWalMinEntrySize) return false;

        // 读取entries
        lsm::Entry entry;
        size_t bytesRead;
        std::string_view remaining = payload.substr(sizeof(entryCount));    // !跳过count

        std::vector<lsm::Entry> decoded;
        for(uint32_t i=0; i<entryCount; i++)
        {
            if(!lsm::DecodeEntryPrefix(remaining, entry, bytesRead)) return false;
            if(!decoded.empty())       // !检查批次内 sequence 连续
            {
                const auto previous = decoded.back().key.sequence;
                if(previous == std::numeric_limits<uint64_t>::max() || entry.key.sequence != previous + 1) return false;
            }
            decoded.push_back(entry);
            remaining.remove_prefix(bytesRead);
        }
        if(!remaining.empty()) return false;

        entries = std::move(decoded);

        return true;
    }

    // ! 检查 taggedLength(->kind + payloadSize) 和 CRC32C，依据kind使用不同Decode工具
    bool DecodeWalRecord(std::string_view encoded, std::vector<lsm::Entry>& entries)          // string_view借用
    {
        // 1. 保证至少 kWalHeaderSize 字节，才能读取length和crc
        if(encoded.size() < lsm::kWalHeaderSize) return false;

        // 2. 读取length和crc
        const uint32_t taggedLength = lsm::DecodeFixed32(encoded.data());
        lsm::WalRecordKind kind = ((taggedLength & lsm::kWalBatchFlag) != 0) ? lsm::WalRecordKind::kBatch : lsm::WalRecordKind::kSingle;       // ! 最高位置位后的结果是 0x80000000
        const uint32_t payloadSize = taggedLength & ~lsm::kWalBatchFlag;
        const uint32_t crc = lsm::DecodeFixed32(encoded.data() + 4);

        // 3. 检查length在合法范围内 + length==encoded剩余长度
        if((kind == lsm::WalRecordKind::kSingle && (payloadSize < lsm::kWalMinEntrySize || lsm::kWalMaxEntrySize < payloadSize))
        || (kind == lsm::WalRecordKind::kBatch && (payloadSize < lsm::kWalMinBatchPayloadSize || lsm::kWalMaxBatchPayloadSize < payloadSize))) return false;
        if(payloadSize != encoded.size()-lsm::kWalHeaderSize) return false;

        // 4. 解析payload
        const auto payload = encoded.substr(lsm::kWalHeaderSize);     // 从index=kWalHeaderSize开始截取

        // 5. 根据length和payload重新计算crc
        std::string checkStr(encoded.substr(0,4));
        checkStr.append(payload);
        if(crc != lsm::Crc32c(checkStr)) return false;

        // 6. 解析(先放到临时decoded，成功才移动到entries传出)
        std::vector<lsm::Entry> decoded(1);
        if(kind == lsm::WalRecordKind::kSingle && !lsm::DecodeEntry(payload, decoded[0])) return false;           // const char* -> string_view 时，会查找 '\0' 来确定长度。而 sequence 等二进制字段很容易包含零字节，因此会被截断；如果找不到零字节，还可能越界读取。 && 此前你已经提取了带长度的 payload，直接传它
        else if(kind == lsm::WalRecordKind::kBatch && !DecodeBatchPayload(payload, decoded))  return false;

        entries = std::move(decoded);
        return true;
    }

}


WalReader::WalReader(const std::string& path)
{
    // ! DB判断需要恢复流程(日志文件存在) 才使用Reader -> 故Reader不需要创建文件，如果此时打不开文件，即为错误
    fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC);     //! Reader不负责创建文件
    if(fd_ == -1) throw std::system_error(errno, std::generic_category(), "open walReader file failed");
}

WalReader::~WalReader()
{
    if(fd_ >= 0) close(fd_);
    fd_ = -1;
}

//! 注意实现逻辑
WalReadStatus WalReader::ReadNext(std::vector<lsm::Entry>& entries)
{
    // 1. 循环读取kWalHeaderSize字节头部
    std::string header;
    header.resize(lsm::kWalHeaderSize);       //!一定要先分配内存 才能读取
    size_t done = 0;
    while(done < lsm::kWalHeaderSize)
    {
        ssize_t n = read(fd_, header.data()+done, lsm::kWalHeaderSize-done);
        if(n == 0)
        {
            if(done == 0) return WalReadStatus::kEndOfFile;
            else return WalReadStatus::kTruncatedTail;
        }
        if(n < 0)
        {
            if(errno == EINTR) continue;
            return WalReadStatus::kIOError;
        }
        done += n;
        offset_ += n;
    }

    // 2. 校验length是否合法(根据WAL记录类型 使用不同的参数检查)
    const uint32_t taggedLength = lsm::DecodeFixed32(header.data());
    const bool isBatch = (taggedLength & lsm::kWalBatchFlag) != 0;
    const uint32_t payloadSize = taggedLength & ~lsm::kWalBatchFlag;

    if((!isBatch && (payloadSize < lsm::kWalMinEntrySize || lsm::kWalMaxEntrySize < payloadSize))
      ||(isBatch && (payloadSize < lsm::kWalMinBatchPayloadSize || lsm::kWalMaxBatchPayloadSize < payloadSize)))
        return WalReadStatus::kCorruption;

    // 3. 分配string 插入payloadSize + 循环读取payload
    std::string encoded(header);                    // !先插入header
    encoded.resize(lsm::kWalHeaderSize + payloadSize);     // !再扩充容量(不会影响header部分的内容)

    while(done < encoded.size())
    {
        ssize_t n = read(fd_, encoded.data()+done, encoded.size()-done);
        if(n == 0)
        {
            return WalReadStatus::kTruncatedTail;   // 只要payload尚未读满 就遇到EOF 一律返回kTruncatedTail
        }
        if(n < 0)
        {
            if(errno == EINTR) continue;
            return WalReadStatus::kIOError;
        }
        done += n;
        offset_ += n;
    }

    // 4. 解析 和 校验
    if(!DecodeWalRecord(encoded,entries))
    {
        return WalReadStatus::kCorruption;
    }

    // 5. 更新lastValidOffset_   (offset_是在read的时候 实时更新的)
    lastValidOffset_ = offset_;

    return WalReadStatus::kRecord;
}

uint64_t WalReader::LastValidOffset() const noexcept 
{
    return lastValidOffset_;
}

WalWriter::WalWriter(const std::string& path, WalWriterOpenMode mode)
{
    //! kAppend    <- db前台写入前台刷盘，更新后只用于测试
    //  kCreateNew <- db前台写入后台刷盘-使用段式WAL
    int flags;
    if(mode == WalWriterOpenMode::kAppend) flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
    else if(mode == WalWriterOpenMode::kCreateNew) flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;         // ! O_EXCL: 仅创建新文件，存在则报错(创建独占文件)
    
    fd_ = open(path.c_str(), flags, 0644); 

    if(fd_ < 0) throw std::system_error(errno, std::generic_category(), "open walWriter file failed");
}

WalWriter::~WalWriter()
{
    if(fd_ >= 0) close(fd_);
    fd_ = -1;
}

std::error_code WalWriter::Append(const lsm::Entry& entry)
{
    std::string encoded = EncodeWalRecord(lsm::EncodeEntry(entry), lsm::WalRecordKind::kSingle);

    return WriteRecord(encoded);
}

// [taggedLength: 4 B][CRC32C: 4 B][count: 4 B][entry][entry][entry]...
std::error_code WalWriter::Append(const std::vector<lsm::Entry>& entries)
{
    std::string encoded = EncodeWalRecord(EncodeBatchPayload(entries), lsm::WalRecordKind::kBatch);

    return WriteRecord(encoded);
}

std::error_code WalWriter::Sync()
{
    while(fsync(fd_) < 0)
    {
        if(errno == EINTR) continue;
        return std::error_code(errno, std::generic_category());
    }
    return {};
}

std::error_code WalWriter::WriteRecord(std::string_view encoded)
{
    size_t done = 0;
    while(done < encoded.size())
    {
        ssize_t n = write(fd_, encoded.data()+done, encoded.size()-done);
        if(n == 0)
        {
            return std::make_error_code(std::errc::io_error);       // 一般不会发生，仅当要求读取的字节=0时会发生(逻辑错误)
        }
        if(n < 0)
        {
            if(errno == EINTR) continue;
            return std::error_code(errno, std::generic_category()); 
        }
        done += n;
    }

    return {};
}
