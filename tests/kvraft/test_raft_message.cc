//test_raft_message.cc
// 覆盖 raft/raft_message：
//   LogEntry 序列化/反序列化（PUT/DEL/GET）
//   Meta 序列化/反序列化
//   Snapshot 二进制序列化（含 \\0 数据、损坏头、长度不匹配）
//   各类损坏输入 -> nullopt

#include <cstring>
#include <iostream>
#include <optional>
#include <string>

#include "raft/raft_message.h"
#include "common/command.h"
#include "test_util.h"

static LogEntry MakeEntry(int64_t index, int64_t term,
                          Command::Type type, std::string key,
                          std::string value = "") {
    return LogEntry{index, term, Command{type, std::move(key), std::move(value)}};
}

// ==================== LogEntry ====================

void TestLogEntryPutRoundTrip() {
    auto e = MakeEntry(7, 3, Command::Type::PUT, "name", "alice");
    std::string s = SerializeLogEntry(e);
    auto d = DeserializeLogEntry(s);
    CHECK(d.has_value());
    CHECK_EQ(d->index, 7);
    CHECK_EQ(d->term, 3);
    CHECK(d->command.type == Command::Type::PUT);
    CHECK_STR_EQ(d->command.key, "name");
    CHECK_STR_EQ(d->command.value, "alice");
}

void TestLogEntryDelRoundTrip() {
    auto e = MakeEntry(42, 9, Command::Type::DEL, "temp");
    std::string s = SerializeLogEntry(e);
    auto d = DeserializeLogEntry(s);
    CHECK(d.has_value());
    CHECK_EQ(d->index, 42);
    CHECK_EQ(d->term, 9);
    CHECK(d->command.type == Command::Type::DEL);
    CHECK_STR_EQ(d->command.key, "temp");
}

void TestLogEntryGetRoundTrip() {
    // GET 理论上不进日志，但序列化格式也应能自洽
    auto e = MakeEntry(1, 1, Command::Type::GET, "ro_key");
    std::string s = SerializeLogEntry(e);
    auto d = DeserializeLogEntry(s);
    CHECK(d.has_value());
    CHECK(d->command.type == Command::Type::GET);
    CHECK_STR_EQ(d->command.key, "ro_key");
}

void TestLogEntryLargeIndexTerm() {
    // 64 位大整数不能丢精度
    auto e = MakeEntry(1234567890123LL, 9876543210LL,
                       Command::Type::PUT, "k", "v");
    auto d = DeserializeLogEntry(SerializeLogEntry(e));
    CHECK(d.has_value());
    CHECK_EQ(d->index, 1234567890123LL);
    CHECK_EQ(d->term, 9876543210LL);
}

void TestLogEntryCorruptInputs() {
    CHECK(!DeserializeLogEntry("").has_value());
    CHECK(!DeserializeLogEntry("1").has_value());                 // 只有 index
    CHECK(!DeserializeLogEntry("1 2").has_value());               // 缺 type
    CHECK(!DeserializeLogEntry("1 2 PUT").has_value());           // 缺 key
    CHECK(!DeserializeLogEntry("1 2 UNKNOWN k").has_value());     // 未知 type
    CHECK(!DeserializeLogEntry("1 2 PUT k").has_value());         // PUT 缺 value
    CHECK(!DeserializeLogEntry("a b PUT k v").has_value());       // 数字字段非数字
    // 尾部多余 token 被忽略（与 Command 解析一致）：字段完整即成功
    auto tail = DeserializeLogEntry("1 2 PUT k v garbage");
    CHECK(tail.has_value());
    CHECK_STR_EQ(tail->command.key, "k");
    CHECK_STR_EQ(tail->command.value, "v");
}

// ==================== Meta ====================

void TestMetaRoundTrip() {
    std::string s = SerializeMeta(5, 2);
    CHECK_STR_EQ(s, "META 5 2");
    auto d = DeserializeMeta(s);
    CHECK(d.has_value());
    CHECK_EQ(d->first, 5);
    CHECK_EQ(d->second, 2);
}

void TestMetaVotedForMinusOne() {
    // votedFor = -1 是合法值（未投票）
    auto d = DeserializeMeta(SerializeMeta(0, -1));
    CHECK(d.has_value());
    CHECK_EQ(d->first, 0);
    CHECK_EQ(d->second, -1);
}

