//test_command.cc
// 覆盖 common/command：文本协议解析（PUT/GET/DEL）、边界输入、批量解析、类型字符串互转

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "common/command.h"
#include "test_util.h"

// ==================== 正常解析 ====================

void TestParsePut() {
    auto c = ParseCommand("PUT key1 value1");
    CHECK(c.has_value());
    CHECK(c->type == Command::Type::PUT);
    CHECK_STR_EQ(c->key, "key1");
    CHECK_STR_EQ(c->value, "value1");
}

void TestParseGet() {
    auto c = ParseCommand("GET key1");
    CHECK(c.has_value());
    CHECK(c->type == Command::Type::GET);
    CHECK_STR_EQ(c->key, "key1");
    CHECK_STR_EQ(c->value, "");   // GET 无 value
}

void TestParseDel() {
    auto c = ParseCommand("DEL key1");
    CHECK(c.has_value());
    CHECK(c->type == Command::Type::DEL);
    CHECK_STR_EQ(c->key, "key1");
}

void TestParseNumbersAndSymbols() {
    // key/value 允许数字、下划线、路径、符号等非空白字符
    auto c = ParseCommand("PUT /api/v1/name 12345");
    CHECK(c.has_value());
    CHECK_STR_EQ(c->key, "/api/v1/name");
    CHECK_STR_EQ(c->value, "12345");
}

// ==================== 空白处理 ====================

void TestParseLeadingTrailingSpaces() {
    auto c = ParseCommand("   PUT   key1   value1   ");
    CHECK(c.has_value());
    CHECK(c->type == Command::Type::PUT);
    CHECK_STR_EQ(c->key, "key1");
    CHECK_STR_EQ(c->value, "value1");
}

void TestParseTabs() {
    auto c = ParseCommand("\tGET\tkey1\t");
    CHECK(c.has_value());
    CHECK(c->type == Command::Type::GET);
    CHECK_STR_EQ(c->key, "key1");
}

void TestParseEmptyLine() {
    CHECK(!ParseCommand("").has_value());
    CHECK(!ParseCommand("   ").has_value());
    CHECK(!ParseCommand("\t\t").has_value());
}

// ==================== 非法输入 ====================

void TestParseUnknownType() {
    // 小写不匹配（大小写敏感）
    CHECK(!ParseCommand("put a b").has_value());
    CHECK(!ParseCommand("get a").has_value());
    CHECK(!ParseCommand("del a").has_value());
    // 完全未知
    CHECK(!ParseCommand("SET a b").has_value());
    CHECK(!ParseCommand("DELETE a").has_value());
    CHECK(!ParseCommand("12345 a b").has_value());
}

void TestParseMissingArgs() {
    CHECK(!ParseCommand("PUT").has_value());          // 缺 key
    CHECK(!ParseCommand("PUT key").has_value());      // 缺 value
    CHECK(!ParseCommand("GET").has_value());          // 缺 key
    CHECK(!ParseCommand("DEL").has_value());          // 缺 key
}

void TestParseExtraTokensIgnored() {
    // 记录当前实现行为：多余 token 被 istringstream 静默忽略
    // （如果将来改为严格校验，本用例应同步调整）
    auto c = ParseCommand("PUT a b c");
    CHECK(c.has_value());
    CHECK_STR_EQ(c->key, "a");
    CHECK_STR_EQ(c->value, "b");

    auto g = ParseCommand("GET a extra");
    CHECK(g.has_value());
    CHECK(g->type == Command::Type::GET);
    CHECK_STR_EQ(g->key, "a");
}

void TestParseValueWithSpaceNotSupported() {
    // 协议限制：value 含空格时只能取到第一个单词（>> 以空白分隔）
    auto c = ParseCommand("PUT key hello world");
    CHECK(c.has_value());
    CHECK_STR_EQ(c->value, "hello");
}

// ==================== 批量解析 ====================

void TestParseBatch() {
    std::vector<std::string> lines = {
        "PUT a 1",
        "garbage line",       // 无效，跳过
        "",                   // 空行，跳过
        "GET a",
        "DEL a",
        "PUT b",              // 缺 value，跳过
    };
    auto cmds = ParseCommand(lines);
    CHECK_EQ(cmds.size(), 3u);
    CHECK(cmds[0].type == Command::Type::PUT);
    CHECK(cmds[1].type == Command::Type::GET);
    CHECK(cmds[2].type == Command::Type::DEL);
}

void TestParseBatchEmpty() {
    auto cmds = ParseCommand(std::vector<std::string>{});
    CHECK(cmds.empty());
}

// ==================== 类型字符串互转 ====================

void TestTypeToString() {
    CHECK_STR_EQ(CommandTypeToString(Command::Type::PUT), "PUT");
    CHECK_STR_EQ(CommandTypeToString(Command::Type::GET), "GET");
    CHECK_STR_EQ(CommandTypeToString(Command::Type::DEL), "DEL");
}

void TestStringToType() {
    CHECK(StringToCommandType("PUT").value() == Command::Type::PUT);
    CHECK(StringToCommandType("GET").value() == Command::Type::GET);
    CHECK(StringToCommandType("DEL").value() == Command::Type::DEL);
    CHECK(!StringToCommandType("put").has_value());
    CHECK(!StringToCommandType("").has_value());
    CHECK(!StringToCommandType("POST").has_value());
}

void TestTypeRoundTrip() {
    for (auto t : {Command::Type::PUT, Command::Type::GET, Command::Type::DEL}) {
        auto back = StringToCommandType(CommandTypeToString(t));
        CHECK(back.has_value());
        CHECK(back.value() == t);
    }
}

int main() {
    std::cout << "===== Command Tests =====\n";

    RUN_TEST(TestParsePut);
    RUN_TEST(TestParseGet);
    RUN_TEST(TestParseDel);
    RUN_TEST(TestParseNumbersAndSymbols);
    RUN_TEST(TestParseLeadingTrailingSpaces);
    RUN_TEST(TestParseTabs);
    RUN_TEST(TestParseEmptyLine);
    RUN_TEST(TestParseUnknownType);
    RUN_TEST(TestParseMissingArgs);
    RUN_TEST(TestParseExtraTokensIgnored);
    RUN_TEST(TestParseValueWithSpaceNotSupported);
    RUN_TEST(TestParseBatch);
    RUN_TEST(TestParseBatchEmpty);
    RUN_TEST(TestTypeToString);
    RUN_TEST(TestStringToType);
    RUN_TEST(TestTypeRoundTrip);

    TEST_SUMMARY_RETURN();
}
