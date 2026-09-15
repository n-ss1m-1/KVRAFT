#include <cassert>
#include <iostream>
#include <string>
#include <sstream>
#include "../src/raft/raft_message_codec.h"

// ==================== 辅助 ====================

LogEntry MakePut(int64_t index, int64_t term,
                 const std::string& key, const std::string& value) {
    LogEntry e;
    e.index = index;
    e.term = term;
    e.command = Command{Command::Type::PUT, key, value};
    return e;
}

LogEntry MakeDel(int64_t index, int64_t term, const std::string& key) {
    LogEntry e;
    e.index = index;
    e.term = term;
    e.command = Command{Command::Type::DEL, key, ""};
    return e;
}

// ==================== LogEntry 往返测试 ====================

void TestLogEntryPutRoundTrip() {
    LogEntry e = MakePut(10, 5, "hello", "world");

    std::string s = SerializeLogEntry(e);
    // 期望格式："10 5 PUT hello world"
    std::cout << "  Serialized: \"" << s << "\"\n";

    auto d = DeserializeLogEntry(s);
    assert(d.has_value());
    assert(d->index == 10);
    assert(d->term == 5);
    assert(d->command.type == Command::Type::PUT);
    assert(d->command.key == "hello");
    assert(d->command.value == "world");

    std::cout << "[PASS] TestLogEntryPutRoundTrip\n";
}

void TestLogEntryDelRoundTrip() {
    LogEntry e = MakeDel(7, 3, "oldkey");

    std::string s = SerializeLogEntry(e);
    // 期望格式："7 3 DEL oldkey"
    std::cout << "  Serialized: \"" << s << "\"\n";

    auto d = DeserializeLogEntry(s);
    assert(d.has_value());
    assert(d->index == 7);
    assert(d->term == 3);
    assert(d->command.type == Command::Type::DEL);
    assert(d->command.key == "oldkey");
    assert(d->command.value.empty());  // DEL 没有 value

    std::cout << "[PASS] TestLogEntryDelRoundTrip\n";
}

void TestLogEntryLargeNumbers() {
    LogEntry e = MakePut(9223372036854775807LL,  // int64 max
                         9223372036854775806LL,
                         "big", "value");

    auto d = DeserializeLogEntry(SerializeLogEntry(e));
    assert(d.has_value());
    assert(d->index == 9223372036854775807LL);
    assert(d->term == 9223372036854775806LL);

    std::cout << "[PASS] TestLogEntryLargeNumbers\n";
}

// ==================== LogEntry 非法输入 ====================

void TestLogEntryInvalid() {
    // 空行
    assert(!DeserializeLogEntry("").has_value());

    // 字段缺失
    assert(!DeserializeLogEntry("10 5 PUT").has_value());
    assert(!DeserializeLogEntry("10 5").has_value());
    assert(!DeserializeLogEntry("10").has_value());

    // 类型不识别
    assert(!DeserializeLogEntry("10 5 BOGUS hello world").has_value());

    // index/term 不是数字
    assert(!DeserializeLogEntry("abc 5 PUT hello world").has_value());
    assert(!DeserializeLogEntry("10 xyz PUT hello world").has_value());

    // PUT 缺 value
    assert(!DeserializeLogEntry("10 5 PUT hello").has_value());

    std::cout << "[PASS] TestLogEntryInvalid\n";
}

// ==================== Meta 往返测试 ====================

void TestMetaRoundTrip() {
    std::string s = SerializeMeta(5, 1);
    std::cout << "  Serialized: \"" << s << "\"\n";
    // 期望："META 5 1"

    auto d = DeserializeMeta(s);
    assert(d.has_value());
    assert(d->first == 5);
    assert(d->second == 1);

    std::cout << "[PASS] TestMetaRoundTrip\n";
}

void TestMetaVotedForNone() {
    // votedFor = -1 表示本任期还没投票
    std::string s = SerializeMeta(3, -1);
    std::cout << "  Serialized: \"" << s << "\"\n";

    auto d = DeserializeMeta(s);
    assert(d.has_value());
    assert(d->first == 3);
    assert(d->second == -1);

    std::cout << "[PASS] TestMetaVotedForNone\n";
}

void TestMetaInvalid() {
    assert(!DeserializeMeta("").has_value());
    assert(!DeserializeMeta("META 5").has_value());
    assert(!DeserializeMeta("META").has_value());
    assert(!DeserializeMeta("WRONG 5 1").has_value());
    assert(!DeserializeMeta("META abc 1").has_value());

    std::cout << "[PASS] TestMetaInvalid\n";
}

// ==================== 多行序列化 ====================

void TestMultipleLogEntries() {
    std::vector<LogEntry> entries = {
        MakePut(1, 1, "a", "1"),
        MakePut(2, 1, "b", "2"),
        MakeDel(3, 2, "a"),
        MakePut(4, 2, "c", "3"),
    };

    // 逐条序列化，用 \n 拼接，模拟日志文件
    std::string fileContent;
    for (const auto& e : entries) {
        fileContent += SerializeLogEntry(e) + "\n";
    }
    std::cout << "  File content:\n" << fileContent;

    // 逐行反序列化
    std::istringstream iss(fileContent);
    std::string line;
    std::vector<LogEntry> parsed;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        auto e = DeserializeLogEntry(line);
        assert(e.has_value());
        parsed.push_back(*e);
    }

    assert(parsed.size() == 4);
    for (size_t i = 0; i < entries.size(); i++) {
        assert(parsed[i].index == entries[i].index);
        assert(parsed[i].term == entries[i].term);
        assert(parsed[i].command.type == entries[i].command.type);
        assert(parsed[i].command.key == entries[i].command.key);
        assert(parsed[i].command.value == entries[i].command.value);
    }

    std::cout << "[PASS] TestMultipleLogEntries\n";
}

// ==================== main ====================

int main() {
    std::cout << "===== Codec Tests =====\n";

    TestLogEntryPutRoundTrip();
    TestLogEntryDelRoundTrip();
    TestLogEntryLargeNumbers();
    TestLogEntryInvalid();

    TestMetaRoundTrip();
    TestMetaVotedForNone();
    TestMetaInvalid();

    TestMultipleLogEntries();

    std::cout << "\nAll codec tests passed!\n";
    return 0;
}