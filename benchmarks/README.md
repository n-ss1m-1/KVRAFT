# LSM 性能实验工具

这些目录记录不同开发阶段的验收与诊断，不是统一适配当前引擎的测试套件。当前正确性测试入口仍为顶层 CMake 的 LSM CTest。

## 工具索引

| 目录 | 用途与适用阶段 |
| --- | --- |
| `comparison/`、`diagnostics/` | 早期同步/后台刷盘比较与成本诊断 |
| `crc_acceptance/` | 查表 CRC 的兼容性、微基准和查询对照 |
| `block_cache_acceptance/` | 关闭、足量、小容量缓存对照 |
| `batch_acceptance/` | 显式批量、共享 Sync 与调用延迟 |
| `group_commit_acceptance/` | 早期参数探测，正式结论以 v2 为准 |
| `group_commit_v2/` | 组提交正式验收与机制轮 |
| `compaction_acceptance/` | 手动全量合并阶段验收 |
| `compaction_diagnosis/` | 热点位置、块内扫描和背压诊断 |
| `dual_background/` | 串行与双后台调度对照 |
| `write_tail_diagnosis/` | 持久化长尾与背压时间线 |
| `auto_compaction/` | 分层自动合并、查询与收敛 |
| `compaction_rewrite/` | 源预算与目标层反复重写 |
| `source_expansion/` | 源预算、扩展倍率参数矩阵 |
| `source_expansion_resume/` | 磁盘故障后续跑及分组统计 |
| `source_expansion_timeout_audit/` | 原机制轮收敛超时核对 |

每个新增实验目录的 README 列出结果依赖、构建参数和脚本。`results/` 不入 Git：历史原始数据、引擎快照、诊断变体、数据库和二进制需另外保存；仅克隆仓库不能重现全部历史结论。

## 运行边界

- 历史 Python 脚本多在顶层执行，有些会生成/改写同目录源码或报告，不应通过 import 探索接口。新增归档脚本默认停止，必须明确设置 `KVRAFT_RUN_HISTORICAL_BENCHMARK=1` 后才能运行。
- 许多 Python 路径仍绑定原实验 `/home/truth/Code/my_kvraft` 和带日期的结果目录；环境变量只解除执行保护，不负责改路径。先在独立 checkout 中核对、替换所有相关路径，选择新的结果目录和数据库目录，再运行。不能直接对已有历史结果运行采样或 finalize。
- prepare/instrument/variants/adjust/setup 等脚本是历史准备或源码变换步骤，不是可重复调用的初始化接口。当前已保存最终源码，不要为了编译再次运行这些步骤。
- CMake 的 ENGINE_ROOT 指定当时的引擎快照；RESULT_ROOT 指定包含 observer/probe/mechanism 等变体的实验根目录。旧版 Compact、tables_ 等接口不保证兼容当前引擎。
- 同名 base.cc、探针头和续跑副本保留为各实验独立证据，不合并成共享实现，以免改变历史实验的耦合关系。重复文件清单见 `archive_inventory.json`。
- `setup_timeout_audit.py` 是生成超时核对工具的历史脚本，已生成的工具在对应目录，通常直接使用最终源码。该脚本不可在现有目录重复执行。
- 源扩展续跑脚本还会检查 `/mnt/d` 空间，这是当时的环境限制，不能直接用于其他机器。
- Linux C++17、CMake、Python 标准库和 pthread 为主要依赖；系统调用 wrap/符号包装与工具链相关。固定种子不固定系统缓存、存储同步或调度。

## 检查与重建

```bash
# 只解析 Python、检查归档文件和本地 include；不会导入历史脚本或启动实验。
python3 benchmarks/check_archive.py

# 使用已有历史快照，在新构建目录重建；不要复用历史 build。
cmake -S benchmarks/batch_acceptance -B /tmp/batch-rebuild \
  -DCMAKE_BUILD_TYPE=Release -DENGINE_ROOT=/absolute/path/to/batch/engine-snapshot
cmake --build /tmp/batch-rebuild -j 4
```

完整实验需按各自结果归档的 commands、REPRODUCE 和源码哈希核对运行顺序。试跑、正式轮、机制轮和开销轮分开；正式采样期间不要并行构建或运行其他验收。禁止将诊断轮性能直接替代关闭诊断的数据。

## 历史同步基线说明（保留原口径）

以下是 2026-11-15 同步实现的使用说明。其中“没有块缓存”“没有并发调用支持”“逐条 Sync”等均描述当时引擎，不描述当前实现。

# LSM 同步刷盘性能基线

独立 C++17 benchmark，直接编译现有 LSM 源文件；不修改引擎、顶层构建、WAL 同步语义。所有 DB 调用串行。仅依赖 CMake、Linux C++ 工具链与 Python 标准库。

```bash
cd ~/Code/my_kvraft
cmake -S benchmarks -B build/lsm-bench -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/lsm-bench -j 4
python3 benchmarks/run_baseline.py \
  --output benchmarks/results/my-run \
  --operations 20000 --repeats 3 --reopens 20 \
  --key-bytes 16 --value-bytes 128 --seed 20261115
```

输出目录必须不存在；每个场景独立进程、新建数据库，保留 fixture，不复用或删除已有数据库。完整运行 28 个场景 × repeats；固定种子打乱场景执行顺序。不要同时运行其他压测。

