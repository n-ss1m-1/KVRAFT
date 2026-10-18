# Raft KV 分布式存储

基于 Raft 共识算法的分布式键值存储系统，使用 C++17 实现。

## 项目简介

一个 3 节点高可用 KV 存储集群，支持：

- 自动 Leader 选举与故障转移
- 日志复制与强一致性提交
- **ReadIndex 线性一致读**（GET 也走共识，不会读到旧 Leader 的脏数据）
- WAL 持久化 + 快照压缩
- 客户端 PUT / GET / DEL 接口
- 基于 muduo 的高性能异步网络通信

目标场景：存储对一致性要求高的元数据（如服务发现、配置中心、分布式锁）。

## 功能特性

### 已实现

| 模块 | 功能 |
| --- | --- |
| 共识层 | Leader 选举、日志复制、提交规则（§5.4.2）、任期管理、冲突回退 |
| 一致性读 | ReadIndex 线性一致读（多数派心跳确认 + Leader 下台/超时安全失败） |
| 存储层 | WAL 追加写、ftruncate 冲突覆盖、快照生成、日志压缩 |
| 网络层 | 基于 muduo 的异步 RPC（RequestVote / AppendEntries / InstallSnapshot） |
| 客户端 | 长度头 + 文本协议 PUT/GET/DEL，Leader 重定向 |
| 持久化 | 元状态 + 日志 + 快照独立文件，崩溃恢复 |
| 并发 | 线程池处理客户端请求，读写锁保护状态机 |
| 可观测 | spdlog 分级日志（终端 + 轮转文件双 sink） |

### 待实现

- Leader 当选后立即写 no-op 日志（加快新 Leader 提交、刷新重启状态机）
- ReadIndex 之外的 Lease Read（跳过一轮心跳往返）
- 批量提交（group commit）优化
- 成员变更（动态添加/删除节点）
- 自研 epoll 网络层（替换 muduo）

## 系统架构

### 整体分层

```
┌────────────────────────────────────────────┐
│                   客户端                    │
└──────────────────┬─────────────────────────┘
                   │ TCP（4 字节长度头 + 文本命令）
                   ▼
┌────────────────────────────────────────────┐
│  ClientServer (客户端端口 8001/8002/8003)   │
│  - LengthHeaderCodec 解包                   │
│  - 线程池处理命令                            │
│  - PUT/DEL → RaftNode::Start()             │
│  - GET     → RaftNode::ReadIndex() 共识读   │
└──────────────────┬─────────────────────────┘
                   │
                   ▼
┌────────────────────────────────────────────┐
│  RaftNode (Raft 核心)                       │
│  - 状态：Follower / Candidate / Leader      │
│  - 选举、日志复制、提交、快照、ReadIndex     │
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
└── snapshot.bin ← 快照（24 字节头：lastIncludedIndex/lastIncludedTerm/dataLen 各 int64，后接数据）
```

