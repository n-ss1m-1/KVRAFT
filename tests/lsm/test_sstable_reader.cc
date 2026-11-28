#include "lsm/sstable.h"
#include "test_util.h"
#include <stdexcept>
#include <string>
#include <algorithm>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>

namespace {
enum class ReadMode { kNormal, kShortAndInterrupt, kIoError, kEof };
ReadMode readMode = ReadMode::kNormal;
int readCalls = 0;
}
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" ssize_t __wrap_pread(int fd, void* data, size_t size, off_t offset) {
    ++readCalls;
    if (readMode == ReadMode::kIoError) { errno = EIO; return -1; }
    if (readMode == ReadMode::kEof) return 0;
    if (readMode == ReadMode::kShortAndInterrupt) {
        if (readCalls % 3 == 1) { errno = EINTR; return -1; }
        size = std::min(size, size_t{3});
    }
    return __real_pread(fd, data, size, offset);
}

namespace {
struct Fixture {
    std::string dir = testutil::MakeTempDir("sstable_reader");
    std::string path = dir + "/table.sst";
    ~Fixture() {
        readMode = ReadMode::kNormal;
        testutil::CleanupDir(dir);
    }
};
std::string Load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void Save(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), bytes.size());
    if (!out) throw std::runtime_error("test fixture write failed");
}
template<class Exception, class Function>
void ExpectThrows(Function action) {
    bool caught = false;
    try { action(); }
    catch (const Exception&) { caught = true; }
    catch (...) {}
    CHECK(caught);
}
void AppendBlock(std::string& file, std::string_view content) {
    file.append(content);
    lsm::AppendFixed32(file, lsm::Crc32c(content));
}
// Rebuild metadata with valid CRCs, so parser tests reach field validation.
std::string ReplaceMetadata(const std::string& file, bool replaceBloom,
                            const std::string& replacement) {
    const auto* footer = file.data() + file.size() - 48;
    const auto bo = lsm::DecodeFixed64(footer + 12);
    const auto bs = lsm::DecodeFixed64(footer + 20);
    const auto io = lsm::DecodeFixed64(footer + 28);
    const auto is = lsm::DecodeFixed64(footer + 36);
    const auto bloom = replaceBloom ? replacement : file.substr(bo, bs);
    const auto index = replaceBloom ? file.substr(io, is) : replacement;
    std::string output = file.substr(0, bo);
    AppendBlock(output, bloom);
    const auto newIndexOffset = output.size();
    AppendBlock(output, index);
    std::string newFooter = "MYLSM001";
    lsm::AppendFixed32(newFooter, sstable_format::kVersion);
    lsm::AppendFixed64(newFooter, bo);
    lsm::AppendFixed64(newFooter, bloom.size());
    lsm::AppendFixed64(newFooter, newIndexOffset);
    lsm::AppendFixed64(newFooter, index.size());
    AppendBlock(output, newFooter);
    return output;
}
void FixFooterCrc(std::string& file) {
    const auto start = file.size() - 48;
    std::string crc;
    lsm::AppendFixed32(crc, lsm::Crc32c(std::string_view(file).substr(start, 44)));
    file.replace(start + 44, 4, crc);
}
lsm::Entry E(std::string key, uint64_t seq, std::string value,
             ValueType type=ValueType::kValue) {
    return {{std::move(key), seq, type}, std::move(value)};
}
void TestEmptyAndLifecycle() {
    Fixture fixture;
    SSTableBuilder builder(0);
    bool threw=false;
    try { SSTableWriter::Write(fixture.path, builder); } catch(const std::logic_error&) { threw=true; }
    CHECK(threw);
    builder.Finish();
    SSTableWriter::Write(fixture.path, builder);
    SSTableReader reader(fixture.path);
    CHECK_EQ(reader.Get(10, "missing").state, LookupState::kNotFound);
}
void TestSingleBlockQueries() {
    Fixture fixture;
    SSTableBuilder builder(4, 4096);
    builder.Add(E("apple", 30, "red"));
    builder.Add(E("apple", 20, "", ValueType::kDeletion));
    builder.Add(E("apple", 10, "green"));
    builder.Add(E("banana", 8, "yellow"));
    builder.Finish();
    SSTableWriter::Write(fixture.path, builder);
    SSTableReader reader(fixture.path);
    auto result = reader.Get(25, "apple");
    CHECK_EQ(result.state, LookupState::kDeleted);
    result = reader.Get(15, "apple");
    CHECK_EQ(result.state, LookupState::kValue);
    CHECK_STR_EQ(result.value, "green");
    result = reader.Get(100, "apple");
    CHECK_EQ(result.state, LookupState::kValue);
    CHECK_STR_EQ(result.value, "red");
    CHECK_EQ(reader.Get(20, "banana").state, LookupState::kValue);
    CHECK_STR_EQ(reader.Get(20, "banana").value, "yellow");
    CHECK_EQ(reader.Get(20, "aardvark").state, LookupState::kNotFound);
    CHECK_EQ(reader.Get(20, "zebra").state, LookupState::kNotFound);
}
void TestCrossBlockVersions() {
    const auto size = lsm::EncodedEntrySize(E("apple", 30, "v"));
    Fixture fixture;
    SSTableBuilder builder(4, size);
    builder.Add(E("apple", 30, "new"));
    builder.Add(E("apple", 20, "mid"));
    builder.Add(E("apple", 10, "old"));
    builder.Add(E("banana", 8, "yellow"));
    builder.Finish();
    REQUIRE(builder.BlockCount() == 4);
    SSTableWriter::Write(fixture.path, builder);
    SSTableReader reader(fixture.path);
    auto result = reader.Get(15, "apple");
    CHECK_EQ(result.state, LookupState::kValue);
    CHECK_STR_EQ(result.value, "old");
    result = reader.Get(25, "apple");
    CHECK_EQ(result.state, LookupState::kValue);
    CHECK_STR_EQ(result.value, "mid");
    result = reader.Get(5, "apple");
    CHECK_EQ(result.state, LookupState::kNotFound);
    result = reader.Get(9, "banana");
    CHECK_EQ(result.state, LookupState::kValue);
}
void TestCrossBlockDeletion() {
    const auto size = lsm::EncodedEntrySize(E("apple", 20, "", ValueType::kDeletion));
    Fixture fixture;
    SSTableBuilder builder(3, size);
    builder.Add(E("apple", 30, "new"));
    builder.Add(E("apple", 20, "", ValueType::kDeletion));
    builder.Add(E("apple", 10, "old"));
    builder.Finish();
    SSTableWriter::Write(fixture.path, builder);
    SSTableReader reader(fixture.path);
    CHECK_EQ(reader.Get(25, "apple").state, LookupState::kDeleted);
    CHECK_EQ(reader.Get(15, "apple").state, LookupState::kValue);
    CHECK_STR_EQ(reader.Get(15, "apple").value, "old");
}
void TestBinaryAndFalseBloom() {
    Fixture fixture;
    SSTableBuilder builder(2, 4096);
    const std::string key("a\0\xFF", 3);
    builder.Add(E(key, 2, std::string("\0v", 2)));
    builder.Add(E("z", 1, "z"));
    builder.Finish();
    SSTableWriter::Write(fixture.path, builder);
    SSTableReader reader(fixture.path);
    auto result=reader.Get(2, key);
    CHECK_EQ(result.state, LookupState::kValue);
    CHECK_STR_EQ(result.value, std::string("\0v",2));
    CHECK_EQ(reader.Get(2, "definitely-absent").state, LookupState::kNotFound);
}

