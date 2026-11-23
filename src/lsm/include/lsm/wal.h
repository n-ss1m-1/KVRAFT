// wal.h
#pragma once
#include <unistd.h>         // read、close
#include <fcntl.h>          // open、O_RDONLY、O_CLOEXEC
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cerrno>           // errno、EINTR
#include <system_error>     // std::error_code

#include "coding.h"
#include "noncopyable.h"

namespace lsm
{
    constexpr uint32_t kWalHeaderSize = 8;
    constexpr uint32_t kWalMinBatchPayloadSize = 4 + lsm::kWalMinEntrySize;  // count + 至少一条entry
    constexpr uint32_t kWalMaxBatchPayloadSize = 16 * 1024 * 1024;
    constexpr uint32_t kWalBatchFlag = uint32_t{1} << 31;

    enum class WalRecordKind
    {
        kSingle,            // 单条：长度最高位 0
        kBatch              // 批量：长度最高位 1
    };

    // ! 检查：批次非空、条数可编码、sequence 连续且不溢出、每条 Entry 合法、总 payload 不超长度限制
    // 返回 [count][entries...] 的编码长度
    size_t EncodeBatchPayloadSize(const std::vector<lsm::Entry>& entries);
};

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

    // 读取一条完整 WAL 记录：单条格式返回一项，批次格式返回整批
    WalReadStatus ReadNext(std::vector<lsm::Entry>& entries);
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
    std::error_code Append(const lsm::Entry& entry);
    std::error_code Append(const std::vector<lsm::Entry>& entries);
    std::error_code Sync();

private:
    std::error_code WriteRecord(std::string_view encoded);
private:
    int fd_ = -1;
};

