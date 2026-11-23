#include "lsm/wal.h"
#include "wal_test_util.h"
#include "test_util.h"
#include <fstream>
#include <iterator>
#include <stdexcept>

// GNU linker wrapping affects this test executable only; engine code is unchanged.
namespace {
enum class Fault { kNone, kInterruptThenShort, kPartialThenFail, kZero };
Fault fault = Fault::kNone;
int writeCalls = 0;
int syncCalls = 0;
int syncInterrupts = 0;
bool syncFailure = false;
void Reset() {
    fault = Fault::kNone;
    writeCalls = syncCalls = syncInterrupts = 0;
    syncFailure = false;
}
}
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t size) {
    ++writeCalls;
    if (fault == Fault::kInterruptThenShort) {
        if (writeCalls == 1) { errno = EINTR; return -1; }
        return __real_write(fd, data, size > 3 ? 3 : size);
    }
    if (fault == Fault::kPartialThenFail) {
        if (writeCalls == 1) return __real_write(fd, data, size > 5 ? 5 : size);
        errno = ENOSPC;
        return -1;
    }
    if (fault == Fault::kZero) return 0;
    return __real_write(fd, data, size);
}
extern "C" int __wrap_fsync(int fd) {
    ++syncCalls;
    if (syncInterrupts > 0) {
        --syncInterrupts;
        errno = EINTR;
        return -1;
    }
    if (syncFailure) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
namespace {
struct Fixture {
    std::string dir = testutil::MakeTempDir("wal_writer");
    std::string path = dir + "/wal";
    Fixture() { Reset(); }
    ~Fixture() { Reset(); testutil::CleanupDir(dir); }
};
lsm::Entry Record(uint64_t seq, bool deletion = false) {
    lsm::Entry r;
    r.key.sequence = seq;
    r.key.user_key = std::string("a\0b", 3);
    r.key.type = deletion ? ValueType::kDeletion : ValueType::kValue;
    if (!deletion) r.value = std::string("\xFF\0v", 3);
    return r;
}
void CheckRecord(const lsm::Entry& actual, const lsm::Entry& expected) {
    CHECK_EQ(actual.key.sequence, expected.key.sequence);
    CHECK_EQ(actual.key.type, expected.key.type);
    CHECK_STR_EQ(actual.key.user_key, expected.key.user_key);
    CHECK_STR_EQ(actual.value, expected.value);
}
std::string Bytes(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
void TestCreateAppendSyncReopen() {
    Fixture f;
    CHECK(!std::filesystem::exists(f.path));
    {
        WalWriter writer(f.path);
        CHECK(std::filesystem::exists(f.path));
        CHECK(!writer.Sync());  // empty file
        CHECK(!writer.Append(Record(1)));
        CHECK(!writer.Append(Record(2, true)));
        CHECK(!writer.Sync());
    }
    {
        WalWriter writer(f.path);
        CHECK(!writer.Append(Record(3)));
        CHECK(!writer.Sync());
    }
    CHECK_STR_EQ(Bytes(f.path), waltest::Frame(Record(1)) +
        waltest::Frame(Record(2, true)) + waltest::Frame(Record(3)));
    WalReader reader(f.path);
    std::vector<lsm::Entry> out;
    for (uint64_t seq = 1; seq <= 3; ++seq) {
        REQUIRE(reader.ReadNext(out) == WalReadStatus::kRecord);
        CheckRecord(out.at(0), Record(seq, seq == 2));
    }
    CHECK_EQ(reader.ReadNext(out), WalReadStatus::kEndOfFile);
    CHECK_EQ(reader.LastValidOffset(), std::filesystem::file_size(f.path));
}
void TestInvalidInputDoesNotWrite() {
    Fixture f;
    WalWriter writer(f.path);
    const auto rejected = [&](lsm::Entry record) {
        const auto size = std::filesystem::file_size(f.path);
        bool threw = false;
        try { (void)writer.Append(record); }
        catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
        CHECK_EQ(std::filesystem::file_size(f.path), size);
    };
    auto record = Record(0); rejected(record);
    record = Record(1); record.key.type = static_cast<ValueType>(255); rejected(record);
    record = Record(1, true); record.value = "invalid"; rejected(record);
    record = Record(1); record.value.assign(lsm::kWalMaxEntrySize, 'x'); rejected(record);
    CHECK_EQ(writeCalls, 0);
    CHECK(!writer.Append(Record(1)));  // validation exceptions do not break writer
    CHECK(!writer.Sync());
}
void TestShortWritesAndInterrupts() {
    Fixture f;
    WalWriter writer(f.path);
    fault = Fault::kInterruptThenShort;
    CHECK(!writer.Append(Record(1)));
    CHECK(writeCalls > 2);
    CHECK_EQ(syncCalls, 0);  // Append must not call fsync
    syncInterrupts = 2;
    CHECK(!writer.Sync());
    CHECK_EQ(syncCalls, 3);
    CHECK_STR_EQ(Bytes(f.path), waltest::Frame(Record(1)));
}
void TestWriteFailureLeavesPartialTail() {
    Fixture f;
    {
        WalWriter writer(f.path);
        CHECK(!writer.Append(Record(1)));
        writeCalls = 0;
        fault = Fault::kPartialThenFail;
        const auto error = writer.Append(Record(2));
        CHECK_EQ(error.value(), ENOSPC);
        CHECK(error.category() == std::generic_category());
        CHECK_EQ(writeCalls, 2);
        // Contract: after an I/O error, close instead of appending again.
    }
    const auto firstSize = waltest::Frame(Record(1)).size();
    CHECK_EQ(std::filesystem::file_size(f.path), firstSize + 5);
    WalReader reader(f.path);
    std::vector<lsm::Entry> out;
    REQUIRE(reader.ReadNext(out) == WalReadStatus::kRecord);
    CHECK_EQ(reader.ReadNext(out), WalReadStatus::kTruncatedTail);
    CHECK_EQ(reader.LastValidOffset(), firstSize);
    CheckRecord(out.at(0), Record(1));
}
void TestZeroWrite() {
    Fixture f;
    WalWriter writer(f.path);
    fault = Fault::kZero;
    CHECK(writer.Append(Record(1)) == std::make_error_code(std::errc::io_error));
    CHECK_EQ(writeCalls, 1);
    CHECK_EQ(std::filesystem::file_size(f.path), 0u);
}
void TestSyncFailure() {
    Fixture f;
    WalWriter writer(f.path);
    CHECK(!writer.Append(Record(1)));
    syncFailure = true;
    const auto error = writer.Sync();
    CHECK_EQ(error.value(), EIO);
    CHECK(error.category() == std::generic_category());
    CHECK_EQ(syncCalls, 1);
}
void TestOpenFailure() {
    Fixture f;
    bool threw = false;
    try { WalWriter writer(f.dir + "/missing/wal"); }
    catch (const std::system_error& e) { threw = true; CHECK_EQ(e.code().value(), ENOENT); }
    CHECK(threw);
    threw = false;
    try { WalWriter writer(f.dir); }
    catch (const std::system_error&) { threw = true; }
    CHECK(threw);
}
}
int main() {
    RUN_TEST(TestCreateAppendSyncReopen);
    RUN_TEST(TestInvalidInputDoesNotWrite);
    RUN_TEST(TestShortWritesAndInterrupts);
    RUN_TEST(TestWriteFailureLeavesPartialTail);
    RUN_TEST(TestZeroWrite);
    RUN_TEST(TestSyncFailure);
    RUN_TEST(TestOpenFailure);
    TEST_SUMMARY_RETURN();
}
