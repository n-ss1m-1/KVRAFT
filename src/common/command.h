#pragma once
#include<string>
#include<optional>
#include<vector>

struct Command
{
    enum class Type
    {
        PUT,
        GET,
        DEL
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