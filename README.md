# Raft KV 分布式存储

基于 Raft 共识算法的分布式键值存储系统，使用 C++17 实现。

## 项目简介

一个 3 节点高可用 KV 存储集群，支持：

- 自动 Leader 选举与故障转移
- 日志复制与强一致性提交
- WAL 持久化 + 快照压缩
- 客户端 PUT / GET / DEL 接口
- 基于 muduo 的高性能异步网络通信

目标场景：存储对一致性要求高的元数据（如服务发现、配置中心、分布式锁）。

## 功能特性

### 已实现

| 模块 | 功能 |
| --- | --- |
| 共识层 | Leader 选举、日志复制、提交规则、任期管理 |
| 存储层 | WAL 追加写、ftruncate 冲突覆盖、快照生成、日志压缩 |
| 网络层 | 基于 muduo 的异步 RPC（RequestVote / AppendEntries / InstallSnapshot） |
| 客户端 | 文本协议 PUT/GET/DEL，Leader 重定向 |
| 持久化 | 元状态 + 日志 + 快照独立文件，崩溃恢复 |
| 并发 | 线程池处理客户端请求，读写锁保护状态机 |
| 可观测 | spdlog 分级日志（终端 + 文件双 sink） |

### 待实现

- Leader no-op 日志（加快新 Leader 提交）
- ReadIndex / Lease Read（线性一致读）
- 批量提交（group commit）优化
- 成员变更（动态添加/删除节点）
- 自研 epoll 网络层（替换 muduo）

## 系统架构

### 整体分层

```
┌────────────────────────────────────────────┐
│                   客户端                    │
└──────────────────┬─────────────────────────┘
                   │ TCP (文本协议)
                   ▼
┌────────────────────────────────────────────┐
│  ClientServer (客户端端口 8001/8002/8003)   │
│  - 解析 PUT/GET/DEL                        │
│  - PUT/DEL → RaftNode::Start()             │
│  - GET    → KVStore::Get()（本地读）        │
└──────────────────┬─────────────────────────┘
                   │
                   ▼
┌────────────────────────────────────────────┐
│  RaftNode (Raft 核心)                       │
│  - 状态：Follower / Candidate / Leader      │
│  - 选举、日志复制、提交、快照触发            │
└───┬────────────┬────────────┬──────────────┘
    │            │            │
    ▼            ▼            ▼
┌────────┐ ┌────────┐ ┌─────────────────────┐
│ KVStore│ │RaftLog │ │  RaftStorage        │
│(状态机) │ │(内存)  │ │  (持久化)           │
└────────┘ └────────┘ └──────────┬──────────┘
                                  │
                                  ▼
                       ┌─────────────────────┐
                       │  Persister (文件 IO) │
                       │  meta.txt           │
                       │  raft.log           │
                       │  snapshot.bin       │
                       └─────────────────────┘
```

节点间通信：RpcServer (9001/9002/9003) ← → RpcClient

### 依赖方向

```
main
 ├── ClientServer ──→ RaftNode / KVStore
 ├── RpcServer    ──→ RaftNode
 ├── RpcClient    ──→ RaftTransport（抽象接口）
 └── RaftNode
       ├── KVStore（状态机）
       ├── RaftLog（内存日志）
       ├── RaftStorage（持久化）
       └── RaftTransport（抽象接口，由 RpcClient 实现）
```

> 
> 关键设计：RaftNode 不直接依赖具体网络实现，只依赖 RaftTransport 抽象接口。测试时可用 FakeTransport 注入。

### 端口分配

| 节点 | 客户端端口 | Raft 端口 |
| --- | --- | --- |
| 0 | 8001 | 9001 |
| 1 | 8002 | 9002 |
| 2 | 8003 | 9003 |

### 数据文件

```
data/nodeX/
├── meta.txt     ← currentTerm + votedFor（追加写，读最后一行）
├── raft.log     ← 日志条目（追加写，支持 ftruncate 覆盖）
└── snapshot.bin ← 快照（二进制：index + term + 数据）
```

