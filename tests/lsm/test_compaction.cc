#include "lsm/compaction.h"
#include "lsm/sstable.h"
#include "test_util.h"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <system_error>

namespace {
bool failRead = false;
}
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* data, size_t size, off_t offset) {
    if (failRead) { errno = EIO; return -1; }
    return __real_pread(fd, data, size, offset);
}

namespace {
lsm::Entry E(std::string key, uint64_t seq, std::string value = "",
             ValueType type = ValueType::kValue) {
    return {{std::move(key), seq, type}, std::move(value)};
}
struct Fixture {
    std::string dir = testutil::MakeTempDir("compaction");
    std::vector<std::unique_ptr<SSTableReader>> readers;
    std::vector<const SSTableReader*> tables;
    std::vector<std::string> paths;
    ~Fixture() {
        failRead = false;
        readers.clear();
        testutil::CleanupDir(dir);
    }
    void Add(std::vector<lsm::Entry> entries, size_t blockSize = 64) {
        InternalKeyComparator cmp;
        std::sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) {
            return cmp(a.key, b.key);
        });
        SSTableBuilder builder(entries.size(), blockSize);
        for (const auto& e : entries) builder.Add(e);
        builder.Finish();
        auto path = dir + "/" + std::to_string(readers.size()) + ".sst";
        SSTableWriter::Write(path, builder);
        paths.push_back(path);
        readers.push_back(std::make_unique<SSTableReader>(path));
        tables.push_back(readers.back().get());
    }
    std::vector<lsm::Entry> Merge() {
        std::vector<lsm::Entry> result;
        MergeTables(tables, [&](const auto& e) { result.push_back(e); });
        return result;
    }
};
template<class Exception, class Function>
void ExpectThrows(Function action) {
    bool caught = false;
    try { action(); }
    catch (const Exception&) { caught = true; }
    catch (...) {}
    CHECK(caught);
}
void CheckEntries(const std::vector<lsm::Entry>& actual,
                  const std::vector<lsm::Entry>& expected) {
    REQUIRE(actual.size() == expected.size());
    InternalKeyComparator cmp;
    for (size_t i = 0; i < actual.size(); ++i) {
        CHECK_STR_EQ(actual[i].key.user_key, expected[i].key.user_key);
        CHECK_EQ(actual[i].key.sequence, expected[i].key.sequence);
        CHECK_EQ(actual[i].key.type, expected[i].key.type);
        CHECK_STR_EQ(actual[i].value, expected[i].value);
        if (i) CHECK(cmp(actual[i-1].key, actual[i].key));
    }
}
void TestEmptyAndSingleInput() {
    Fixture f;
    CHECK(f.Merge().empty());
    f.Add({});
    CHECK(f.Merge().empty());
    const std::vector<lsm::Entry> expected = {
        E("", 100), E("apple", 90, "", ValueType::kDeletion),
        E(std::string("b\0x", 3), 70, std::string("v\0x", 3)),
        E("z", 60, std::string(9000, 'x'))
    };
    auto input = expected;
    input.push_back(E("apple", 80, "old"));
    f.Add(input, 1);
    CheckEntries(f.Merge(), expected);
}
void TestOverlappingTablesAndOrder() {
    Fixture f;
    f.Add({E("apple", 10, "red"), E("banana", 11, "yellow")});
    f.Add({});
    f.Add({E("apple", 20, "green"), E("banana", 25, "", ValueType::kDeletion)});
    f.Add({E("apple", 30, "", ValueType::kDeletion), E("cherry", 31, "purple")});
    const std::vector<lsm::Entry> expected = {
        E("apple", 30, "", ValueType::kDeletion),
        E("banana", 25, "", ValueType::kDeletion), E("cherry", 31, "purple")
    };
    CheckEntries(f.Merge(), expected);
    std::reverse(f.tables.begin(), f.tables.end());
    CheckEntries(f.Merge(), expected);
    CheckEntries(f.Merge(), expected); // 多次调用不改变 Reader
}
void TestDuplicateInternalKeys() {
    for (bool oldVersion : {false, true}) {
        for (bool differentType : {false, true}) {
            Fixture f;
            std::vector<lsm::Entry> first = {E("apple", 20, "one")};
            if (oldVersion) first.push_back(E("apple", 30, "newest"));
            f.Add(first);
            f.Add({E("apple", 20, differentType ? "" : "two",
                     differentType ? ValueType::kDeletion : ValueType::kValue)});
            ExpectThrows<std::runtime_error>([&] { f.Merge(); });
        }
    }
}
void TestRandomizedOracle() {
    std::mt19937 rng(20261124);
    for (size_t count : {size_t{1}, size_t{4}, size_t{32}}) {
        Fixture f;
        std::vector<std::vector<lsm::Entry>> batches(count);
        std::map<std::string, lsm::Entry> oracle;
        for (uint64_t seq = 1; seq <= 700; ++seq) {
            auto key = std::string("key-") + std::to_string(rng() % 97);
            auto type = rng() % 4 == 0 ? ValueType::kDeletion : ValueType::kValue;
            auto e = E(key, seq, type == ValueType::kDeletion ? "" : std::to_string(seq), type);
            oracle[key] = e;
            batches[rng() % count].push_back(e);
        }
        for (auto& batch : batches) f.Add(std::move(batch), 128);
        std::vector<lsm::Entry> expected;
        for (const auto& pair : oracle) expected.push_back(pair.second);
        CheckEntries(f.Merge(), expected);
        std::shuffle(f.tables.begin(), f.tables.end(), rng);
        CheckEntries(f.Merge(), expected);
    }
}
struct EmitError {};
void TestExceptionPropagation() {
    Fixture f;
    f.Add({E("apple", 20, "first"), E("banana", 10, "second")}, 1);
    ExpectThrows<std::invalid_argument>([&] {
        MergeTables({nullptr}, [](const auto&) {});
    });
    size_t emitted = 0;
    ExpectThrows<EmitError>([&] {
        MergeTables(f.tables, [&](const auto&) { ++emitted; throw EmitError{}; });
    });
    CHECK_EQ(emitted, size_t{1});
    failRead = true;
    ExpectThrows<std::system_error>([&] { f.Merge(); });
    failRead = false;
    emitted = 0;
    ExpectThrows<std::system_error>([&] {
        MergeTables(f.tables, [&](const auto&) { ++emitted; failRead = true; });
    });
    CHECK_EQ(emitted, size_t{1});
    failRead = false;
    CHECK_EQ(f.Merge().size(), size_t{2});

    // 首块可读，下一块 CRC 损坏时必须抛异常，不能返回部分成功。
    SSTableBuilder layout(2, 1);
    layout.Add(E("apple", 20, "first"));
    layout.Add(E("banana", 10, "second"));
    layout.Finish();
    std::fstream file(f.paths[0], std::ios::in | std::ios::out | std::ios::binary);
    const auto offset = layout.Blocks()[0].size() + 4; // 文件偏移包含前一块 CRC
    file.seekg(offset);
    char byte;
    file.read(&byte, 1);
    byte ^= 1;
    file.seekp(offset);
    file.write(&byte, 1);
    file.flush();
    REQUIRE(bool(file));
    emitted = 0;
    ExpectThrows<std::runtime_error>([&] {
        MergeTables(f.tables, [&](const auto&) { ++emitted; });
    });
    CHECK_EQ(emitted, size_t{1});
}
}
int main() {
    RUN_TEST(TestEmptyAndSingleInput);
    RUN_TEST(TestOverlappingTablesAndOrder);
    RUN_TEST(TestDuplicateInternalKeys);
    RUN_TEST(TestRandomizedOracle);
    RUN_TEST(TestExceptionPropagation);
    TEST_SUMMARY_RETURN();
}
