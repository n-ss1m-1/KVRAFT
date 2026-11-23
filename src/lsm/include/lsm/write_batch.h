// write_batch.h
#pragma once
#include <string_view>
#include <vector>

#include "internal_key.h"
#include "coding.h"

class WriteBatch
{
public:
    WriteBatch() = default;

    void Put(std::string_view user_key, std::string_view value);
    void Delete(std::string_view user_key);

    // 仅用于选组预算，不检查合法性；累加溢出时饱和为size_t最大值
    // 单个超预算请求仍可独立成组，合法性由WriteGroup统一检查
    size_t EstimatedWalRecordSize() const noexcept;

    bool Empty() const noexcept;
    size_t Count() const noexcept;
    void Clear() noexcept;
private:
    friend class DB;
    std::vector<lsm::Entry> operations_;        // ! sequence保持默认，B::Write()检查后才逐项分配
};