## 目录结构

```
my_kvraft/
├── CMakeLists.txt
├── README.md
├── .gitignore
│
├── src/
│   ├── main.cc                    # 启动入口：装配所有组件
│   │
│   ├── common/                    # 通用工具
│   │   ├── config.h               # 集群配置、常量
│   │   ├── types.h                # Role 枚举等
│   │   ├── command.h / .cc       # Command 结构与解析
│   │   └── thread_pool.h / .cc   # 线程池
│   │
│   ├── kv/                        # 状态机
│   │   └── kv_store.h / .cc       # 纯内存哈希表 + 读写锁
│   │
│   ├── persist/                   # 底层文件 IO
│   │   └── persister.h / .cc      # open/write/fsync/ftruncate
│   │
│   ├── raft/                      # Raft 核心
│   │   ├── raft_message.h / .cc   # LogEntry + 3 类 RPC 消息
│   │   ├── raft_log.h / .cc       # 内存日志（index/term 管理）
│   │   ├── raft_node.h / .cc      # Raft 节点（选举、复制、提交）
│   │   ├── raft_storage.h / .cc  # 持久化（组合两个 Persister）
│   │   └── raft_transport.h      # RPC 抽象接口
│   │
│   ├── rpc/                       # 网络层
│   │   ├── rpc_message.h / .cc   # RPC 消息序列化
│   │   ├── codec.h                # LengthHeaderCodec（粘包处理）
│   │   ├── rpc_client.h / .cc    # 实现 RaftTransport
│   │   └── rpc_server.h / .cc    # 接收 Raft RPC
│   │
│   ├── client_server/             # 客户端接口
│   │   └── client_server.h / .cc
│   │
│   └── server/                    # （预留）
│
├── tests/
│   ├── test_kv_store.cc
│   ├── test_raft_log.cc
│   ├── test_raft_storage.cc
│   ├── test_codec.cc
│   ├── test_persister.cc
│   └── test_raft_node.cc          # 使用 FakeTransport
│
├── client/
│   └── client.cc                  # 测试客户端
│
├── scripts/
│   ├── test_client.py             # 基础功能测试
│   ├── test_failover.sh           # 故障转移测试
│   ├── test_restart.sh            # 持久化测试
│   └── test_snapshot.sh           # 快照测试
│
├── third_party/
│   └── spdlog/                    # header-only 日志库
│
└── data/                          # 运行时数据目录（.gitignore）
    ├── node0/
    ├── node1/
    └── node2/
```

## 核心模块说明

### RaftNode（核心）

职责：实现 Raft 共识算法，保证多节点日志一致。

状态字段：

| 类型 | 字段 | 说明 |
| --- | --- | --- |
| 持久化 | currentTerm_ | 当前任期 |
| 持久化 | votedFor_ | 本任期投给谁 |
| 持久化 | log_ | 日志条目 |
| 易失 | role_ | Follower / Candidate / Leader |
| 易失 | commitIndex_ | 已提交的最大 index |
| 易失 | lastApplied_ | 已应用到状态机的 index |
| Leader 专有 | nextIndex_[] | 每个 Follower 下一条要发的 index |
| Leader 专有 | matchIndex_[] | 每个 Follower 已确认的 index |

核心方法：

- `Start(cmd)`：客户端提交命令（仅 Leader）
- `Tick()`：定时器驱动，检查选举超时 / 心跳周期
- `HandleRequestVote` / `HandleAppendEntries` / `HandleInstallSnapshot`：处理节点间 RPC
- `HandleXXXReply`：处理 RPC 回复
- `TakeSnapshot()`：日志超过阈值时触发快照
- `Recover()`：启动时从磁盘恢复

### RaftLog（内存日志）

核心不变式：`entries_[i].index == snapshotIndex_ + i + 1`

关键接口：