日志输出在 `logs/nodeX.log`（10 MB 轮转、保留 3 份）。

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
│   │   ├── config.h               # 集群拓扑、时间参数、快照阈值
│   │   ├── types.h                # Role 枚举等
│   │   ├── command.h / .cc        # Command 结构与解析
│   │   └── thread_pool.h / .cc    # 线程池
│   │
│   ├── kv/                        # 状态机
│   │   └── kv_store.h / .cc       # 内存哈希表 + 读写锁 + 快照序列化
│   │
│   ├── persist/                   # 底层文件 IO
│   │   └── persister.h / .cc      # open/write/fsync/ftruncate/TruncateFront
│   │
│   ├── raft/                      # Raft 核心
│   │   ├── raft_message.h / .cc   # LogEntry / Meta / Snapshot 序列化
│   │   ├── raft_log.h / .cc       # 内存日志（index/term、快照起点）
│   │   ├── raft_node.h / .cc      # Raft 节点（选举/复制/提交/快照/ReadIndex）
│   │   ├── raft_storage.h / .cc   # 持久化编排（组合三个 Persister）
│   │   └── raft_transport.h       # RPC 传输抽象接口
│   │
│   ├── rpc/                       # 网络层
│   │   ├── rpc_message.h / .cc    # 6 类 RPC 消息序列化
│   │   ├── codec.h                # LengthHeaderCodec（粘包处理）
│   │   ├── rpc_client.h / .cc     # 实现 RaftTransport（主动连接 peer）
│   │   └── rpc_server.h / .cc     # 接收 Raft RPC
│   │
│   └── client_server/             # 客户端接口
│       └── client_server.h / .cc
│
├── tests/                         # 单元测试（9 个套件 + 公共头）
│   ├── test_util.h                # CHECK 宏 / RUN_TEST / 临时目录
│   ├── test_command.cc
│   ├── test_thread_pool.cc
│   ├── test_kv_store.cc
│   ├── test_persister.cc
│   ├── test_raft_message.cc
│   ├── test_rpc_message.cc
│   ├── test_raft_log.cc
│   ├── test_raft_storage.cc
│   └── test_raft_node.cc          # FakeTransport 驱动核心场景
│
├── scripts/
│   ├── start_cluster.sh           # 一键启动 3 节点
│   ├── test_client.py             # 基础功能 + 一致性测试（Python 客户端）
│   ├── test_failover.sh           # 故障转移测试
│   ├── test_restart.sh            # 持久化测试
│   ├── test_snapshot.sh           # 快照测试
│   └── check_log.sh               # 日志检查
│
├── docs/                          # 设计笔记
│
├── third_party/
│   ├── muduo/                     # muduo 源码（需先编译出 build/lib）
│   └── spdlog/                    # spdlog v1.17（CMake 自动引入，无需安装）
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
| 易失 | leaderId_ | 当前 Leader ID |
| Leader 专有 | nextIndex_[] | 每个 Follower 下一条要发的 index |
| Leader 专有 | matchIndex_[] | 每个 Follower 已确认的 index |
| ReadIndex | pendingReads_ | 挂起的读请求队列 |
| ReadIndex | peerHeartbeatAckTimes_[] | 各 peer 最近心跳确认时间 |

核心方法：

- `Start(cmd)`：客户端提交写命令（仅 Leader），阻塞等待多数派提交
- `Tick()`：定时器驱动，检查选举超时 / 心跳周期
- `StartElection()` / `SendHeartbeat()`：发起选举 / 广播心跳（锁外发 RPC）
- `HandleRequestVote` / `HandleAppendEntries` / `HandleInstallSnapshot`：处理节点间 RPC
- `HandleXXXReply`：处理 RPC 回复
- `ReadIndex(cb)`：线性一致读，确认自身仍是合法 Leader 后回调
- `TakeSnapshot()`：日志超过阈值时触发快照
- `Recover()`：启动时从磁盘恢复

> 约束：RaftNode 继承 `enable_shared_from_this`，RPC 回调中持有 weak_ptr，因此只能用 `std::make_shared` 构造。

### RaftLog（内存日志）

核心不变式：`entries_[i].index == snapshotIndex_ + i + 1`

关键接口：

- `AppendFrom(startIndex, entries)`：追加（空 entries 也触发截断语义）
- `GetEntry(index)` / `GetTerm(index)`：按 index 查询
- `SetSnapshotIndexAndTerm(index, term)`：快照后更新逻辑起点
- `CheckConsistency(prevIndex, prevTerm)`：Follower 一致性检查，返回 conflictIndex（`-1` 表示需要快照）

### RaftStorage（持久化）

组合三个 Persister：

- `metaPersister_`：存 currentTerm / votedFor
- `logPersister_`：存日志条目
- `snapPersister_`：存快照（`needCreate=false`，按需创建）

维护状态：

