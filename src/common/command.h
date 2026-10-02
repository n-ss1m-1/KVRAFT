//command.h
#pragma once
#include<string>
#include<optional>
#include<vector>
#include<cstdint>

struct Command
{
    enum class Type : uint32_t
    {
        PUT     = 0,
        GET     = 1,
        DEL     = 2
    };

    Command()=default;
    Command(Type t,std::string k):type(t),key(std::move(k)) {}
    Command(Type t,std::string k,std::string v):type(t),key(std::move(k)),value(std::move(v)) {}


    Command::Type type;
    std::string key;
    std::string value;
};

//解析一行
std::optional<Command> ParseCommand(const std::string& line);

//解析所有
std::vector<Command> ParseCommand(const std::vector<std::string>& lines);


//用于raft_message的序列化和反序列化
std::string CommandTypeToString(const Command::Type& type);
std::optional<Command::Type> StringToCommandType(const std::string& type);