- `AppendFrom(startIndex, entries)`：追加（覆盖冲突部分）
- `GetEntry(index)`：按 index 查询
- `SetSnapshotIndexAndTerm(index, term)`：快照后更新逻辑起点
- `CheckConsistency(prevIndex, prevTerm)`：Follower 一致性检查

### RaftStorage（持久化）

组合两个 Persister：

- `metaPersister_`：存 currentTerm / votedFor
- `logPersister_`：存日志条目

维护状态：

- `indexToOffset_`：index → 文件字节偏移（用于 ftruncate）
- `lastPersistedIndex_`：已持久化的最大 index

核心接口：

- `AppendLogEntry(entry)`：顺序追加
- `AppendLogEntriesFrom(start, entries)`：覆盖追加（先 ftruncate 再写）
- `SaveSnapshot` / `LoadSnapshot`：快照读写
- `TruncateLogUpTo(index)`：快照后删除旧日志

### KVStore（状态机）

纯内存哈希表 + shared_mutex：

- `ApplyPut` / `ApplyDelete`：只改内存，由 Raft 调用
- `Get`：读操作，不走共识
- `Serialize` / `Deserialize`：快照序列化

### RaftTransport（抽象接口）

```
class RaftTransport {
    virtual void SendRequestVote(int32_t peerId, ...) = 0;
    virtual void SendAppendEntries(int32_t peerId, ...) = 0;
    virtual void SendInstallSnapshot(int32_t peerId, ...) = 0;
};
```

- 生产环境：RpcClient 实现（基于 muduo）
- 测试环境：FakeTransport 实现（同进程直接调用对端 HandleXXX）

## 快速开始

### 环境要求

- Linux（推荐 Ubuntu 20.04+）
- g++ 9+ 或 clang 10+（支持 C++17）
- CMake 3.16+
- muduo 网络库（需预先编译安装）

### 安装 muduo

```
git clone [https://github.com/chenshuo/muduo.git](https://github.com/chenshuo/muduo.git)
cd muduo
mkdir build && cd build
cmake ..
make
sudo make install
```

### 安装 spdlog

```
cd third_party
git clone --depth 1 --branch v1.13.0 [https://github.com/gabime/spdlog.git](https://github.com/gabime/spdlog.git)
```

### 编译

```
mkdir -p build && cd build
cmake ..
make -j4
```

编译产物在 `bin/` 目录。

### 启动 3 节点集群

打开 3 个终端：

```
# 终端 1
./bin/raft-kv 0

# 终端 2（间隔 < 1 秒）
./bin/raft-kv 1

# 终端 3
./bin/raft-kv 2
```

启动日志（其中一个节点会输出）：

```
[Node 1] Follower -> Candidate, term=1
[Node 1] Candidate -> ** Leader **, term=1
```

其他节点输出 `Vote GRANTED to candidate 1`。

### 客户端操作

方式 1：Python 客户端

```
python3 scripts/test_client.py
```

方式 2：手写 C++ 客户端

```
./bin/client 8001
> PUT hello world
OK
> GET hello
world
> DEL hello
OK
```

## 客户端协议

文本协议，每条命令以 `\n` 结尾。客户端端口使用 LengthHeaderCodec 封装（4 字节长度 + payload）。

请求格式：

```
PUT <key> <value>
GET <key>
DEL <key>
```

响应格式：

| 响应 | 含义 |
| --- | --- |
| OK | 写操作成功 |
|  | GET 返回的值 |
| (nil) | key 不存在 |
| NOT_LEADER | 当前节点不是 Leader，建议连接 id 节点 |
| ERROR | 请求格式错误 |
| TIMEOUT | Raft 提交超时 |

## 测试

### 单元测试

```
cd build
make test-kv-store test-raft-log test-raft-storage test-codec test-persister test-raft-node

./bin/test-kv-store
./bin/test-raft-log
./bin/test-raft-storage
./bin/test-codec
./bin/test-persister
./bin/test-raft-node
```

