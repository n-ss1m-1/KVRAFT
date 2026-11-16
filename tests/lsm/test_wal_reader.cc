#include "lsm/wal.h"
#include "wal_test_util.h"
#include "test_util.h"
#include <fstream>
#include <vector>

namespace {
lsm::Entry MakeRecord(uint64_t seq) {
    lsm::Entry r;
    r.key.sequence = seq;
    r.key.user_key = std::string("a\0b", 3);
    r.value = std::string("\xFF\0v", 3);
    return r;
}
void Save(const std::string& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("test fixture write failed");
}
void CheckRecord(const lsm::Entry& r, uint64_t seq) {
    auto expected = MakeRecord(seq);
    CHECK_EQ(r.key.sequence, seq);
    CHECK_EQ(r.key.type, expected.key.type);
    CHECK_STR_EQ(r.key.user_key, expected.key.user_key);
    CHECK_STR_EQ(r.value, expected.value);
}
void TestEmptyAndMissing() {
    const auto dir = testutil::MakeTempDir("wal_empty");
    const auto path = dir + "/wal";
    Save(path, "");
    {
        WalReader reader(path);
        auto output = MakeRecord(999);
        CHECK_EQ(reader.LastValidOffset(), 0u);
        CHECK_EQ(reader.ReadNext(output), WalReadStatus::kEndOfFile);
        CheckRecord(output, 999);
    }
    bool threw = false;
    try { WalReader reader(dir + "/missing"); }
    catch (const std::system_error&) { threw = true; }
    CHECK(threw);
    CHECK(!std::filesystem::exists(dir + "/missing"));
    testutil::CleanupDir(dir);
}
void TestRecords() {
    const auto dir = testutil::MakeTempDir("wal_records");
    const auto path = dir + "/wal";
    const auto a = waltest::Frame(MakeRecord(1));
    auto deletion = MakeRecord(2);
    deletion.key.type = ValueType::kDeletion;
    deletion.value.clear();
    const auto b = waltest::Frame(deletion);
    Save(path, a + b);
    {
        WalReader reader(path);
        lsm::Entry output;
        if (reader.ReadNext(output) == WalReadStatus::kRecord) {
            CheckRecord(output, 1);
            CHECK_EQ(reader.LastValidOffset(), a.size());
            if (reader.ReadNext(output) == WalReadStatus::kRecord) {
                CHECK_EQ(output.key.sequence, 2u);
                CHECK_EQ(output.key.type, ValueType::kDeletion);
                CHECK(output.value.empty());
                CHECK_EQ(reader.LastValidOffset(), a.size() + b.size());
                CHECK_EQ(reader.ReadNext(output), WalReadStatus::kEndOfFile);
            } else { CHECK(false); }
        } else { CHECK(false); }
    }
    testutil::CleanupDir(dir);
}
void TestTruncatedTails() {
    const auto dir = testutil::MakeTempDir("wal_tail");
    const auto path = dir + "/wal";
    const auto a = waltest::Frame(MakeRecord(1));
    const auto b = waltest::Frame(MakeRecord(2));
    for (size_t n = 0; n < b.size(); ++n) {
        Save(path, b.substr(0, n));
        {
            WalReader reader(path);
            auto output = MakeRecord(999);
            CHECK_EQ(reader.ReadNext(output), n == 0 ?
                WalReadStatus::kEndOfFile : WalReadStatus::kTruncatedTail);
            CHECK_EQ(reader.LastValidOffset(), 0u);
            CheckRecord(output, 999);
        }
        Save(path, a + b.substr(0, n));
        {
            WalReader reader(path);
            lsm::Entry output;
            if (reader.ReadNext(output) != WalReadStatus::kRecord) {
                CHECK(false);
                continue;
            }
            CHECK_EQ(reader.ReadNext(output), n == 0 ?
                WalReadStatus::kEndOfFile : WalReadStatus::kTruncatedTail);
            CHECK_EQ(reader.LastValidOffset(), a.size());
            CheckRecord(output, 1);
        }
    }
    testutil::CleanupDir(dir);
}
void TestCorruption() {
    const auto dir = testutil::MakeTempDir("wal_corrupt");
    const auto path = dir + "/wal";
    const auto valid = waltest::Frame(MakeRecord(1));
    std::vector<std::string> cases;
    for (uint32_t length : {0u, lsm::kWalMinEntrySize - 1,
                           lsm::kWalMaxEntrySize + 1, UINT32_MAX}) {
        std::string header;
        lsm::AppendFixed32(header, length);
        lsm::AppendFixed32(header, 0);
        cases.push_back(header);
    }
    auto bad = valid; bad[4] ^= 1; cases.push_back(bad);
    bad = valid; bad.back() ^= 1; cases.push_back(bad);
    auto payload = valid.substr(8);
    payload[8] = static_cast<char>(255);
    std::string header;
    lsm::AppendFixed32(header, static_cast<uint32_t>(payload.size()));
    bad = header;
    lsm::AppendFixed32(bad, lsm::Crc32c(header + payload));
    cases.push_back(bad + payload);
    for (const auto& bytes : cases) {
        Save(path, bytes);
        {
            WalReader reader(path);
            auto output = MakeRecord(999);
            CHECK_EQ(reader.ReadNext(output), WalReadStatus::kCorruption);
            CHECK_EQ(reader.LastValidOffset(), 0u);
            CheckRecord(output, 999);
        }
    }
    testutil::CleanupDir(dir);
}
}
int main() {
    RUN_TEST(TestEmptyAndMissing);
    RUN_TEST(TestRecords);
    RUN_TEST(TestTruncatedTails);
    RUN_TEST(TestCorruption);
    TEST_SUMMARY_RETURN();
}
