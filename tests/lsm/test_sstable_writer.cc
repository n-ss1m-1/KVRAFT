#include "lsm/sstable.h"
#include "test_util.h"

#include <cerrno>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace {
enum class WriteMode { kNormal, kShortAndInterrupt, kFailAfterPartial, kZero };
WriteMode mode = WriteMode::kNormal;
int writeCalls = 0;
int syncInterrupts = 0;
bool failSync = false;
}
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size) {
    ++writeCalls;
    if (mode == WriteMode::kShortAndInterrupt) {
        if (writeCalls == 1) { errno = EINTR; return -1; }
        return __real_write(fd, data, size > 3 ? 3 : size);
    }
    if (mode == WriteMode::kFailAfterPartial) {
        if (writeCalls == 1) return __real_write(fd, data, size > 5 ? 5 : size);
        errno = ENOSPC; return -1;
    }
    if (mode == WriteMode::kZero) return 0;
    return __real_write(fd, data, size);
}
extern "C" int __wrap_fsync(int fd) {
    if (syncInterrupts > 0) { --syncInterrupts; errno = EINTR; return -1; }
    if (failSync) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
namespace {
struct Fixture {
    std::string dir = testutil::MakeTempDir("sstable_writer");
    std::string path = dir + "/table.sst";
    Fixture() { Reset(); }
    static void Reset() {
        mode = WriteMode::kNormal; writeCalls = 0; syncInterrupts = 0; failSync = false;
    }
    ~Fixture() { Reset(); testutil::CleanupDir(dir); }
};
std::string Load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void SaveOld(const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    out << "old table";
}
lsm::Entry E(std::string key, uint64_t seq, std::string value,
             ValueType type = ValueType::kValue) {
    return {{std::move(key), seq, type}, std::move(value)};
}
std::string_view CheckedBlock(const std::string& file, uint64_t offset, uint64_t size) {
    if (offset > file.size() || size > file.size() - offset ||
        file.size() - offset - size < 4)
        throw std::runtime_error("invalid block handle in written file");
    const auto content = std::string_view(file).substr(offset, size);
    CHECK_EQ(lsm::Crc32c(content), lsm::DecodeFixed32(file.data() + offset + size));
    return content;
}
void VerifyFile(const std::string& path, const SSTableBuilder& builder) {
    const auto file = Load(path);
    REQUIRE(file.size() >= 48);
    const auto footer = std::string_view(file).substr(file.size() - 48);
    CHECK_STR_EQ(std::string(footer.substr(0, 8)), "MYLSM001");
    CHECK_EQ(lsm::DecodeFixed32(footer.data() + 8), sstable_format::kVersion);
    CHECK_EQ(lsm::Crc32c(footer.substr(0, 44)),
             lsm::DecodeFixed32(footer.data() + 44));
    const auto bloomOffset = lsm::DecodeFixed64(footer.data() + 12);
    const auto bloomSize = lsm::DecodeFixed64(footer.data() + 20);
    const auto indexOffset = lsm::DecodeFixed64(footer.data() + 28);
    const auto indexSize = lsm::DecodeFixed64(footer.data() + 36);
    CHECK_EQ(indexOffset, bloomOffset + bloomSize + 4);
    CHECK_EQ(indexOffset + indexSize + 4, file.size() - 48);
    const auto bloom = CheckedBlock(file, bloomOffset, bloomSize);
    REQUIRE(bloom.size() >= 16);
    CHECK_EQ(lsm::DecodeFixed32(bloom.data()), builder.Filter().BitCount());
    CHECK_EQ(lsm::DecodeFixed32(bloom.data() + 4), builder.Filter().HashCount());
    const auto byteSize = lsm::DecodeFixed64(bloom.data() + 8);
    CHECK_EQ(byteSize, builder.Filter().ByteSize());
    CHECK_EQ(bloom.size(), 16 + byteSize);
    const auto& bits = builder.Filter().Bits();
    for (size_t i = 0; i < bits.size(); ++i)
        CHECK_EQ(static_cast<unsigned char>(bloom[16 + i]), bits[i]);
    const auto index = CheckedBlock(file, indexOffset, indexSize);
    REQUIRE(index.size() >= 4);
    const auto count = lsm::DecodeFixed32(index.data());
    REQUIRE(count == builder.BlockCount());
    size_t pos = 4;
    uint64_t expectedOffset = 0;
    for (size_t i = 0; i < count; ++i) {
        REQUIRE(index.size() - pos >= 4);
        const auto keyLength = lsm::DecodeFixed32(index.data() + pos); pos += 4;
        REQUIRE(keyLength <= index.size() - pos);
        const std::string key(index.substr(pos, keyLength)); pos += keyLength;
        REQUIRE(index.size() - pos >= 21);
        const auto seq = lsm::DecodeFixed64(index.data() + pos); pos += 8;
        const auto type = static_cast<unsigned char>(index[pos++]);
        const auto offset = lsm::DecodeFixed64(index.data() + pos); pos += 8;
        const auto size = lsm::DecodeFixed32(index.data() + pos); pos += 4;
        const auto& expected = builder.Index()[i];
        CHECK_STR_EQ(key, expected.firstKey.user_key);
        CHECK_EQ(seq, expected.firstKey.sequence);
        CHECK_EQ(type, static_cast<uint8_t>(expected.firstKey.type));
        CHECK_EQ(offset, expectedOffset);
        CHECK_EQ(size, builder.Blocks()[i].size());
        const auto content = CheckedBlock(file, offset, size);
        CHECK_STR_EQ(std::string(content), builder.Blocks()[i]);
        DataBlockReader block{std::string(content)};
        CHECK(block.Begin().Valid());
        expectedOffset += size + 4;
    }
    CHECK_EQ(pos, index.size());
    CHECK_EQ(expectedOffset, bloomOffset);
    CHECK(!std::filesystem::exists(path + ".tmp"));
}
void TestEmptyAndMultiBlockFormat() {
    Fixture f;
    SSTableBuilder empty(0); empty.Finish();
    SSTableWriter::Write(f.path, empty);
    VerifyFile(f.path, empty); // zero-valued Bloom bytes are binary, not a C string
    SSTableBuilder builder(64, 1);
    builder.Add(E("", 1, ""));
    builder.Add(E(std::string("a\0\xFF", 3), 30, std::string("\0\x80", 2)));
    builder.Add(E(std::string("a\0\xFF", 3), 20, "", ValueType::kDeletion));
    builder.Add(E("z", 1, std::string(5000, 'x')));
    builder.Finish();
    SSTableWriter::Write(f.path, builder);
    VerifyFile(f.path, builder);
}
void TestUnfinishedAndOpenFailure() {
    Fixture f;
    SSTableBuilder builder(1);
    bool caught = false;
    try { SSTableWriter::Write(f.path, builder); }
    catch (const std::logic_error&) { caught = true; }
    CHECK(caught);
    CHECK(!std::filesystem::exists(f.path));
    CHECK(!std::filesystem::exists(f.path + ".tmp"));
    builder.Finish();
    caught = false;
    try { SSTableWriter::Write(f.dir + "/missing/table", builder); }
    catch (const std::system_error&) { caught = true; }
    CHECK(caught);
}
void TestShortWriteAndSyncInterrupt() {
    Fixture f;
    SSTableBuilder builder(2, 1);
    builder.Add(E("a", 2, "abc")); builder.Add(E("b", 1, "def")); builder.Finish();
    mode = WriteMode::kShortAndInterrupt;
    syncInterrupts = 2;
    SSTableWriter::Write(f.path, builder);
    CHECK(writeCalls > 10);
    CHECK_EQ(syncInterrupts, 0);
    VerifyFile(f.path, builder);
}
void TestFailurePreservesOldFile() {
    Fixture f;
    SSTableBuilder builder(1);
    builder.Add(E("a", 1, "value")); builder.Finish();
    for (int scenario = 0; scenario < 3; ++scenario) {
        Fixture::Reset();
        SaveOld(f.path);
        if (scenario == 0) mode = WriteMode::kFailAfterPartial;
        if (scenario == 1) mode = WriteMode::kZero;
        if (scenario == 2) failSync = true;
        bool caught = false;
        try { SSTableWriter::Write(f.path, builder); }
        catch (const std::system_error& e) {
            caught = true;
            CHECK_EQ(e.code().value(), scenario == 0 ? ENOSPC : EIO);
        }
        CHECK(caught);
        CHECK_STR_EQ(Load(f.path), "old table");
        CHECK(!std::filesystem::exists(f.path + ".tmp"));
    }
}
void TestRenameFailureCleanup() {
    Fixture f;
    std::filesystem::create_directory(f.path);
    SSTableBuilder builder(1); builder.Finish();
    bool caught = false;
    try { SSTableWriter::Write(f.path, builder); }
    catch (const std::system_error&) { caught = true; }
    CHECK(caught);
    CHECK(std::filesystem::is_directory(f.path));
    CHECK(!std::filesystem::exists(f.path + ".tmp"));
}
}
int main() {
    RUN_TEST(TestEmptyAndMultiBlockFormat);
    RUN_TEST(TestUnfinishedAndOpenFailure);
    RUN_TEST(TestShortWriteAndSyncInterrupt);
    RUN_TEST(TestFailurePreservesOldFile);
    RUN_TEST(TestRenameFailureCleanup);
    TEST_SUMMARY_RETURN();
}
