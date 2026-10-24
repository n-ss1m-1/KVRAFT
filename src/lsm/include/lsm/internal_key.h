// internal_key.h

#pragma once
#include <cstdint>
#include <string>

enum class ValueType : uint8_t
{
    kValue      = 1,
    kDeletion   = 2
};

struct InternalKey {
    std::string user_key;
    uint64_t sequence = 0;
    ValueType type = ValueType::kValue;
};

class InternalKeyComparator {
public:
    int Compare(const InternalKey& lhs, const InternalKey& rhs) const noexcept;
    bool operator()(const InternalKey& lhs, const InternalKey& rhs) const noexcept {
        return Compare(lhs, rhs) < 0;       //! true代表: lhs 排在 rhs 前面
    }
};
