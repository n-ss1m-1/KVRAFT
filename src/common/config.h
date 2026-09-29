//config.h

#include<cstdint>
#include<string>
#include<vector>

#include "common/types.h"

namespace config
{
    struct NodeConfig
    {
        PeerInfo peerInfo;
        uint16_t clientServerPort;
    };
    //集群拓扑(含自己)
    inline const std::vector<NodeConfig> kCluster = 
    {
        // {{nodeId, host, port},clientServerPort}
        {{0, "127.0.0.1", 9001},8001},
        {{1, "127.0.0.1", 9002},8002},
        {{2, "127.0.0.1", 9003},8003},
    };

    //数据目录前缀(实际路径 = kDataDirPrefix + "/node" + nodeId)
    inline constexpr const char* kDataDirPrefix = "data";

    //Raft时间参数
    inline constexpr int kHeartbeatIntervalMs   = 50;
    inline constexpr int kElectionTimeoutMinMs = 150;
    inline constexpr int kElectionTimeoutMaxMs = 300;

};



/*
? or ？表示疑问 可以改进的地方
▲ 表示重要的地方
! 表示错误之后改正的地方
*/