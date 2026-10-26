#include "lsm/mem_table.h"
#include "test_util.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

template <class Exception, class Action>
bool Throws(Action action) {
    try { action(); }
    catch (const Exception&) { return true; }
    catch (...) { return false; }
    return false;
}

void TestEmptyAndBasicProperties() {
    MemTable table;
    CHECK(table.Empty());
    CHECK_EQ(table.Size(), 0u);
    CHECK_EQ(table.ApproximateMemoryUsage(), 0u);
    CHECK(!table.IsFrozen());
    CHECK(!table.Begin().Valid());
    REQUIRE(table.Add(10, ValueType::kValue, "a", "A"));
    CHECK(!table.Empty());
    CHECK_EQ(table.Size(), 1u);
    CHECK(table.ApproximateMemoryUsage() > 0u);
}

void TestGetVisibleVersions() {
    MemTable table;
    REQUIRE(table.Add(10, ValueType::kValue, "apple", "old"));
    REQUIRE(table.Add(20, ValueType::kValue, "apple", "new"));
    REQUIRE(table.Add(30, ValueType::kDeletion, "apple", ""));
    auto old = table.Get(15, "apple");
    CHECK_EQ(old.state, LookupState::kValue);
    CHECK_STR_EQ(old.value, "old");
    auto newest = table.Get(25, "apple");
    CHECK_EQ(newest.state, LookupState::kValue);
    CHECK_STR_EQ(newest.value, "new");
    auto deleted = table.Get(30, "apple");
    CHECK_EQ(deleted.state, LookupState::kDeleted);
    auto not_visible = table.Get(5, "apple");
    CHECK_EQ(not_visible.state, LookupState::kNotFound);
    CHECK_EQ(table.Get(100, "banana").state, LookupState::kNotFound);
}

void TestDeleteStopsOlderLookup() {
    MemTable table;
    REQUIRE(table.Add(10, ValueType::kValue, "apple", "old"));
    REQUIRE(table.Add(20, ValueType::kDeletion, "apple", ""));
    auto result = table.Get(25, "apple");
    CHECK_EQ(result.state, LookupState::kDeleted);
    CHECK(result.value.empty());
}

void TestDuplicateAndMemoryAccounting() {
    MemTable table;
    REQUIRE(table.Add(10, ValueType::kValue, "a", "first"));
    const size_t before = table.ApproximateMemoryUsage();
    CHECK(!table.Add(10, ValueType::kValue, "a", "replacement"));
    CHECK_EQ(table.Size(), 1u);
    CHECK_EQ(table.ApproximateMemoryUsage(), before);
    CHECK(!table.Add(10, ValueType::kDeletion, "a", ""));
    CHECK_EQ(table.ApproximateMemoryUsage(), before);
}

void TestInvalidArguments() {
    MemTable table;
    CHECK(Throws<std::invalid_argument>([&] { table.Add(0, ValueType::kValue, "a", "bad"); }));
    CHECK(Throws<std::invalid_argument>([&] { table.Add(1, static_cast<ValueType>(99), "a", "bad"); }));
    CHECK(Throws<std::invalid_argument>([&] { table.Add(1, ValueType::kDeletion, "a", "must-be-empty"); }));
    CHECK(table.Empty());
    CHECK_EQ(table.ApproximateMemoryUsage(), 0u);
}

void TestFreezeRejectsWrites() {
    MemTable table;
    REQUIRE(table.Add(1, ValueType::kValue, "a", "A"));
    const size_t before = table.ApproximateMemoryUsage();
    table.Freeze();
    CHECK(table.IsFrozen());
    table.Freeze();
    CHECK(table.IsFrozen());
    CHECK(Throws<std::logic_error>([&] { table.Add(2, ValueType::kValue, "b", "B"); }));
    CHECK_EQ(table.Size(), 1u);
    CHECK_EQ(table.ApproximateMemoryUsage(), before);
    auto result = table.Get(10, "a");
    CHECK_EQ(result.state, LookupState::kValue);
    CHECK_STR_EQ(result.value, "A");
}

void TestBeginExposesSortedInternalRecords() {
    MemTable table;
    REQUIRE(table.Add(20, ValueType::kValue, "k", "new"));
    REQUIRE(table.Add(10, ValueType::kValue, "k", "old"));
    REQUIRE(table.Add(1, ValueType::kValue, "a", "first"));
    auto it = table.Begin();
    REQUIRE(it.Valid());
    CHECK_STR_EQ(it.Key().user_key, "a");
    it.Next();
    REQUIRE(it.Valid());
    CHECK_STR_EQ(it.Key().user_key, "k");
    CHECK_EQ(it.Key().sequence, 20u);
    it.Next();
    REQUIRE(it.Valid());
    CHECK_EQ(it.Key().sequence, 10u);
    it.Next();
    CHECK(!it.Valid());
}

}

int main() {
    std::cout << "===== LSM MemTable Tests =====\n";
    RUN_TEST(TestEmptyAndBasicProperties);
    RUN_TEST(TestGetVisibleVersions);
    RUN_TEST(TestDeleteStopsOlderLookup);
    RUN_TEST(TestDuplicateAndMemoryAccounting);
    RUN_TEST(TestInvalidArguments);
    RUN_TEST(TestFreezeRejectsWrites);
    RUN_TEST(TestBeginExposesSortedInternalRecords);
    TEST_SUMMARY_RETURN();
}