> 
> `test-raft-node` 使用 FakeTransport，不需要真实网络。

### 集成测试

基础功能（找 Leader + PUT/GET/DEL + 一致性）：

```
pkill -f raft-kv
rm -rf data/node*
./bin/raft-kv 0 & ./bin/raft-kv 1 & ./bin/raft-kv 2 &
sleep 4
python3 scripts/test_client.py
```

故障转移（kill Leader → 重新选举）：

```
bash scripts/test_failover.sh
```

持久化（全节点重启后数据恢复）：

```
bash scripts/test_restart.sh
```

快照（日志超过阈值触发压缩）：

```
bash scripts/test_snapshot.sh
```

## 设计要点

### 1. 分层与依赖倒置

RaftNode 通过 RaftTransport 抽象接口与网络层解耦，不直接依赖 muduo。测试时可注入 FakeTransport，生产时注入 RpcClient。

### 2. 持久化三件套

| 数据 | 文件 | 写入策略 |
| --- | --- | --- |
| 元状态 | meta.txt | 追加写，读最后一行 |
| 日志 | raft.log | 追加写，冲突时 ftruncate |
| 快照 | snapshot.bin | tmp + rename 原子替换 |

### 3. 日志覆盖优化

Follower 收到冲突日志时，通过 ftruncate 精确截断到冲突起点，再追加新日志，避免全量重写。

### 4. 快照触发

`ApplyCommittedEntries` 后检查 `log_.Size() > kSnapshotThreshold`，超过则：

1. `KVStore::Serialize` 序列化状态机
2. `RaftStorage::SaveSnapshot` 原子写盘
3. `RaftLog::SetSnapshotIndexAndTerm` 更新内存起点
4. `RaftStorage::TruncateLogUpTo` 物理截断日志

### 5. InstallSnapshot

Follower 落后太多（`nextIndex_[peer] <= snapshotIndex_`）时，Leader 直接发送整个快照，Follower 无条件采纳。

### 6. 并发模型

- muduo IO 线程：处理网络收发
- Tick 线程：每 10ms 检查选举超时 / 心跳
- 线程池：处理客户端请求（Start 会阻塞等待提交）
- 所有共享状态：由 `RaftNode::mutex_` 保护

## 故障排查

### 集群选不出 Leader

症状：日志反复出现 `StartElection` / `BecomeCandidate`。

排查：

- 检查 3 个节点是否都能互相连通（`telnet localhost 9001`）
- 检查 RpcServer 是否正常监听
- 确认时钟超时范围合理（默认 150~300ms）

### 提交超时

症状：客户端 PUT 返回 `TIMEOUT`。

排查：

- 检查 Leader 是否收到大多数 `AppendEntriesReply(success=true)`
- 检查 Follower 是否拒绝日志（日志中查 `consistency FAIL`）
- 检查 `nextIndex_` 是否卡在某个值

### 重启后崩溃

症状：节点启动立即崩溃（assertion failed）。

排查：

- 检查 `Recover` 是否从快照和日志正确恢复
- 检查 `log_.AppendEntry` 是否满足 index 连续性断言
- 清理 `data/nodeX/` 重试（仅在测试环境）

## 参考资料

- Raft 论文（Diego Ongaro, John Ousterhout）
- Raft 可视化
- muduo 网络库
- spdlog
- etcd Raft 实现

## License

本项目仅供学习使用。

## 附录：关键常量

| 常量 | 值 | 说明 |
| --- | --- | --- |
| kElectionTimeoutMinMs | 150 | 选举超时下限 |
| kElectionTimeoutMaxMs | 300 | 选举超时上限 |
| kHeartbeatIntervalMs | 50 | 心跳周期 |
| kSnapshotThreshold | 10000 | 触发快照的日志条数 |
| kMaxSnapshotSize | 1GB | 快照文件上限 |

---

*文档版本：v1.0*
*最后更新：2026-10-10*
*维护者：项目所有者*