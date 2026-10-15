//test_rpc_message.cc
// 覆盖 rpc/rpc_message：6 类 RPC 消息的序列化/反序列化 round-trip。
// 模拟网络层外层：先读 type + reqId，再把剩余流交给对应 Deserialize。

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "rpc/rpc_message.h"
#include "common/command.h"
#include "test_util.h"

// 读外层头部（type + reqId），返回剩余流
static std::istringstream StripHeader(const std::string& wire,
                                      uint32_t* type, uint64_t* reqId) {
    std::istringstream iss(wire);
    iss >> *type >> *reqId;
    return iss;   // 剩余内容继续由各 Deserialize 读取
}

static LogEntry MakeEntry(int64_t index, int64_t term,
                          Command::Type type, std::string key,
                          std::string value = "") {
    return LogEntry{index, term, Command{type, std::move(key), std::move(value)}};
}

// ==================== RequestVote ====================

void TestRequestVoteArgsRoundTrip() {
    RequestVoteArgs args;
    args.term = 5;
    args.candidateId = 2;
    args.lastLogIndex = 100;
    args.lastLogTerm = 4;

    std::string wire = SerializeRequestVoteArgs(77, args);
    uint32_t type; uint64_t reqId;
    auto iss = StripHeader(wire, &type, &reqId);
    CHECK_EQ(type, static_cast<uint32_t>(RpcMessageType::kRequestVote));
    CHECK_EQ(reqId, 77u);

    auto d = DeserializeRequestVoteArgs(iss);
    CHECK(d.has_value());
    CHECK_EQ(d->term, 5);
    CHECK_EQ(d->candidateId, 2);
    CHECK_EQ(d->lastLogIndex, 100);
    CHECK_EQ(d->lastLogTerm, 4);
}

void TestRequestVoteReplyRoundTrip() {
    RequestVoteReply reply{3, true};
    std::string wire = SerializeRequestVoteReply(9, reply);
    uint32_t type; uint64_t reqId;
    auto iss = StripHeader(wire, &type, &reqId);
    CHECK_EQ(type, static_cast<uint32_t>(RpcMessageType::kRequestVoteReply));
    CHECK_EQ(reqId, 9u);

    auto d = DeserializeRequestVoteReply(iss);
    CHECK(d.has_value());
    CHECK_EQ(d->term, 3);
    CHECK(d->voteGranted);
}

void TestRequestVoteReplyFalse() {
    RequestVoteReply reply{8, false};
    std::string wire = SerializeRequestVoteReply(1, reply);
    uint32_t type; uint64_t reqId;
    auto iss = StripHeader(wire, &type, &reqId);
    auto d = DeserializeRequestVoteReply(iss);
    CHECK(d.has_value());
    CHECK(!d->voteGranted);
}

// ==================== AppendEntries ====================

void TestAppendEntriesHeartbeat() {
    // 心跳：entries 为空
    AppendEntriesArgs args;
    args.term = 4; args.leaderId = 1;
    args.prevLogIndex = 10; args.prevLogTerm = 4;
    args.leaderCommit = 8;
    // entries 为空

    std::string wire = SerializeAppendEntriesArgs(100, args);
    uint32_t type; uint64_t reqId;
    auto iss = StripHeader(wire, &type, &reqId);
    CHECK_EQ(type, static_cast<uint32_t>(RpcMessageType::kAppendEntries));
    CHECK_EQ(reqId, 100u);

    auto d = DeserializeAppendEntriesArgs(iss);
    CHECK(d.has_value());
    CHECK_EQ(d->term, 4);
    CHECK_EQ(d->leaderId, 1);
    CHECK_EQ(d->prevLogIndex, 10);
    CHECK_EQ(d->prevLogTerm, 4);
    CHECK_EQ(d->leaderCommit, 8);
    CHECK(d->entries.empty());
}

void TestAppendEntriesWithEntries() {
    AppendEntriesArgs args;
    args.term = 6; args.leaderId = 0;
    args.prevLogIndex = 3; args.prevLogTerm = 5;
    args.leaderCommit = 3;
    args.entries = {
        MakeEntry(4, 6, Command::Type::PUT, "a", "1"),
        MakeEntry(5, 6, Command::Type::PUT, "b", "2"),
        MakeEntry(6, 6, Command::Type::DEL, "a"),
    };

    std::string wire = SerializeAppendEntriesArgs(55, args);
    uint32_t type; uint64_t reqId;
    auto iss = StripHeader(wire, &type, &reqId);
    auto d = DeserializeAppendEntriesArgs(iss);
    CHECK(d.has_value());
    CHECK_EQ(d->entries.size(), 3u);

    CHECK_EQ(d->entries[0].index, 4);
    CHECK_EQ(d->entries[0].term, 6);
    CHECK(d->entries[0].command.type == Command::Type::PUT);
    CHECK_STR_EQ(d->entries[0].command.key, "a");
    CHECK_STR_EQ(d->entries[0].command.value, "1");

    CHECK_EQ(d->entries[1].index, 5);
    CHECK_STR_EQ(d->entries[1].command.value, "2");

    CHECK_EQ(d->entries[2].index, 6);
    CHECK(d->entries[2].command.type == Command::Type::DEL);
}

