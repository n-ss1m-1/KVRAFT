// mem_table.h
#pragma once

#include<string>
#include<cstdint>

#include "skip_list.h"

enum class LookupState
{
    kNotFound,
    kValue,
    kDeleted
};

struct LookupResult
{
    LookupState state = LookupState::kNotFound;
    std::string value;
};


class MemTable : private lsm::NonCopyable
{
public:
    using Iterator = SkipList::Iterator;

    MemTable() = default;
    ~MemTable() = default;

    bool Add(uint64_t sequence, ValueType type, const std::string& user_key, const std::string& value);     // 字符串：&
    LookupResult Get(uint64_t sequence, const std::string& user_key) const;

    Iterator Begin() const;
    size_t Size() const noexcept;
    bool Empty() const noexcept;

    size_t ApproximateMemoryUsage() const noexcept;

    void Freeze() noexcept;
    bool IsFrozen() const noexcept;
private:
    SkipList skipList_;     // skiplist类内锁?
    bool frozen_ = false;   // memtable锁?
};
