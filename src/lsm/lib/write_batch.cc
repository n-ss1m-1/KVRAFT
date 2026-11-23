// write_batch.cc
#include <limits>

#include "wal.h"
#include "write_batch.h"

void WriteBatch::Put(std::string_view user_key, std::string_view value)
{
    lsm::Entry entry;
    entry.key.type = ValueType::kValue;
    entry.key.user_key = user_key;
    entry.value = value;
    operations_.push_back(std::move(entry));
}

void WriteBatch::Delete(std::string_view user_key)
{
    lsm::Entry entry;
    entry.key.type = ValueType::kDeletion;
    entry.key.user_key = user_key;
    operations_.push_back(std::move(entry));
}

size_t WriteBatch::EstimatedWalRecordSize() const noexcept
{
    // WAL头部 + count + Σ(Entry固定部分 + user_key长度 + value长度)
    size_t byteSize = lsm::kWalHeaderSize + 4;

    // 局部的饱和累加函数，上溢时直接取size_t的MAX
    constexpr size_t kMax = std::numeric_limits<size_t>::max();
    auto Add = [&](size_t len)
    {
        if(len > kMax - byteSize) byteSize = kMax;
        else byteSize += len;
    };

    for(const auto& entry : operations_)
    {
        Add(lsm::kWalMinEntrySize);
        Add(entry.key.user_key.size());
        Add(entry.value.size());
    }

    return byteSize;
}

bool WriteBatch::Empty() const noexcept
{
    return operations_.empty();
}

size_t WriteBatch::Count() const noexcept
{
    return operations_.size();
}

void WriteBatch::Clear() noexcept
{
    operations_.clear();
}
