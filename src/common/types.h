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

struct peerInfo
{
    int32_t peerId;
    std::string host;
    int port;
};