void TestIndependentLifetimeAndLargeValue() {
    Fixture fixture;
    const std::string large(9000, 'x');
    {
        SSTableBuilder builder(2, 4096);
        builder.Add(E("", 7, ""));
        builder.Add(E("apple", 3, large));
        builder.Finish();
        SSTableWriter::Write(fixture.path, builder);
    }
    for (int i = 0; i < 2; ++i) {
        SSTableReader reader(fixture.path);
        CHECK_EQ(reader.Get(7, "").state, LookupState::kValue);
        CHECK_STR_EQ(reader.Get(7, "").value, "");
        CHECK_STR_EQ(reader.Get(3, "apple").value, large);
        CHECK_EQ(reader.Get(0, "apple").state, LookupState::kNotFound);
        CHECK_STR_EQ(reader.Get(std::numeric_limits<uint64_t>::max(), "apple").value, large);
    }
}
void TestReadFaults() {
    Fixture fixture;
    SSTableBuilder builder(1);
    builder.Add(E("apple", 20, "value"));
    builder.Finish();
    SSTableWriter::Write(fixture.path, builder);
    readMode = ReadMode::kShortAndInterrupt;
    readCalls = 0;
    {
        SSTableReader reader(fixture.path);
        CHECK_STR_EQ(reader.Get(30, "apple").value, "value");
        CHECK(readCalls > 10);
    }
    readMode = ReadMode::kIoError;
    ExpectThrows<std::system_error>([&] { SSTableReader reader(fixture.path); });
    readMode = ReadMode::kEof;
    ExpectThrows<std::runtime_error>([&] { SSTableReader reader(fixture.path); });
    readMode = ReadMode::kNormal;
    SSTableReader reader(fixture.path);
    readMode = ReadMode::kIoError;
    ExpectThrows<std::system_error>([&] { reader.Get(30, "apple"); });
    readMode = ReadMode::kEof;
    ExpectThrows<std::runtime_error>([&] { reader.Get(30, "apple"); });
    readMode = ReadMode::kNormal;
    CHECK_STR_EQ(reader.Get(30, "apple").value, "value");
}
void TestFooterAndBlockCorruption() {
    Fixture fixture;
    ExpectThrows<std::system_error>([&] { SSTableReader reader(fixture.path); });
    SSTableBuilder builder(1);
    builder.Add(E("apple", 20, "value"));
    builder.Finish();
    SSTableWriter::Write(fixture.path, builder);
    const auto original = Load(fixture.path);
    for (size_t length : {size_t{0}, size_t{1}, size_t{47}}) {
        Save(fixture.path, original.substr(0, length));
        ExpectThrows<std::runtime_error>([&] { SSTableReader reader(fixture.path); });
    }
    for (size_t position : {original.size() - 1, original.size() - 48}) {
        auto bytes = original;
        bytes[position] ^= 1;
        Save(fixture.path, bytes);
        ExpectThrows<std::runtime_error>([&] { SSTableReader reader(fixture.path); });
    }
    // CRC正确，但magic或version不受支持
    for (size_t relative : {size_t{0}, size_t{8}}) {
        auto bytes = original;
        bytes[bytes.size() - 48 + relative] ^= 1;
        FixFooterCrc(bytes);
        Save(fixture.path, bytes);
        ExpectThrows<std::runtime_error>([&] { SSTableReader reader(fixture.path); });
    }
    {
        auto legacy = original;
        std::string version;
        lsm::AppendFixed32(version, 1);
        legacy.replace(legacy.size() - 48 + 8, 4, version);
        FixFooterCrc(legacy);
        Save(fixture.path, legacy);
        ExpectThrows<std::runtime_error>([&] { SSTableReader reader(fixture.path); });
    }
    const auto* footer = original.data() + original.size() - 48;
    for (size_t position : {size_t{0},
         static_cast<size_t>(lsm::DecodeFixed64(footer + 12)),
         static_cast<size_t>(lsm::DecodeFixed64(footer + 28))}) {
        auto bytes = original;
        bytes[position] ^= 1;
        Save(fixture.path, bytes);
        ExpectThrows<std::runtime_error>([&] {
            SSTableReader reader(fixture.path);
            reader.Get(30, "apple"); // Data CRC is checked lazily.
        });
    }
    Save(fixture.path, original);
    SSTableReader reader(fixture.path);
    CHECK_STR_EQ(reader.Get(30, "apple").value, "value");
}
void TestMalformedMetadata() {
    Fixture fixture;
    SSTableBuilder builder(1);
    builder.Add(E("apple", 20, "value"));
    builder.Finish();
    SSTableWriter::Write(fixture.path, builder);
    const auto original = Load(fixture.path);
    const auto* footer = original.data() + original.size() - 48;
    const auto index = original.substr(lsm::DecodeFixed64(footer + 28),
                                       lsm::DecodeFixed64(footer + 36));
    const auto bloom = original.substr(lsm::DecodeFixed64(footer + 12),
                                       lsm::DecodeFixed64(footer + 20));
    // Every truncated prefix has a newly computed valid CRC.
    for (size_t length = 0; length < index.size(); ++length) {
        Save(fixture.path, ReplaceMetadata(original, false, index.substr(0, length)));
        ExpectThrows<std::runtime_error>([&] { SSTableReader reader(fixture.path); });
    }
    Save(fixture.path, ReplaceMetadata(original, false, index + "x"));
    ExpectThrows<std::runtime_error>([&] { SSTableReader reader(fixture.path); });
    for (size_t length = 0; length < bloom.size(); ++length) {
        Save(fixture.path, ReplaceMetadata(original, true, bloom.substr(0, length)));
        ExpectThrows<std::runtime_error>([&] { SSTableReader reader(fixture.path); });
    }
    auto badBloom = bloom;
    badBloom[8] ^= 1; // Declared byte count no longer matches payload.
    Save(fixture.path, ReplaceMetadata(original, true, badBloom));
    ExpectThrows<std::runtime_error>([&] { SSTableReader reader(fixture.path); });
}


