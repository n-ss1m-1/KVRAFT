// wal.h
#pragma once
#include <unistd.h>         // read、close
#include <fcntl.h>          // open、O_RDONLY、O_CLOEXEC
#include <string>
#include <string_view>
#include <cstdint>
#include <cerrno>           // errno、EINTR
#include <system_error>     // std::error_code

#include "coding.h"
#include "noncopyable.h"


enum class WalReadStatus
{
    kRecord,
    kEndOfFile,
    kTruncatedTail,
    kCorruption,
    kIOError
};

class WalReader : private lsm::NonCopyable
{
public:
    explicit WalReader(const std::string& path);
    ~WalReader();

    WalReadStatus ReadNext(lsm::Entry& record);
    uint64_t LastValidOffset() const noexcept;

private:
    int fd_ = -1;
    uint64_t offset_ = 0;               // 已经读取的字节数
    uint64_t lastValidOffset_ = 0;      // 最后一条有效记录的结束位置
};

// enum class WalWriteStatus
// {
//     kOk,
//     kInvalidArgument,
//     kRecordTooLarge,
//     kIOError,
//     kFailedState
// };

// struct WalWriteResult
// {
//     WalWriteStatus status = WalWriteStatus::kOk;
//     std::error_code error;
// };

enum class WalWriterOpenMode
{
    kAppend,                // 不存在则创建，存在则追加
    kCreateNew              // 仅创建新文件，存在则报错(创建独占文件)
};

class WalWriter : private lsm::NonCopyable
{
public:
    explicit WalWriter(const std::string& path, WalWriterOpenMode mode = WalWriterOpenMode::kAppend);
    ~WalWriter();

    /*
    参数非法、记录超限：抛异常。
    文件操作失败：返回对应系统错误。
    成功：返回空 error_code。
    */
    std::error_code Append(const lsm::Entry& record);
    std::error_code Sync();

private:
    int fd_ = -1;
};

