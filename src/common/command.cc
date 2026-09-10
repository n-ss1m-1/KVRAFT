#include "command.h"
#include<sstream>


std::optional<Command> ParseCommand(const std::string& line)
{
    if(line.empty()) return std::nullopt;               //空行

    std::istringstream iss(line);                   //使用istringstream来解析输入
    Command command;
    std::string type,key,value;
    if(!(iss>>type)) return std::nullopt;
    if(!(iss>>key)) return std::nullopt;

    if(type=="PUT") 
    {
        if(!(iss>>value)) return std::nullopt;
        return Command(Command::Type::PUT,key,value);
    }
    else if(type=="GET") return Command(Command::Type::GET,key);
    else if(type=="DEL") return Command(Command::Type::DEL,key);
    
    return std::nullopt;                                //无效指令
}

std::vector<Command> ParseCommand(const std::vector<std::string>& lines)
{
    std::vector<Command> commands;

    for(const std::string& line : lines)
    {
        std::optional<Command> command=ParseCommand(line);
        if(command.has_value()) commands.emplace_back(command.value());
    }
    
    return commands;
}