// 对照全部记录，覆盖块内推进、跨块版本、删除标记和二进制内容。
void TestIteratorTraversal() {
    const std::vector<lsm::Entry> entries = {
        E("", 40, ""), E("apple", 30, "new"),
        E("apple", 20, "", ValueType::kDeletion), E("apple", 10, "old"),
        E(std::string("b\0x", 3), 9, std::string("v\0x", 3)),
        E("cherry", 8, std::string(9000, 'x')), E("zebra", 7, "last")
    };
    for (size_t target : {size_t{1}, size_t{80}, size_t{65536}}) {
        Fixture fixture;
        SSTableBuilder builder(entries.size(), target);
        for (const auto& entry : entries) builder.Add(entry);
        builder.Finish();
        SSTableWriter::Write(fixture.path, builder);
        SSTableReader reader(fixture.path);
        auto it = reader.Begin();
        auto independent = reader.Begin();
        for (const auto& entry : entries) {
            REQUIRE(it.Valid());
            CHECK_STR_EQ(it.Key().user_key, entry.key.user_key);
            CHECK_EQ(it.Key().sequence, entry.key.sequence);
            CHECK_EQ(it.Key().type, entry.key.type);
            CHECK_STR_EQ(it.Value(), entry.value);
            it.Next();
        }
        CHECK(!it.Valid());
        CHECK(!it.Valid());
        REQUIRE(independent.Valid());
        CHECK_STR_EQ(independent.Key().user_key, "");
        auto restarted = reader.Begin();
        REQUIRE(restarted.Valid());
        CHECK_EQ(restarted.Key().sequence, uint64_t{40});
    }
    Fixture fixture;
    SSTableBuilder empty(0);
    empty.Finish();
    SSTableWriter::Write(fixture.path, empty);
    SSTableReader reader(fixture.path);
    CHECK(!reader.Begin().Valid());
}

