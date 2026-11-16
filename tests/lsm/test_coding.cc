#include "lsm/coding.h"
#include "test_util.h"
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <stdexcept>

namespace {
void TestKnownEncoding() {
    std::string out = "prefix";
    lsm::AppendFixed32(out, 0x12345678u);
    CHECK_STR_EQ(out, std::string("prefix") + std::string("\x78\x56\x34\x12", 4));
    lsm::AppendFixed64(out, UINT64_C(0x0123456789ABCDEF));
    CHECK_STR_EQ(out.substr(10), std::string("\xEF\xCD\xAB\x89\x67\x45\x23\x01", 8));
    CHECK_EQ(out.size(), 18u);
}
void TestKnownDecoding() {
    const char zero[8] = {};
    CHECK_EQ(lsm::DecodeFixed32(zero), 0u);
    CHECK_EQ(lsm::DecodeFixed64(zero), UINT64_C(0));
    CHECK_EQ(lsm::DecodeFixed32("\x78\x56\x34\x12"), 0x12345678u);
    CHECK_EQ(lsm::DecodeFixed64("\xEF\xCD\xAB\x89\x67\x45\x23\x01"),
             UINT64_C(0x0123456789ABCDEF));
    CHECK_EQ(lsm::DecodeFixed32("\xFF\xFF\xFF\xFF"), std::numeric_limits<uint32_t>::max());
    CHECK_EQ(lsm::DecodeFixed64("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"),
             std::numeric_limits<uint64_t>::max());
    // Not aligned, not NUL terminated: only the specified width may be read.
    const char bytes[] = {'X', '\x78', '\x56', '\x34', '\x12'};
    CHECK_EQ(lsm::DecodeFixed32(bytes + 1), 0x12345678u);
}
void TestEachBit() {
    for (int bit = 0; bit < 64; ++bit) {
        std::string expected(8, '\0');
        expected[bit / 8] = static_cast<char>(1u << (bit % 8));
        const uint64_t value = UINT64_C(1) << bit;
        std::string out;
        lsm::AppendFixed64(out, value);
        CHECK_STR_EQ(out, expected);
        CHECK_EQ(lsm::DecodeFixed64(expected.data()), value);
        if (bit < 32) {
            std::string out32;
            lsm::AppendFixed32(out32, static_cast<uint32_t>(value));
            CHECK_STR_EQ(out32, expected.substr(0, 4));
            CHECK_EQ(lsm::DecodeFixed32(expected.data()), static_cast<uint32_t>(value));
        }
    }
}
void TestCrc32c() {
    CHECK_EQ(lsm::Crc32c(std::string_view{}), 0x00000000u);
    CHECK_EQ(lsm::Crc32c("123456789"), 0xE3069283u);
}

void TestEntryEncoding() {
    lsm::Entry record;
    record.key.user_key = std::string("a\0b", 3);
    record.key.sequence = 20;
    record.key.type = ValueType::kValue;
    record.value = std::string("\x80\0", 2);
    const auto payload = lsm::EncodeEntry(record);
    CHECK_EQ(payload.size(), 22u);
    CHECK_EQ(lsm::DecodeFixed64(payload.data()), 20u);
    CHECK_EQ(static_cast<unsigned char>(payload[8]), 1u);
    CHECK_EQ(lsm::DecodeFixed32(payload.data() + 9), 3u);
    CHECK_EQ(lsm::DecodeFixed32(payload.data() + 16), 2u);
    CHECK_STR_EQ(payload.substr(13, 3), record.key.user_key);
    CHECK_STR_EQ(payload.substr(20), record.value);
    record.key.type = ValueType::kDeletion;
    record.value.clear();
    CHECK_EQ(lsm::EncodeEntry(record).size(), 20u);
    record.key.user_key.clear();
    CHECK_EQ(lsm::EncodeEntry(record).size(), 17u);
}
void TestInvalidEntryEncoding() {
    const auto rejected = [](const lsm::Entry& record) {
        try { (void)lsm::EncodeEntry(record); }
        catch (const std::invalid_argument&) { return true; }
        return false;
    };
    lsm::Entry record;
    CHECK(rejected(record));
    record.key.sequence = 1;
    record.key.type = static_cast<ValueType>(255);
    CHECK(rejected(record));
    record.key.type = ValueType::kDeletion;
    record.value = "invalid";
    CHECK(rejected(record));
    record.key.type = ValueType::kValue;
    record.value.assign(lsm::kWalMaxEntrySize - lsm::kWalMinEntrySize, 'x');
    CHECK_EQ(lsm::EncodeEntry(record).size(), lsm::kWalMaxEntrySize);
    record.value.push_back('x');
    CHECK(rejected(record));
}


lsm::Entry SentinelRecord() {
    lsm::Entry r;
    r.key.user_key = "sentinel";
    r.key.sequence = 999;
    r.key.type = ValueType::kValue;
    r.value = "unchanged";
    return r;
}
void CheckRecordEqual(const lsm::Entry& actual, const lsm::Entry& expected) {
    CHECK_STR_EQ(actual.key.user_key, expected.key.user_key);
    CHECK_EQ(actual.key.sequence, expected.key.sequence);
    CHECK_EQ(actual.key.type, expected.key.type);
    CHECK_STR_EQ(actual.value, expected.value);
}
void CheckBadPayload(std::string_view bytes) {
    auto output = SentinelRecord();
    CHECK(!lsm::DecodeEntry(bytes, output));
    CheckRecordEqual(output, SentinelRecord());
}

void PutFixed32(std::string& bytes, size_t offset, uint32_t value) {
    std::string field;
    lsm::AppendFixed32(field, value);
    bytes.replace(offset, 4, field);
}
void TestEntryRoundTrips() {
    lsm::Entry input;
    input.key.sequence = UINT64_MAX;
    input.key.user_key = std::string("a\0b", 3);
    input.value = std::string("\x80\0\xFF", 3);
    for (int mode = 0; mode < 4; ++mode) {
        if (mode == 1) input.value.clear();
        if (mode == 2) input.key.user_key.clear();
        if (mode == 3) input.key.type = ValueType::kDeletion;
        auto output = SentinelRecord();
        if (lsm::DecodeEntry(lsm::EncodeEntry(input), output)) {
            CheckRecordEqual(output, input);
        } else { CHECK(false); }

    }
}
void TestMalformedEntries() {
    lsm::Entry input;
    input.key.sequence = 1;
    input.key.user_key = "key";
    input.value = "value";
    const auto valid = lsm::EncodeEntry(input);
    for (size_t n = 0; n < valid.size(); ++n)
        CheckBadPayload(std::string_view(valid).substr(0, n));
    CheckBadPayload(valid + "extra");
    auto bad = valid;
    bad.replace(0, 8, 8, '\0');
    CheckBadPayload(bad);
    bad = valid; bad[8] = static_cast<char>(255);
    CheckBadPayload(bad);
    bad = valid; PutFixed32(bad, 9, UINT32_MAX);
    CheckBadPayload(bad);
    bad = valid; PutFixed32(bad, 16, UINT32_MAX);
    CheckBadPayload(bad);
    // A deletion carrying value bytes must be rejected, even with correct lengths.
    bad = valid;
    bad[8] = static_cast<char>(ValueType::kDeletion);
    CheckBadPayload(bad);
}


void CheckKeyEqual(const InternalKey& actual, const InternalKey& expected) {
    CHECK_EQ(actual.sequence, expected.sequence);
    CHECK_EQ(actual.type, expected.type);
    CHECK_STR_EQ(actual.user_key, expected.user_key);
}
void TestInternalKeyCodec() {
    for (const auto type : {ValueType::kValue, ValueType::kDeletion}) {
        for (const auto& text : {std::string(), std::string("a\0\xff", 3)}) {
            InternalKey key{text, UINT64_MAX, type}, decoded;
            const auto bytes = lsm::EncodeInternalKey(key);
            CHECK_EQ(bytes.size(), 13u + text.size());
            CHECK_EQ(lsm::EncodedInternalKeySize(key), bytes.size());
            CHECK_EQ(lsm::DecodeFixed64(bytes.data()), UINT64_MAX);
            CHECK_EQ(static_cast<unsigned char>(bytes[8]), static_cast<unsigned char>(type));
            CHECK_EQ(lsm::DecodeFixed32(bytes.data() + 9), text.size());
            CHECK_STR_EQ(bytes.substr(13), text);
            size_t used = 0;
            REQUIRE(lsm::DecodeInternalKey(bytes + bytes, decoded, used));
            CheckKeyEqual(decoded, key);
            CHECK_EQ(used, bytes.size());
            size_t second = 0;
            REQUIRE(lsm::DecodeInternalKey(std::string_view(bytes + bytes).substr(used), decoded, second));
            CHECK_EQ(second, used);
            // ! Entry必须直接复用同一份InternalKey编码
            lsm::Entry entry{key, type == ValueType::kValue ? "value" : ""};
            CHECK_STR_EQ(lsm::EncodeEntry(entry).substr(0, used), bytes);
        }
    }
}
void TestInvalidInternalKeys() {
    const InternalKey sentinel{"unchanged", 99, ValueType::kDeletion};
    const InternalKey key{"apple", 7, ValueType::kValue};
    const auto bytes = lsm::EncodeInternalKey(key);
    const auto reject = [&](std::string_view input) {
        InternalKey out = sentinel;
        size_t used = 999;
        CHECK(!lsm::DecodeInternalKey(input, out, used));
        CheckKeyEqual(out, sentinel);
        CHECK_EQ(used, 999u);
    };
    for (size_t n = 0; n < bytes.size(); ++n) reject(std::string_view(bytes).substr(0, n));
    auto bad = bytes;
    bad.replace(0, 8, 8, '\0'); reject(bad);
    bad = bytes; bad[8] = static_cast<char>(255); reject(bad);
    bad = bytes; PutFixed32(bad, 9, UINT32_MAX); reject(bad);
    for (const auto invalid : {InternalKey{"k", 0, ValueType::kValue},
                              InternalKey{"k", 1, static_cast<ValueType>(255)}}) {
        bool caught = false;
        try { lsm::EncodeInternalKey(invalid); }
        catch (const std::invalid_argument&) { caught = true; }
        CHECK(caught);
    }
}

void TestRandomRoundTrips() {
    std::mt19937_64 rng(20261027);
    for (int i = 0; i < 128; ++i) {
        const uint64_t value = rng();
        std::string out;
        lsm::AppendFixed64(out, value);
        CHECK_EQ(out.size(), 8u);
        CHECK_EQ(lsm::DecodeFixed64(out.data()), value);
        out.clear();
        const auto value32 = static_cast<uint32_t>(value);
        lsm::AppendFixed32(out, value32);
        CHECK_EQ(out.size(), 4u);
        CHECK_EQ(lsm::DecodeFixed32(out.data()), value32);
    }
}
}
int main() {
    RUN_TEST(TestKnownEncoding);
    RUN_TEST(TestKnownDecoding);
    RUN_TEST(TestEachBit);
    RUN_TEST(TestRandomRoundTrips);
    RUN_TEST(TestCrc32c);
    RUN_TEST(TestInternalKeyCodec);
    RUN_TEST(TestInvalidInternalKeys);
    RUN_TEST(TestEntryEncoding);
    RUN_TEST(TestInvalidEntryEncoding);
    RUN_TEST(TestEntryRoundTrips);
    RUN_TEST(TestMalformedEntries);
    TEST_SUMMARY_RETURN();
}
