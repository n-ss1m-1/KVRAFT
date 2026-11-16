// wal.cc

#include "wal.h"

namespace 
{
    constexpr uint32_t kWalHeaderSize = 8;

    // 二进制布局： record: [payload_length：4 B][CRC32C：4 B][payload：length B]
    std::string EncodeWalRecord(const lsm::Entry& entry) 
    {
        const std::string payload = lsm::EncodeEntry(entry);
        
        std::string encoded;
        encoded.reserve(8 + payload.size());
        lsm::AppendFixed32(encoded, payload.size());
        lsm::AppendFixed32(encoded, lsm::Crc32c(encoded + payload));        //! CRC32C检查: length+payload
        encoded.append(payload);
        
        return encoded;
    }

    // !注意实现思路
    bool DecodeWalRecord(std::string_view encoded, lsm::Entry& entry)          // string_view借用
    {
        // 1. 保证至少 8 字节，才能读取length和crc
        if(encoded.size() < 8) return false;

        // 2. 读取length和crc
        const uint32_t payloadLength = lsm::DecodeFixed32(encoded.data());
        const uint32_t crc = lsm::DecodeFixed32(encoded.data() + 4);

        // 3. 检查length在合法范围内 + length==encoded剩余长度
        if(payloadLength < lsm::kWalMinEntrySize || lsm::kWalMaxEntrySize < payloadLength) return false;
        if(payloadLength != encoded.size()-8) return false;

        // 4. 解析payload
        const auto payload = encoded.substr(8);     // 从index=8开始截取

        // 5. 根据length和payload重新计算crc
        std::string checkStr(encoded.substr(0,4));
        checkStr.append(payload);
        if(crc != lsm::Crc32c(checkStr)) return false;

        return lsm::DecodeEntry(payload, entry);           // const char* -> string_view 时，会查找 '\0' 来确定长度。而 sequence 等二进制字段很容易包含零字节，因此会被截断；如果找不到零字节，还可能越界读取。 && 此前你已经提取了带长度的 payload，直接传它
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
WalReadStatus WalReader::ReadNext(lsm::Entry& entry) 
{
    // 1. 循环读取8字节头部
    std::string header;
    header.resize(kWalHeaderSize);       //!一定要先分配内存 才能读取
    size_t done = 0;
    while(done < kWalHeaderSize)
    {
        ssize_t n = read(fd_, header.data()+done, kWalHeaderSize-done);        
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

    // 2. 校验length是否合法
    uint32_t length = lsm::DecodeFixed32(header.data());
    if(length < lsm::kWalMinEntrySize || lsm::kWalMaxEntrySize < length) return WalReadStatus::kCorruption;

    // 3. 分配string 插入length + 循环读取payload
    std::string encoded(header);                    // !先插入header
    encoded.resize(kWalHeaderSize + length);     // !再扩充容量(不会影响header部分的内容)

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
    if(!DecodeWalRecord(encoded,entry))         //! DecodeWalRecord
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
    std::string encoded = EncodeWalRecord(entry);           //! EncodeWalRecord

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

std::error_code WalWriter::Sync()
{
    while(fsync(fd_) < 0)
    {
        if(errno == EINTR) continue;
        return std::error_code(errno, std::generic_category());
    }
    return {};
}