void TestAppendEntriesManyEntries() {
    AppendEntriesArgs args;
    args.term = 2; args.leaderId = 1;
    for (int i = 1; i <= 200; ++i) {
        args.entries.push_back(
            MakeEntry(i, 2, Command::Type::PUT,
                      "k" + std::to_string(i), "v" + std::to_string(i)));
    }
    std::string wire = SerializeAppendEntriesArgs(1, args);
    uint32_t type; uint64_t reqId;
    auto iss = StripHeader(wire, &type, &reqId);
    auto d = DeserializeAppendEntriesArgs(iss);
    CHECK(d.has_value());
    CHECK_EQ(d->entries.size(), 200u);
    CHECK_EQ(d->entries.back().index, 200);
    CHECK_STR_EQ(d->entries.back().command.value, "v200");
}

void TestAppendEntriesReplyRoundTrip() {
    AppendEntriesReply reply{4, true, 33, 0};
    std::string wire = SerializeAppendEntriesReply(3, reply);
    uint32_t type; uint64_t reqId;
    auto iss = StripHeader(wire, &type, &reqId);
    CHECK_EQ(type, static_cast<uint32_t>(RpcMessageType::kAppendEntriesReply));
    CHECK_EQ(reqId, 3u);
    auto d = DeserializeAppendEntriesReply(iss);
    CHECK(d.has_value());
    CHECK_EQ(d->term, 4);
    CHECK(d->success);
    CHECK_EQ(d->matchIndex, 33);
    CHECK_EQ(d->conflictIndex, 0);
}

void TestAppendEntriesReplyConflict() {
    AppendEntriesReply reply{2, false, 0, 7};
    std::string wire = SerializeAppendEntriesReply(3, reply);
    uint32_t type; uint64_t reqId;
    auto iss = StripHeader(wire, &type, &reqId);
    auto d = DeserializeAppendEntriesReply(iss);
    CHECK(d.has_value());
    CHECK(!d->success);
    CHECK_EQ(d->conflictIndex, 7);
}

// ==================== InstallSnapshot ====================

void TestInstallSnapshotArgsRoundTrip() {
    std::string data = "k1\nv1\nk2\nvalue with spaces\n";
    InstallSnapshotArgs args;
    args.term = 10; args.leaderId = 0;
    args.lastIncludedIndex = 50;
    args.lastIncludedTerm = 9;
    args.data = data;
    args.dataLen = static_cast<int64_t>(data.size());

    std::string wire = SerializeInstallSnapshotArgs(21, args);
    uint32_t type; uint64_t reqId;
    auto iss = StripHeader(wire, &type, &reqId);
    CHECK_EQ(type, static_cast<uint32_t>(RpcMessageType::kInstallSnapshot));
    CHECK_EQ(reqId, 21u);

    auto d = DeserializeInstallSnapshotArgs(iss);
    CHECK(d.has_value());
    CHECK_EQ(d->term, 10);
    CHECK_EQ(d->leaderId, 0);
    CHECK_EQ(d->lastIncludedIndex, 50);
    CHECK_EQ(d->lastIncludedTerm, 9);
    CHECK_EQ(d->dataLen, static_cast<int64_t>(data.size()));
    CHECK(d->data == data);
}

void TestInstallSnapshotBinaryData() {
    // 二进制快照（含 NUL / 0xFF）
    std::string data;
    for (int i = 0; i < 256; ++i) data.push_back(static_cast<char>(i));
    InstallSnapshotArgs args;
    args.term = 1; args.leaderId = 0;
    args.lastIncludedIndex = 1; args.lastIncludedTerm = 1;
    args.data = data;
    args.dataLen = data.size();

    std::string wire = SerializeInstallSnapshotArgs(1, args);
    uint32_t type; uint64_t reqId;
    auto iss = StripHeader(wire, &type, &reqId);
    auto d = DeserializeInstallSnapshotArgs(iss);
    CHECK(d.has_value());
    CHECK(d->data == data);
}