- `indexToOffset_`：index → 文件字节偏移（用于 ftruncate）
- `lastPersistedIndex_`：已持久化的最大 index

核心接口：

- `AppendLogEntry(entry)`：顺序追加（强制 index 连续）
- `AppendLogEntriesFrom(start, entries)`：覆盖追加（先 ftruncate 再写）
- `LoadLogEntries(snapshotIndex)`：加载日志，必须传入快照点，否则会被当成 gap
- `SaveSnapshot` / `LoadSnapshot` / `HasSnapshot`：快照读写
- `TruncateLogUpTo(index)`：快照后删除旧日志

### KVStore（状态机）

纯内存哈希表 + shared_mutex：

- `Apply(cmd)`：日志重放的统一入口，内部按 PUT/DEL 分发（只改内存）
- `ApplyPut` / `ApplyDelete`：具体的状态机变更
- `Get`：读操作（返回 std::optional），不走共识
- `Serialize` / `Deserialize`：快照序列化，key 与 value 各占一行（`key\nvalue\n` 成对），反序列化是整体替换

### RaftTransport（抽象接口）

```
class RaftTransport {
    virtual void SendRequestVote(int32_t peerId, const RequestVoteArgs&, RequestVoteCallback) = 0;
    virtual void SendAppendEntries(int32_t peerId, const AppendEntriesArgs&, AppendEntriesCallback) = 0;
    virtual void SendInstallSnapshot(int32_t peerId, const InstallSnapshotArgs&, InstallSnapshotCallback) = 0;
};
```

三个接口均为**异步回调**：发送方把回调交给传输层，收到回复后再回调 `HandleXXXReply`。

- 生产环境：RpcClient 实现（基于 muduo）
- 测试环境：FakeTransport 实现（同进程同步直连对端 HandleXXX，可注入分区/丢回执）

## 快速开始

### 环境要求

- Linux（推荐 Ubuntu 20.04+），本项目在 WSL Ubuntu 上开发验证
- g++ 9+（支持 C++17，当前用 g++ 15 + CMake 4 验证）
- CMake 3.16+
- muduo 与 spdlog 源码均已放在 `third_party/`，无需系统安装

### 编译 muduo 依赖（仅首次）

CMake 从 `third_party/muduo` 引头文件、从 `third_party/muduo/build/lib` 链接静态库，因此需要先在本地编译一次（不需要 `make install`）：

```
cd third_party/muduo
mkdir -p build && cd build
cmake ..
make -j4
```

spdlog 已在 `third_party/spdlog`（v1.17.0），由主工程 `add_subdirectory` 自动引入，无需额外操作。

### 编译工程

```
mkdir -p build && cd build
cmake ..
make -j4
```

主程序与测试可执行文件都输出到项目根 `bin/` 目录。

### 启动 3 节点集群

方式 1：一键脚本

```
bash scripts/start_cluster.sh
```

方式 2：打开 3 个终端手动启动（间隔 < 几秒，启动时有 3 秒静默等待）

```
# 终端 1
./bin/raft-kv 0
# 终端 2
./bin/raft-kv 1
# 终端 3
./bin/raft-kv 2
```

启动后其中一个节点会输出：

```
[Node 1] Follower -> Candidate, term=1
[Node 1] Candidate -> ** Leader **, term=1
```

其他节点输出 `Vote GRANTED to candidate 1`。

### 客户端操作

运行 Python 客户端（自动找 Leader、PUT/GET/DEL、覆盖写、批量写并校验三节点一致性）：

```
python3 scripts/test_client.py
```

协议为 4 字节大端长度头 + payload（见下文），用普通 `telnet` 无法直接交互，需用带长度头的客户端，`scripts/test_client.py` 即是可直接参考的样例。

## 客户端协议

每条消息格式为：**4 字节大端长度（int32）+ payload**，由 `LengthHeaderCodec` 处理粘包。

请求 payload（文本，空格分隔）：

```
PUT <key> <value>
GET <key>
DEL <key>
```

