// internal_key.cc

#include "lsm/internal_key.h"

int InternalKeyComparator::Compare(const InternalKey& lhs, const InternalKey& rhs) const noexcept
{
    const int key_cmp = lhs.user_key.compare(rhs.user_key);

    if (key_cmp != 0) return key_cmp;
    if (lhs.sequence > rhs.sequence) return -1;
    if (lhs.sequence < rhs.sequence) return 1;

    return 0;
}