void TestInstallSnapshotReplyRoundTrip() {
    InstallSnapshotReply reply{7, 50, true};
    std::string wire = SerializeInstallSnapshotReply(8, reply);
    uint32_t type; uint64_t reqId;
    auto iss = StripHeader(wire, &type, &reqId);
    CHECK_EQ(type, static_cast<uint32_t>(RpcMessageType::kInstallSnapshotReply));
    CHECK_EQ(reqId, 8u);
    auto d = DeserializeInstallSnapshotReply(iss);
    CHECK(d.has_value());
    CHECK_EQ(d->term, 7);
    CHECK_EQ(d->lastIncludedIndex, 50);
    CHECK(d->success);
}

// ==================== 损坏输入 ====================

void TestCorruptInputs() {
    // RequestVoteArgs 字段不足
    {
        std::istringstream iss("5 2 100");   // 缺 lastLogTerm
        CHECK(!DeserializeRequestVoteArgs(iss).has_value());
    }
    // RequestVoteReply 字段不足
    {
        std::istringstream iss("5");
        CHECK(!DeserializeRequestVoteReply(iss).has_value());
    }
    // AppendEntriesArgs header 不足
    {
        std::istringstream iss("4 1 10 4 8");   // 缺 count
        CHECK(!DeserializeAppendEntriesArgs(iss).has_value());
    }
    // AppendEntriesArgs 声明 2 条但只给 1 条
    {
        std::string wire = "3 1 4 1 10 4 8 2\n1 4 PUT a 1";
        std::istringstream iss(wire);
        uint32_t t; uint64_t r;
        iss >> t >> r;
        CHECK(!DeserializeAppendEntriesArgs(iss).has_value());
    }
    // AppendEntriesArgs 某条 entry 损坏
    {
        std::string wire = "3 1 4 1 10 4 8 1\nGARBAGE";
        std::istringstream iss(wire);
        uint32_t t; uint64_t r;
        iss >> t >> r;
        CHECK(!DeserializeAppendEntriesArgs(iss).has_value());
    }
    // AppendEntriesReply 字段不足
    {
        std::istringstream iss("4 1 33");
        CHECK(!DeserializeAppendEntriesReply(iss).has_value());
    }
    // InstallSnapshotArgs 字段不足
    {
        std::istringstream iss("10 0 50 9");   // 缺 dataLen
        CHECK(!DeserializeInstallSnapshotArgs(iss).has_value());
    }
    // InstallSnapshotReply 字段不足
    {
        std::istringstream iss("7");
        CHECK(!DeserializeInstallSnapshotReply(iss).has_value());
    }
}

// ==================== 消息类型枚举值稳定性 ====================

void TestMessageTypeValues() {
    // 显式钉死枚举值：网络协议上的数字不能因插入新枚举而漂移
    CHECK_EQ(static_cast<uint32_t>(RpcMessageType::kRequestVote), 1u);
    CHECK_EQ(static_cast<uint32_t>(RpcMessageType::kRequestVoteReply), 2u);
    CHECK_EQ(static_cast<uint32_t>(RpcMessageType::kAppendEntries), 3u);
    CHECK_EQ(static_cast<uint32_t>(RpcMessageType::kAppendEntriesReply), 4u);
    CHECK_EQ(static_cast<uint32_t>(RpcMessageType::kInstallSnapshot), 5u);
    CHECK_EQ(static_cast<uint32_t>(RpcMessageType::kInstallSnapshotReply), 6u);
}

int main() {
    std::cout << "===== RPC Message Tests =====\n";

    RUN_TEST(TestRequestVoteArgsRoundTrip);
    RUN_TEST(TestRequestVoteReplyRoundTrip);
    RUN_TEST(TestRequestVoteReplyFalse);
    RUN_TEST(TestAppendEntriesHeartbeat);
    RUN_TEST(TestAppendEntriesWithEntries);
    RUN_TEST(TestAppendEntriesManyEntries);
    RUN_TEST(TestAppendEntriesReplyRoundTrip);
    RUN_TEST(TestAppendEntriesReplyConflict);
    RUN_TEST(TestInstallSnapshotArgsRoundTrip);
    RUN_TEST(TestInstallSnapshotBinaryData);
    RUN_TEST(TestInstallSnapshotReplyRoundTrip);
    RUN_TEST(TestCorruptInputs);
    RUN_TEST(TestMessageTypeValues);

    TEST_SUMMARY_RETURN();
}