void TestMetaCorruptInputs() {
    CHECK(!DeserializeMeta("").has_value());
    CHECK(!DeserializeMeta("FOO 5 2").has_value());      // tag 错误
    CHECK(!DeserializeMeta("META 5").has_value());       // 字段缺失
    CHECK(!DeserializeMeta("META x 2").has_value());     // 非数字
}

// ==================== Snapshot ====================

static std::string BuildSnapshotRaw(int64_t idx, int64_t term,
                                    const std::string& data) {
    std::string buf;
    buf.resize(24);
    int64_t len = static_cast<int64_t>(data.size());
    std::memcpy(buf.data(),      &idx,  8);
    std::memcpy(buf.data() + 8,  &term, 8);
    std::memcpy(buf.data() + 16, &len,  8);
    buf.append(data);
    return buf;
}

void TestSnapshotRoundTrip() {
    Snapshot snap{100, 7, std::string{"k1\nv1\nk2\nv2\n"}};
    std::string raw = SerializeSnapshot(snap);
    auto d = DeserializeSnapshot(raw);
    CHECK(d.has_value());
    CHECK_EQ(d->lastIncludedIndex, 100);
    CHECK_EQ(d->lastIncludedTerm, 7);
    CHECK_STR_EQ(d->data, "k1\nv1\nk2\nv2\n");
}

void TestSnapshotEmptyData() {
    Snapshot snap{0, 0, ""};
    auto d = DeserializeSnapshot(SerializeSnapshot(snap));
    CHECK(d.has_value());
    CHECK_EQ(d->lastIncludedIndex, 0);
    CHECK(d->data.empty());
}

void TestSnapshotBinaryData() {
    // 含 NUL、0xFF、换行等二进制字节
    std::string bin;
    for (int i = 0; i < 256; ++i) bin.push_back(static_cast<char>(i));
    Snapshot snap{55, 3, bin};
    auto d = DeserializeSnapshot(SerializeSnapshot(snap));
    CHECK(d.has_value());
    CHECK_EQ(d->data.size(), bin.size());
    CHECK(d->data == bin);
}

void TestSnapshotCorruptHeader() {
    CHECK(!DeserializeSnapshot("").has_value());
    CHECK(!DeserializeSnapshot("short").has_value());
    CHECK(!DeserializeSnapshot(std::string(23, 'x')).has_value()); // 差 1 字节
}

void TestSnapshotDataLenMismatch() {
    // 声明 100 字节，实际只有 5 字节
    std::string raw = BuildSnapshotRaw(10, 2, "12345");
    int64_t fakeLen = 100;
    std::memcpy(raw.data() + 16, &fakeLen, 8);
    CHECK(!DeserializeSnapshot(raw).has_value());
}

void TestSnapshotDataLenNegative() {
    // 声明 -1 字节
    std::string raw = BuildSnapshotRaw(10, 2, "12345");
    int64_t neg = -1;
    std::memcpy(raw.data() + 16, &neg, 8);
    CHECK(!DeserializeSnapshot(raw).has_value());
}

void TestSnapshotDataTooLarge() {
    // 声明超过 1GB
    std::string raw = BuildSnapshotRaw(10, 2, "");
    int64_t huge = (1LL << 30) + 1;
    std::memcpy(raw.data() + 16, &huge, 8);
    CHECK(!DeserializeSnapshot(raw).has_value());
}

int main() {
    std::cout << "===== Raft Message Tests =====\n";

    RUN_TEST(TestLogEntryPutRoundTrip);
    RUN_TEST(TestLogEntryDelRoundTrip);
    RUN_TEST(TestLogEntryGetRoundTrip);
    RUN_TEST(TestLogEntryLargeIndexTerm);
    RUN_TEST(TestLogEntryCorruptInputs);
    RUN_TEST(TestMetaRoundTrip);
    RUN_TEST(TestMetaVotedForMinusOne);
    RUN_TEST(TestMetaCorruptInputs);
    RUN_TEST(TestSnapshotRoundTrip);
    RUN_TEST(TestSnapshotEmptyData);
    RUN_TEST(TestSnapshotBinaryData);
    RUN_TEST(TestSnapshotCorruptHeader);
    RUN_TEST(TestSnapshotDataLenMismatch);
    RUN_TEST(TestSnapshotDataLenNegative);
    RUN_TEST(TestSnapshotDataTooLarge);

    TEST_SUMMARY_RETURN();
}
