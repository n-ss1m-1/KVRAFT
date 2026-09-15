//raft_message_codec.h

#pragma once

#include "raft_message.h"


//LogEntry序列化和反序列化
std::string SerializeLogEntry(const LogEntry& entry);

std::optional<LogEntry> DeserializeLogEntry(const std::string& line);


//Raft元状态序列化和反序列化 元状态：当前所在任期、投票给谁
std::string SerializeMeta(int64_t currentTerm,int32_t votedFor);

std::optional<std::pair<int64_t,int32_t>> DeserializeMeta(const std::string& line);