void TestIteratorCacheEviction() {
    Fixture fixture;
    SSTableBuilder builder(3, 64);
    builder.Add(E("apple", 30, "new"));
    builder.Add(E("apple", 20, "old"));
    builder.Add(E("banana", 10, "yellow"));
    builder.Finish();
    REQUIRE(builder.BlockCount() == 2);
    SSTableWriter::Write(fixture.path, builder);
    auto cache = std::make_shared<BlockCache>(128);
    SSTableReader reader(fixture.path, 77, cache);
    auto it = reader.Begin();
    REQUIRE(it.Valid());
    REQUIRE(cache->Get({77, builder.Index()[0].offset}) != nullptr);
    // 填满缓存，确认当前块已被淘汰，迭代器仍可在块内推进。
    auto other = std::make_shared<const DataBlockReader>(lsm::EncodeEntry(E("other", 1, "v")));
    cache->Insert({99, 0}, other, 128);
    CHECK(cache->Get({77, builder.Index()[0].offset}) == nullptr);
    readMode = ReadMode::kIoError;
    it.Next();
    REQUIRE(it.Valid());
    CHECK_EQ(it.Key().sequence, uint64_t{20});
    readMode = ReadMode::kNormal;
    it.Next();
    REQUIRE(it.Valid());
    CHECK_STR_EQ(it.Key().user_key, "banana");
    it.Next();
    CHECK(!it.Valid());
}