> key 与 value 不能含空格（文本协议限制）。

响应：

| 响应 | 含义 |
| --- | --- |
| `OK` | PUT/DEL 已提交并应用 |
| `Timeout` | 写命令未在 500ms 内达成多数派 |
| `<value>` | GET 命中，直接返回值 |
| `nil` | GET 的 key 不存在 |
| `NOT_LEADER <id>` | 当前节点不是 Leader，`id` 为已知 Leader（-1 表示未知） |
| `NOT_LEADER <id> or TIMEOUT` | ReadIndex 读失败（已下台或确认超时） |
| `ERROR invalid command` | 请求无法解析 |
| `ERROR unknown command type` | 命令类型非法 |

## 测试

### 单元测试（9 个套件，共 159 个用例）

单元测试不依赖 muduo，RaftNode 全部用 FakeTransport 同进程同步驱动，确定性、无睡眠（个别超时用例除外）。

一键编译并运行：

```
cd build
make -j4
ctest --output-on-failure
```

也可以单独运行某个套件（可执行文件在 `bin/`，需在项目根目录运行，临时数据写在 `test_data/`）：

| 测试 target | 覆盖模块 | 用例数 |
| --- | --- | --- |
| test-command | 命令解析（含 client_server 命令分支） | 16 |
| test-thread-pool | 线程池执行/异常隔离/停止 | 11 |
| test-kv-store | 状态机 CRUD、序列化、并发 | 17 |
| test-raft-message | LogEntry/Meta/Snapshot 序列化 | 15 |
| test-rpc-message | 6 类 RPC 消息序列化 | 13 |
| test-persister | 文件追加/覆盖/截断/并发 | 14 |
| test-raft-log | 内存日志、快照、一致性检查 | 25 |
| test-raft-storage | 持久化编排、重启恢复 | 24 |
| test-raft-node | 选举/复制/提交/快照/ReadIndex/单节点 | 24 |

> `test-raft-node` 的核心场景：唯一 Leader 选举、重复投票、高 term 降级、冲突回退覆盖、§5.4.2 旧 term 不直接提交、重启恢复、InstallSnapshot 追平、ReadIndex 成功/失败/下台/超时。

### 集成测试（真实网络）

基础功能（找 Leader + PUT/GET/DEL + 三节点一致性）：

