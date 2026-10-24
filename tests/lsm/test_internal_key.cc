#include <algorithm>
#include <iostream>
#include <vector>
#include "lsm/internal_key.h"
#include "test_util.h"
static InternalKey Key(const char* user_key, uint64_t sequence, ValueType type = ValueType::kValue) {
    return InternalKey{user_key, sequence, type};
}
void TestUserKeyAscending() {
    InternalKeyComparator cmp;
    CHECK(cmp.Compare(Key("", 5), Key("a", 1)) < 0);
    CHECK(cmp.Compare(Key("a", 1), Key("aa", 1)) < 0);
    CHECK(cmp.Compare(Key("aa", 1), Key("b", 1)) < 0);
}
void TestSequenceDescending() {
    InternalKeyComparator cmp;
    CHECK(cmp.Compare(Key("a", 20), Key("a", 10)) < 0);
    CHECK(cmp.Compare(Key("a", 10), Key("a", 20)) > 0);
}
void TestTypeDoesNotAffectOrder() {
    InternalKeyComparator cmp;
    CHECK_EQ(cmp.Compare(Key("a", 20, ValueType::kValue), Key("a", 20, ValueType::kDeletion)), 0);
}
void TestSortedOrder() {
    InternalKeyComparator cmp;
    std::vector<InternalKey> keys = {Key("b", 1), Key("a", 10), Key("a", 20, ValueType::kDeletion), Key("", 5), Key("aa", 3)};
    std::sort(keys.begin(), keys.end(), [&cmp](const InternalKey& lhs, const InternalKey& rhs) { return cmp(lhs, rhs); });
    CHECK_STR_EQ(keys[0].user_key, "");
    CHECK_STR_EQ(keys[1].user_key, "a");
    CHECK_EQ(keys[1].sequence, 20u);
    CHECK_STR_EQ(keys[2].user_key, "a");
    CHECK_EQ(keys[2].sequence, 10u);
    CHECK_STR_EQ(keys[3].user_key, "aa");
    CHECK_STR_EQ(keys[4].user_key, "b");
}
void TestStrictWeakOrderingBasics() {
    InternalKeyComparator cmp;
    const InternalKey a10 = Key("a", 10), a20 = Key("a", 20), b1 = Key("b", 1);
    CHECK(cmp(a20, a10)); CHECK(cmp(a10, b1)); CHECK(cmp(a20, b1)); CHECK(!cmp(a10, a10));
}
int main() {
    std::cout << "===== LSM InternalKey Tests =====\n";
    RUN_TEST(TestUserKeyAscending); RUN_TEST(TestSequenceDescending); RUN_TEST(TestTypeDoesNotAffectOrder); RUN_TEST(TestSortedOrder); RUN_TEST(TestStrictWeakOrderingBasics); TEST_SUMMARY_RETURN();
}
