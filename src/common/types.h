//types.h

#pragma once
#include<cstdint>
#include<string>

enum class Role
{
    Leader,
    Candidate,
    Follower
};

struct PeerInfo
{
    int32_t peerId;
    std::string host;
    uint16_t port;
};