```
bash scripts/start_cluster.sh
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

> 网络层（rpc_client / rpc_server / client_server / codec）不单独写 C++ 单测，由上述脚本做端到端兜底。

## 设计要点

### 1. 分层与依赖倒置

RaftNode 通过 RaftTransport 抽象接口与网络层解耦，不直接依赖 muduo。测试时可注入 FakeTransport，生产时注入 RpcClient。

### 2. 线性一致读（ReadIndex）

GET 不做本地读（本地读可能在 Leader 已被替换时返回脏数据），而是走论文 §6.4 的 ReadIndex：

1. Leader 记录 `readIndex = commitIndex`、当前 term、`bornTime`，挂入 `pendingReads_`；
2. 立即广播一轮心跳，收到多数派心跳确认，证明自己仍是合法 Leader；
3. 当 `bornTime <= 多数派确认时间` 且 `readIndex <= lastApplied_` 时，回调执行本地读；
4. 若期间收到更高 term 而降级（`AbortPendingReads`），或挂起超过 500ms（Tick 清理），回调失败，客户端可重定向到新 Leader。

非 Leader 节点直接同步返回 `NOT_LEADER`。

### 3. 持久化三件套

| 数据 | 文件 | 写入策略 |
| --- | --- | --- |
| 元状态 | meta.txt | 追加写，读最后一行 |
| 日志 | raft.log | 追加写，冲突时 ftruncate / TruncateFront |
| 快照 | snapshot.bin | tmp + rename 原子替换 |

### 4. 提交规则（§5.4.2）

Leader 只在**当前任期**的日志达到多数派时才推进 commitIndex；旧任期的日志即使已复制到多数节点也不能直接提交，而要等当前任期日志提交时被"顺带"提交，避免已被新 Leader 覆盖的日志被错误确认。

### 5. 日志冲突与覆盖

Follower 一致性检查失败时返回 `conflictIndex`（本任期段起点），Leader 据此回退 `nextIndex_` 重发；空 entries 心跳也会触发截断。回退下限是 `snapshotIndex + 1`，低于此点直接改发 InstallSnapshot，避免死循环。

### 6. 快照

`ApplyCommittedEntries` 末尾检查 `log_.Size() > kSnapshotThreshold`（当前为 20），超过则：

1. `KVStore::Serialize` 序列化状态机；
2. `RaftStorage::SaveSnapshot` 原子写盘（先写 tmp 再 rename）；
3. `RaftLog::SetSnapshotIndexAndTerm` 更新内存起点；
4. `RaftStorage::TruncateLogUpTo` 物理截断日志。

Follower 落后太多（`nextIndex <= snapshotIndex`）时，Leader 发送整个快照，Follower 校验后安装；旧快照（index 更小）直接忽略。

### 7. 并发模型

- muduo IO 线程：ClientServer 设 2 个 IO 线程处理客户端收发；RpcServer 处理 Raft 收发
- Tick 线程：启动先静默 3 秒（避免 peer 未连接时 term 疯涨），之后每 10ms 调一次 `Tick`
- 客户端线程池：4 个 worker 处理命令（Start 会阻塞等提交，避免占住 IO 线程）
- 共享状态：由 `RaftNode::mutex_` 保护；RPC 一律在锁外发送，回复在锁外 flush

## 故障排查

### 集群选不出 Leader

症状：日志反复出现 `StartElection` / `BecomeCandidate`。

排查：

- 检查 3 个节点是否都能互相连通（`telnet 127.0.0.1 9001`）
- 检查 RpcServer 是否正常监听、RpcClient 是否在 3 秒静默期后连上 peer
- 确认时钟超时范围合理（默认 150~300ms）

### 提交超时

症状：客户端 PUT 返回 `Timeout`。

排查：

- 检查 Leader 是否收到大多数 `AppendEntriesReply(success=true)`
- 检查 Follower 是否拒绝日志（日志中查 `consistency FAIL` / conflict）
- 检查 `nextIndex_` 是否卡在某个值

### 重启后读不到数据

重启后未持久化 commitIndex、状态机暂时停在快照点，需要等 Leader 心跳或一条新写请求触发应用后才追平（见 `raft_node.h` 末尾 TODO）。这是已知限制，规划用 no-op 日志解决。

### 清理状态重来

```
pkill -f raft-kv
rm -rf data/node*
```

## 参考资料

- Raft 论文（Diego Ongaro, John Ousterhout）
- Raft 可视化
- muduo 网络库
- spdlog
- etcd Raft 实现

## 附录：关键常量

| 常量 | 值 | 说明 |
| --- | --- | --- |
| kHeartbeatIntervalMs | 50 | 心跳周期 |
| kElectionTimeoutMinMs | 150 | 选举超时下限 |
| kElectionTimeoutMaxMs | 300 | 选举超时上限 |
| kSnapshotThreshold | 20 | 触发快照的日志条数（内存日志超过此值即压缩） |
| kNeedSnapshot | -1 | conflictIndex 特殊值，表示改发 InstallSnapshot |
| kMaxSnapshotSize | 1GB | 快照数据上限（反序列化校验） |
| Tick 周期 | 10ms | 定时器线程驱动间隔 |
| Start 等待 | 500ms | 写命令等待多数派提交的超时 |
| ReadIndex 等待 | 500ms | 挂起读的确认超时（Tick 清理） |

## License

本项目仅供学习使用。

---

*文档版本：v1.1*
*最后更新：2026-10-15*