void TestIteratorReadErrors() {
    Fixture fixture;
    SSTableBuilder builder(2, 1);
    builder.Add(E("apple", 20, "first"));
    builder.Add(E("banana", 10, "second"));
    builder.Finish();
    SSTableWriter::Write(fixture.path, builder);
    const auto original = Load(fixture.path);
    const auto second = builder.Index()[1];
    {
        SSTableReader reader(fixture.path);
        readMode = ReadMode::kIoError;
        ExpectThrows<std::system_error>([&] { reader.Begin(); });
        readMode = ReadMode::kNormal;
        auto it = reader.Begin();
        readMode = ReadMode::kIoError;
        ExpectThrows<std::system_error>([&] { it.Next(); });
        CHECK(!it.Valid());
        readMode = ReadMode::kNormal;
    }
    {
        SSTableReader reader(fixture.path);
        auto it = reader.Begin();
        readMode = ReadMode::kEof;
        ExpectThrows<std::runtime_error>([&] { it.Next(); });
        CHECK(!it.Valid());
        readMode = ReadMode::kNormal;
    }
    // 下一块 CRC 损坏，以及 CRC 正确但记录格式损坏，都不能伪装成遍历结束。
    for (bool fixCrc : {false, true}) {
        auto bytes = original;
        bytes[second.offset + 8] = char(0x7f); // 非法 ValueType
        if (fixCrc) {
            std::string crc;
            lsm::AppendFixed32(crc, lsm::Crc32c(std::string_view(bytes).substr(second.offset, second.size)));
            bytes.replace(second.offset + second.size, 4, crc);
        }
        Save(fixture.path, bytes);
        SSTableReader reader(fixture.path);
        auto it = reader.Begin();
        REQUIRE(it.Valid());
        ExpectThrows<std::runtime_error>([&] { it.Next(); });
        CHECK(!it.Valid());
    }
    Save(fixture.path, original);
    readMode = ReadMode::kShortAndInterrupt;
    readCalls = 0;
    SSTableReader reader(fixture.path);
    auto it = reader.Begin();
    REQUIRE(it.Valid());
    it.Next();
    REQUIRE(it.Valid());
    CHECK_STR_EQ(it.Value(), "second");
    it.Next();
    CHECK(!it.Valid());
    readMode = ReadMode::kNormal;
}

}
int main() {
    RUN_TEST(TestIteratorTraversal);
    RUN_TEST(TestIteratorCacheEviction);
    RUN_TEST(TestIteratorReadErrors);
    RUN_TEST(TestEmptyAndLifecycle);
    RUN_TEST(TestSingleBlockQueries);
    RUN_TEST(TestCrossBlockVersions);
    RUN_TEST(TestCrossBlockDeletion);
    RUN_TEST(TestBinaryAndFalseBloom);
    RUN_TEST(TestIndependentLifetimeAndLargeValue);
    RUN_TEST(TestReadFaults);
    RUN_TEST(TestFooterAndBlockCorruption);
    RUN_TEST(TestMalformedMetadata);
    TEST_SUMMARY_RETURN();
}
