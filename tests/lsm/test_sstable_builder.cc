#include "lsm/sstable.h"
#include "test_util.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
lsm::Entry MakeEntry(std::string key, uint64_t sequence = 1,
                     std::string value = "v",
                     ValueType type = ValueType::kValue) {
    return {{std::move(key), sequence, type}, std::move(value)};
}
template<class Exception, class Function>
void CheckThrows(Function function) {
    bool caught = false;
    try { function(); }
    catch (const Exception&) { caught = true; }
    CHECK(caught);
}
void CheckBuilt(const SSTableBuilder& builder,
                const std::vector<lsm::Entry>& expected) {
    const auto& blocks = builder.Blocks();
    const auto& index = builder.Index();
    REQUIRE(blocks.size() == index.size());
    CHECK_EQ(builder.BlockCount(), blocks.size());
    uint64_t offset = 0;
    size_t record = 0;
    InternalKeyComparator compare;
    for (size_t i = 0; i < blocks.size(); ++i) {
        CHECK(!blocks[i].empty());
        CHECK_EQ(index[i].offset, offset);
        CHECK_EQ(index[i].size, blocks[i].size());
        DataBlockReader reader(blocks[i]);
        auto it = reader.Begin();
        REQUIRE(it.Valid());
        CHECK_EQ(compare.Compare(index[i].firstKey, it.Key()), 0);
        CHECK_EQ(index[i].firstKey.type, it.Key().type);
        for (; it.Valid(); it.Next()) {
            REQUIRE(record < expected.size());
            CHECK_EQ(compare.Compare(it.Key(), expected[record].key), 0);
            CHECK_EQ(it.Key().type, expected[record].key.type);
            CHECK_STR_EQ(it.Value(), expected[record].value);
            CHECK(builder.Filter().MayContain(it.Key().user_key));
            ++record;
        }
        offset += blocks[i].size();
    }
    CHECK_EQ(record, expected.size());
}
void TestEmptyAndState() {
    CheckThrows<std::invalid_argument>([] { SSTableBuilder builder(1, 0); });
    SSTableBuilder builder(0);
    CHECK(builder.Empty());
    CHECK_EQ(builder.BlockCount(), 0u);
    CheckThrows<std::logic_error>([&] { (void)builder.Blocks(); });
    CheckThrows<std::logic_error>([&] { (void)builder.Index(); });
    CheckThrows<std::logic_error>([&] { (void)builder.Filter(); });
    builder.Finish();
    builder.Finish();
    CheckBuilt(builder, {});
    CHECK(builder.Empty());
    CHECK(!builder.Filter().MayContain("missing"));
    CheckThrows<std::logic_error>([&] { builder.Add(MakeEntry("a")); });
}
void TestExactBoundaryAndIndex() {
    std::vector<lsm::Entry> entries = {
        MakeEntry("a", 30), MakeEntry("a", 20), MakeEntry("b"), MakeEntry("c")
    };
    const auto size = lsm::EncodedEntrySize(entries[0]);
    SSTableBuilder builder(entries.size(), 2 * size);
    builder.Add(entries[0]);
    builder.Add(entries[1]);
    CHECK_EQ(builder.BlockCount(), 0u);
    builder.Add(entries[2]);
    CHECK_EQ(builder.BlockCount(), 1u);
    builder.Add(entries[3]);
    builder.Finish();
    REQUIRE(builder.Blocks().size() == 2);
    CHECK_EQ(builder.Blocks()[0].size(), 2 * size);
    CheckBuilt(builder, entries);
    const auto blocks = builder.BlockCount();
    builder.Finish();
    CHECK_EQ(builder.BlockCount(), blocks);
}
void TestOversizedRecord() {
    const auto small = MakeEntry("a");
    const auto big = MakeEntry("b", 1, std::string(5000, 'x'));
    const auto tail = MakeEntry("c");
    SSTableBuilder builder(3, 64);
    builder.Add(small); builder.Add(big); builder.Add(tail); builder.Finish();
    REQUIRE(builder.BlockCount() == 3);
    CHECK_EQ(builder.Blocks()[1].size(), lsm::EncodedEntrySize(big));
    CHECK(builder.Blocks()[1].size() > 4096);
    CheckBuilt(builder, {small, big, tail});
    SSTableBuilder firstBig(1, 1);
    firstBig.Add(big); firstBig.Finish();
    CheckBuilt(firstBig, {big});
}
void TestRejectedInputPreservesLayout() {
    auto a = MakeEntry("a", 30);
    auto b = MakeEntry("b", 20);
    auto c = MakeEntry("c", 10);
    SSTableBuilder builder(3, lsm::EncodedEntrySize(a));
    builder.Add(a); builder.Add(b); // a flushed; b pending
    CHECK_EQ(builder.BlockCount(), 1u);
    CheckThrows<std::invalid_argument>([&] { builder.Add(a); });
    CheckThrows<std::invalid_argument>([&] { builder.Add(b); });
    auto duplicate = b; duplicate.key.type = ValueType::kDeletion; duplicate.value.clear();
    CheckThrows<std::invalid_argument>([&] { builder.Add(duplicate); });
    auto invalid = c; invalid.key.sequence = 0;
    CheckThrows<std::invalid_argument>([&] { builder.Add(invalid); });
    invalid = c; invalid.key.type = static_cast<ValueType>(255);
    CheckThrows<std::invalid_argument>([&] { builder.Add(invalid); });
    invalid = c; invalid.key.type = ValueType::kDeletion;
    CheckThrows<std::invalid_argument>([&] { builder.Add(invalid); });
    invalid = c; invalid.value.assign(lsm::kWalMaxEntrySize, 'x');
    CheckThrows<std::invalid_argument>([&] { builder.Add(invalid); });
    CHECK_EQ(builder.BlockCount(), 1u); // rejected input must not flush b
    builder.Add(c); builder.Finish();
    CheckBuilt(builder, {a, b, c});
}
void TestVersionsBinaryAndZeroEstimate() {
    std::vector<lsm::Entry> entries = {
        MakeEntry("", 1, ""),
        MakeEntry(std::string("a\0\xFF", 3), 30, std::string("\0\x80", 2)),
        MakeEntry(std::string("a\0\xFF", 3), 20, "", ValueType::kDeletion),
        MakeEntry(std::string("a\0\xFF", 3), 10, "old")
    };
    SSTableBuilder builder(0, 1);
    for (const auto& entry : entries) builder.Add(entry);
    builder.Finish();
    CheckBuilt(builder, entries);
    DataBlockReader deletion(builder.Blocks()[2]);
    CHECK_EQ(deletion.Get(25, entries[2].key.user_key).state, LookupState::kDeleted);
}
void TestFirstKeyAndReset() {
    DataBlockBuilder block;
    block.Add(MakeEntry("a", 30));
    block.Add(MakeEntry("a", 20));
    block.Add(MakeEntry("b"));
    CHECK_STR_EQ(block.FirstKey().user_key, "a");
    CHECK_EQ(block.FirstKey().sequence, 30u);
    block.Reset();
    block.Add(MakeEntry("c", 10));
    CHECK_STR_EQ(block.FirstKey().user_key, "c");
    CHECK_EQ(block.FirstKey().sequence, 10u);
}
}
int main() {
    RUN_TEST(TestEmptyAndState);
    RUN_TEST(TestExactBoundaryAndIndex);
    RUN_TEST(TestOversizedRecord);
    RUN_TEST(TestRejectedInputPreservesLayout);
    RUN_TEST(TestVersionsBinaryAndZeroEstimate);
    RUN_TEST(TestFirstKeyAndReset);
    TEST_SUMMARY_RETURN();
}