## 测量矩阵

| 场景 | 配置及计时边界 |
| --- | --- |
| 顺序、随机、重复覆盖写 | WAL 开/关；随机写是无放回打乱；覆盖写的 key 空间为 N/10，保留全部版本；关闭阈值刷盘 |
| 查询 | 固定 N 条数据，0/1/4/16 张 SSTable；0 表表示 MemTable；有表时 MemTable 为空；交错 key 范围使老表命中经历新表筛选；首次命中、相同 trace 再读、缺失 key 首次及再读分别计时 |
| 混合 | WAL 开/关；读:Put:Delete 约为 95:4:1 或 50:40:10；先准备 4 张表；64 KiB 阈值；随机 trace，逐条检查最新值/删除状态 |
| 阈值刷盘 | WAL 开/关；16/64/256 KiB；所有 Put 和实际生成新 SSTable 的 Put 子集分别输出 |
| 手动刷盘 | WAL 开/关；每批 N 条写入后单独计时 Flush，再计时首个 Put；重复 reopens 批；装载时间不算 Flush API 延迟 |
| 恢复 | WAL 开；N/10 与 N 条 WAL；每个大小搭配 0/4/16 张 SSTable；先关闭 DB，再计时构造；关闭与 Get 校验不算 Open API 延迟；重复 reopens 次 |

恢复的有表场景先将 N 条基础记录分成指定数量 SSTable，再写 N 条 WAL 尾部；无表场景只有 N 条 WAL。恢复属于正常关闭后重新打开，未模拟进程崩溃、断电或清空系统缓存。表数量很大而 N 很小时，以结果的实际数量为准。

## 指标与口径

每个 phase 输出 JSONL 和 CSV，包含：操作数、API 总耗时、API 吞吐、整段墙钟吞吐、nearest-rank p50/p95/p99/max（微秒）、进程 CPU 时间/单核百分比、进程累计峰值 RSS（KiB）、phase 结束时数据库逻辑字节数、WAL 字节数、实际 SSTable 数量。API 时间包含引擎调用与时钟采样开销；返回值检查、随机 trace 生成、统计与序列化位于 API 时间之外。

`ops_per_second` = 操作数 / API 延迟之和；`wall_ops_per_second` = 操作数 / phase 墙钟时间。墙钟包含夹具装载（手动 Flush）、Get 校验、恢复间关闭、阈值场景的表数量观测等 harness 工作。不要把 Flush/Open 的墙钟吞吐当作纯 API 吞吐。mixed 的各类操作与 triggered-flush 子集共享整段 CPU 观察区间，CPU 不能按类型相加或解释成该类 API 独占 CPU。RSS 是整个独立进程的高水位，包含 fixture、预生成 key 和延迟样本，不等于引擎独占内存。磁盘为结束时逻辑文件大小，不是峰值、实际分配块数或宿主 VHD 大小。

开启 WAL 的每次 Put/Delete 都保留原有 Append + fsync；没有批量提交、异步 Sync 或替换系统调用。无 WAL 的未刷盘数据没有持久化保证。

`first_pass` 表示 fixture 准备后第一次 API 读取，写入和创建 Reader 已经可能使 OS 缓存变热；`warm` 重复相同命中 trace。未调用 drop_caches，未声称测得磁盘冷读。DB 没有块缓存，但仍受 Linux 页缓存、WSL 和宿主缓存影响。缺失 key 使用与已存 key 相同宽度和前缀、末尾非数字的键；不是纯表边界外的缺失查询。

DB 无并发调用支持，因此没有“Flush 正在执行时另一个 Put”的并发测量；触发阈值的 Put 自身承担 Flush 全部耗时，另报手动 Flush 和后续首个 Put。p99 对少量 Flush/Open 样本可能等于最大值，应一起查看 operations，不能视为稳定尾延迟估计。未来应增加批次数和操作数，并在相同环境重复。

## 可复现信息

metadata.json 保存操作规模、种子、执行顺序、Git HEAD 和 dirty 状态、LSM/benchmark 与 tracked 文件 SHA256、可执行文件 SHA256、CMakeCache、编译器、CPU、内存、内核、文件系统、虚拟块设备信息。当前源码存在未提交修改，因此仅凭 HEAD 无法复现，应保留同一工作树或存档源码。

首轮位置：`benchmarks/results/baseline-20261115`。该轮 N=5000，key=16 B，value=128 B，3 次重复，每种恢复/手动 Flush 20 次，seed=20261115。这是小规模开发基线，后续正式比较应沿用参数或另建同规格基线。

生成汇总：`python3 benchmarks/summarize.py benchmarks/results/my-run`。SSTable fixture 保留在结果目录的 databases/ 中，并由 benchmarks/.gitignore 排除版本控制。

随机种子固定 workload trace 和场景顺序；引擎内部跳表仍用 random_device 初始化，节点高度与内存用量可能跨进程变化，所以阈值触发次数和延迟存在随机波动。为保持引擎原状，本工具没有替换该随机源。


完整性检查：`python3 benchmarks/validate_results.py benchmarks/results/my-run`。本轮主要观察与运行干扰记录见 `results/baseline-20261115/FINDINGS.md`。

