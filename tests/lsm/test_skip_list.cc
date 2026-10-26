// Public-contract tests: no access to SkipList's private nodes or levels.
#include "lsm/skip_list.h"
#include "test_util.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace {
struct Record {
    InternalKey key;
    std::string value;
};

InternalKey Key(std::string user_key, uint64_t sequence,
                ValueType type = ValueType::kValue) {
    return {std::move(user_key), sequence, type};
}

// Independent oracle: deliberately do not call InternalKeyComparator.
bool Before(const InternalKey& a, const InternalKey& b) {
    if (a.user_key != b.user_key) return a.user_key < b.user_key;
    return a.sequence > b.sequence;
}

template <class Exception, class Action>
bool Throws(Action action) {
    try {
        action();
    } catch (const Exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

void CheckRecord(const SkipList::Iterator& it, const Record& expected) {
    REQUIRE(it.Valid());
    CHECK_STR_EQ(it.Key().user_key, expected.key.user_key);
    CHECK_EQ(it.Key().sequence, expected.key.sequence);
    CHECK_EQ(it.Key().type, expected.key.type);
    CHECK_STR_EQ(it.Value(), expected.value);
}

void CheckContents(const SkipList& list, const std::vector<Record>& expected) {
    CHECK_EQ(list.Size(), expected.size());
    CHECK_EQ(list.Empty(), expected.empty());
    auto it = list.Begin();
    // Bounded iteration also detects extra records / cycles without hanging.
    for (const auto& record : expected) {
        REQUIRE(it.Valid());
        CheckRecord(it, record);
        it.Next();
    }
    CHECK(!it.Valid());
}

void CheckSeek(const SkipList& list, const std::vector<Record>& expected,
               const InternalKey& target) {
    const auto found = std::lower_bound(
        expected.begin(), expected.end(), target,
        [](const Record& record, const InternalKey& key) {
            return Before(record.key, key);
        });
    auto it = list.Seek(target);
    CHECK_EQ(it.Valid(), found != expected.end());
    if (found != expected.end()) CheckRecord(it, *found);
}

void TestEmpty() {
    SkipList list([] { return 1; });
    CHECK_EQ(list.Size(), 0u);
    CHECK(list.Empty());
    CHECK(!list.Begin().Valid());
    CHECK(!list.Seek(Key("a", 1)).Valid());
    CHECK(!list.Seek(Key("", 0)).Valid());
}

void TestSingleAndIteratorEnd() {
    SkipList list([] { return 1; });
    REQUIRE(list.Insert(Key("a", 1), "A"));
    CheckContents(list, {{Key("a", 1), "A"}});
    auto it = list.Begin();
    REQUIRE(it.Valid());
    it.Next();
    CHECK(!it.Valid());
}

void TestInsertionOrdersAndHeights() {
    const std::vector<Record> sorted{
        {Key("a", 1), "A"}, {Key("c", 1), "C"},
        {Key("h", 1), "H"}, {Key("k", 1), "K"},
        {Key("m", 1), "M"}};
    const std::vector<std::vector<int>> orders{
        {0, 1, 2, 3, 4}, {4, 3, 2, 1, 0}, {3, 1, 4, 0, 2}};
    for (int height : {1, 2, SkipList::kMaxHeight}) {
        for (const auto& order : orders) {
            SkipList list([height] { return height; });
            for (int index : order) {
                REQUIRE(list.Insert(sorted[index].key, sorted[index].value));
            }
            CheckContents(list, sorted);
            for (const auto& record : sorted) {
                CheckSeek(list, sorted, record.key);
            }
        }
    }
}

void TestMixedHeights() {
    const std::vector<int> heights{1, 12, 2, 1, 6, 3, 12, 1};
    size_t call = 0;
    SkipList list([&] { return heights.at(call++); });
    const std::vector<std::string> keys{"h", "c", "m", "a", "k", "b", "z", "d"};
    std::vector<Record> expected;
    for (const auto& key : keys) {
        REQUIRE(list.Insert(Key(key, 1), key));
        expected.push_back({Key(key, 1), key});
    }
    std::sort(expected.begin(), expected.end(),
              [](const Record& a, const Record& b) { return Before(a.key, b.key); });
    CHECK_EQ(call, heights.size());
    CheckContents(list, expected);
    for (const auto& record : expected) CheckSeek(list, expected, record.key);
}

void TestVersionsAndDeletionRecord() {
    SkipList list([] { return 2; });
    REQUIRE(list.Insert(Key("k", 10), "old"));
    REQUIRE(list.Insert(Key("k", 30, ValueType::kDeletion), ""));
    REQUIRE(list.Insert(Key("k", 20), "middle"));
    REQUIRE(list.Insert(Key("a", 1), "first"));
    REQUIRE(list.Insert(Key("z", 1), "last"));
    // A deletion is a stored record, not physical removal by SkipList.
    const std::vector<Record> expected{
        {Key("a", 1), "first"},
        {Key("k", 30, ValueType::kDeletion), ""},
        {Key("k", 20), "middle"}, {Key("k", 10), "old"},
        {Key("z", 1), "last"}};
    CheckContents(list, expected);
    for (uint64_t seq : {40u, 30u, 25u, 20u, 15u, 10u, 5u, 0u}) {
        CheckSeek(list, expected, Key("k", seq));
    }
}

void TestDuplicateDoesNotOverwrite() {
    SkipList list([] { return 1; });
    REQUIRE(list.Insert(Key("k", 20), "original"));
    CHECK(!list.Insert(Key("k", 20), "replacement"));
    CHECK(!list.Insert(Key("k", 20, ValueType::kDeletion), ""));
    CheckContents(list, {{Key("k", 20), "original"}});

    SkipList deleted([] { return 1; });
    REQUIRE(deleted.Insert(Key("k", 20, ValueType::kDeletion), ""));
    CHECK(!deleted.Insert(Key("k", 20), "resurrect"));
    CheckContents(deleted, {{Key("k", 20, ValueType::kDeletion), ""}});
}

void TestSeekUserKeyBoundaries() {
    SkipList list([] { return 3; });
    const std::vector<Record> expected{
        {Key("b", 2), "B"}, {Key("d", 4), "D"}, {Key("f", 6), "F"}};
    for (const auto& record : expected) REQUIRE(list.Insert(record.key, record.value));
    for (const auto& key : {"", "a", "b", "c", "d", "e", "f", "g"}) {
        CheckSeek(list, expected, Key(key, 100));
        CheckSeek(list, expected, Key(key, 0));
    }
    CheckSeek(list, expected, Key("d", 4));
}

void TestBinaryKeysAndValues() {
    SkipList list([] { return 2; });
    const std::string binary_value("x\0y\n", 4);
    std::vector<Record> expected{
        {Key("", 1), ""},
        {Key(std::string(1, '\0'), 1), binary_value},
        {Key("a", 1), ""},
        {Key(std::string("a\0b", 3), 1), "embedded NUL"},
        {Key(std::string(1, static_cast<char>(0x7f)), 1), "127"},
        {Key(std::string(1, static_cast<char>(0x80)), 1), "128"},
        {Key(std::string(1, static_cast<char>(0xff)), 1), "255"}};
    for (auto it = expected.rbegin(); it != expected.rend(); ++it) {
        REQUIRE(list.Insert(it->key, it->value));
    }
    CheckContents(list, expected); // Explicit unsigned-byte ordering.
    for (const auto& record : expected) CheckRecord(list.Seek(record.key), record);
}

void TestSequenceLimits() {
    SkipList list([] { return 1; });
    const auto max = std::numeric_limits<uint64_t>::max();
    REQUIRE(list.Insert(Key("k", 1), "one"));
    REQUIRE(list.Insert(Key("k", max), "max"));
    const std::vector<Record> expected{{Key("k", max), "max"}, {Key("k", 1), "one"}};
    CheckContents(list, expected);
    for (auto seq : {max, max - 1, uint64_t{1}, uint64_t{0}}) {
        CheckSeek(list, expected, Key("k", seq));
    }
}

void TestZeroSequenceRejected() {
    SkipList list([] { return 1; });
    CHECK(Throws<std::invalid_argument>([&] { list.Insert(Key("a", 0), "bad"); }));
    CheckContents(list, {});
    REQUIRE(list.Insert(Key("k", 1), "kept"));
    CHECK(Throws<std::invalid_argument>([&] { list.Insert(Key("z", 0), "bad"); }));
    CheckContents(list, {{Key("k", 1), "kept"}});
}

void TestInvalidHeightRejectedAndRecovery() {
    for (int invalid : {-1, 0, SkipList::kMaxHeight + 1}) {
        int height = 1;
        SkipList list([&] { return height; });
        REQUIRE(list.Insert(Key("b", 1), "kept"));
        height = invalid;
        CHECK(Throws<std::invalid_argument>([&] { list.Insert(Key("a", 1), "bad"); }));
        CheckContents(list, {{Key("b", 1), "kept"}});
        height = 2;
        REQUIRE(list.Insert(Key("a", 1), "retry"));
        CheckContents(list, {{Key("a", 1), "retry"}, {Key("b", 1), "kept"}});
    }
}

void TestThrowingGeneratorLeavesListIntact() {
    bool fail = false;
    SkipList list([&]() -> int {
        if (fail) throw std::runtime_error("injected generator failure");
        return 2;
    });
    REQUIRE(list.Insert(Key("b", 1), "kept"));
    fail = true;
    CHECK(Throws<std::runtime_error>([&] { list.Insert(Key("a", 1), "bad"); }));
    CheckContents(list, {{Key("b", 1), "kept"}});
    fail = false;
    REQUIRE(list.Insert(Key("a", 1), "retry"));
    CheckContents(list, {{Key("a", 1), "retry"}, {Key("b", 1), "kept"}});
}

void TestIteratorsSurviveInsertions() {
    SkipList list([] { return 2; });
    REQUIRE(list.Insert(Key("m", 1), "M"));
    auto held = list.Begin();
    auto copy = held;
    // Forces the owning vector to grow; node addresses must remain stable.
    for (int i = 1; i <= 2048; ++i) {
        REQUIRE(list.Insert(Key("a", static_cast<uint64_t>(i)), std::to_string(i)));
    }
    REQUIRE(list.Insert(Key("z", 1), "Z"));
    CheckRecord(held, {Key("m", 1), "M"});
    held.Next();
    CheckRecord(held, {Key("z", 1), "Z"});
    CheckRecord(copy, {Key("m", 1), "M"});
}

void TestAgainstSortedReference() {
    std::vector<Record> expected;
    for (int k = 0; k < 64; ++k) {
        for (uint64_t seq = 1; seq <= 8; ++seq) {
            auto type = seq % 3 == 0 ? ValueType::kDeletion : ValueType::kValue;
            expected.push_back({Key("key-" + std::to_string(k), seq, type),
                                type == ValueType::kDeletion ? "" : std::to_string(seq)});
        }
    }
    std::sort(expected.begin(), expected.end(),
              [](const Record& a, const Record& b) { return Before(a.key, b.key); });
    for (unsigned seed : {7u, 42u, 2026u}) {
        std::mt19937 rng(seed);
        auto shuffled = expected;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        SkipList list([&] { return 1 + static_cast<int>(rng() % SkipList::kMaxHeight); });
        for (const auto& record : shuffled) REQUIRE(list.Insert(record.key, record.value));
        CheckContents(list, expected);
        for (const auto& record : expected) CHECK(!list.Insert(record.key, "overwrite"));
        CHECK_EQ(list.Size(), expected.size());
        for (int k = -1; k <= 64; ++k) {
            for (uint64_t seq = 0; seq <= 10; ++seq) {
                CheckSeek(list, expected, Key("key-" + std::to_string(k), seq));
            }
        }
    }
}


void TestEveryInsertAgainstReference() {
    // Check intermediate states as well as the final state.
    std::mt19937 rng(12345);
    SkipList list([&] {
        return 1 + static_cast<int>(rng() % SkipList::kMaxHeight);
    });
    std::vector<Record> input;
    for (int k = 0; k < 24; ++k) {
        for (uint64_t seq : {1u, 3u, 7u}) {
            input.push_back({Key("key-" + std::to_string(k), seq), std::to_string(seq)});
        }
    }
    std::shuffle(input.begin(), input.end(), rng);
    std::vector<Record> expected;
    for (const auto& record : input) {
        REQUIRE(list.Insert(record.key, record.value));
        expected.push_back(record);
        std::sort(expected.begin(), expected.end(),
                  [](const Record& a, const Record& b) { return Before(a.key, b.key); });
        CheckContents(list, expected);
        for (const auto& stored : expected) CheckSeek(list, expected, stored.key);
        CheckSeek(list, expected, Key("key-12", 5));
        CheckSeek(list, expected, Key("zz", 0));
    }
}


void TestApproximateMemoryUsage() {
    SkipList list([] { return 1; });
    CHECK_EQ(list.ApproximateMemoryUsage(), 0u);

    REQUIRE(list.Insert(Key("a", 1), "A"));
    const size_t after_first = list.ApproximateMemoryUsage();
    CHECK(after_first > 0u);

    CHECK(!list.Insert(Key("a", 1), "replacement"));
    CHECK_EQ(list.ApproximateMemoryUsage(), after_first);

    CHECK(Throws<std::invalid_argument>([&] {
        list.Insert(Key("bad", 0), "bad");
    }));
    CHECK_EQ(list.ApproximateMemoryUsage(), after_first);
    CHECK_EQ(list.Size(), 1u);
}

void TestApproximateMemoryUsageReflectsHeight() {
    SkipList short_list([] { return 1; });
    SkipList tall_list([] { return SkipList::kMaxHeight; });

    REQUIRE(short_list.Insert(Key("same", 1), "value"));
    REQUIRE(tall_list.Insert(Key("same", 1), "value"));

    const size_t short_usage = short_list.ApproximateMemoryUsage();
    const size_t tall_usage = tall_list.ApproximateMemoryUsage();
    CHECK(short_usage > 0u);
    CHECK(tall_usage > short_usage);
}

void TestApproximateMemoryUsageUnaffectedByInvalidHeight() {
    int height = 1;
    SkipList list([&] { return height; });
    REQUIRE(list.Insert(Key("kept", 1), "value"));
    const size_t before = list.ApproximateMemoryUsage();

    height = 0;
    CHECK(Throws<std::invalid_argument>([&] {
        list.Insert(Key("bad", 2), "value");
    }));
    CHECK_EQ(list.ApproximateMemoryUsage(), before);
    CHECK_EQ(list.Size(), 1u);

    height = SkipList::kMaxHeight + 1;
    CHECK(Throws<std::invalid_argument>([&] {
        list.Insert(Key("bad-again", 3), "value");
    }));
    CHECK_EQ(list.ApproximateMemoryUsage(), before);
    CHECK_EQ(list.Size(), 1u);
}

void TestDefaultAndEmptyGenerator() {
    for (bool empty_generator : {false, true}) {
        // Both constructors should provide a working default random-height path.
        auto list = empty_generator
            ? std::make_unique<SkipList>(SkipList::HeightGenerator{})
            : std::make_unique<SkipList>();
        std::vector<Record> expected;
        for (int i = 1; i <= 512; ++i) {
            expected.push_back({Key("k", static_cast<uint64_t>(i)), std::to_string(i)});
            REQUIRE(list->Insert(expected.back().key, expected.back().value));
        }
        std::reverse(expected.begin(), expected.end());
        CheckContents(*list, expected);
        for (const auto& record : expected) CheckSeek(*list, expected, record.key);
    }
}
} // namespace

int main() {
    std::cout << "===== LSM SkipList Tests =====\n";
    RUN_TEST(TestEmpty);
    RUN_TEST(TestSingleAndIteratorEnd);
    RUN_TEST(TestInsertionOrdersAndHeights);
    RUN_TEST(TestMixedHeights);
    RUN_TEST(TestVersionsAndDeletionRecord);
    RUN_TEST(TestDuplicateDoesNotOverwrite);
    RUN_TEST(TestSeekUserKeyBoundaries);
    RUN_TEST(TestBinaryKeysAndValues);
    RUN_TEST(TestSequenceLimits);
    RUN_TEST(TestZeroSequenceRejected);
    RUN_TEST(TestInvalidHeightRejectedAndRecovery);
    RUN_TEST(TestThrowingGeneratorLeavesListIntact);
    RUN_TEST(TestIteratorsSurviveInsertions);
    RUN_TEST(TestAgainstSortedReference);
    RUN_TEST(TestEveryInsertAgainstReference);
    RUN_TEST(TestApproximateMemoryUsage);
    RUN_TEST(TestApproximateMemoryUsageReflectsHeight);
    RUN_TEST(TestApproximateMemoryUsageUnaffectedByInvalidHeight);
    RUN_TEST(TestDefaultAndEmptyGenerator);
    TEST_SUMMARY_RETURN();
}
