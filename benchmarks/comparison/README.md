> 历史实验：下面描述 2026-11-16 的同步/单后台版本，不是当前双后台、分层引擎的对照入口。脚本路径和结果依赖需在新实验目录中调整，见上级 README。

# 同步与后台刷盘对比

使用同一 harness 编译历史同步版本与当前版本的隔离源码快照，不改引擎或历史基准。结果目录：`benchmarks/results/background-20261116`，包含 metadata.json、results.jsonl、results.csv、每场景 stdout/stderr、每次请求 latency.csv 和源码快照。

## 适配核对

- DBOptions、Put/Get/Delete/Flush 的签名兼容，后台版构建须链接 Threads::Threads。
- 旧程序依赖 wal.log、Put 完成时 SST 文件出现的观察方式，不适配分段 WAL 与异步发布。
- 析构不保证 active 刷盘；两版均在请求阶段后显式 Flush，分别计时请求与最终等待。
- 后台 Flush 等待目标 sequence 已发布到 SSTable/MANIFEST，不等待 WAL 清理；结果中总时间准确指请求开始到 Flush 返回，关闭/join 单独位于测量外。

## 同条件矩阵

5000 次请求、16 B key、128 B value、种子 20261115、1 前台线程、16/64 KiB MemTable、WAL off/on、每组 3 次。同步版无后台线程，新版 1 后台线程；不可为保持总线程数而关闭待测后台功能。WAL on 保留每写 Append+Sync；新版本创建分段 WAL 的文件/目录同步属于真实升级成本。

random：5000 个唯一键固定打乱顺序写入；mixed50：预填 5000 个键并 Flush，之后约 50% Get / 40% Put / 10% Delete。两种负载都保存带版本号的 128 B 值，检查覆盖和删除。每例须至少生成 3 张 SSTable，否则判失败。随机 trace 在计时前生成；请求循环没有目录扫描或额外验证，返回值存储于预分配 vector，事后按 trace oracle 校验。

请求耗时包括请求循环与采样开销，报告请求数/请求墙钟秒。总耗时为请求开始至最终 Flush 返回，另报最终 Flush 等待秒与请求数/总秒。延迟 nearest-rank 分位数覆盖所有请求，每条延迟 CSV 中 type 0=Get、1=Put、2=Delete；混合负载的写吞吐可用 puts/request_seconds、(puts+deletes)/request_seconds 计算。请求延迟最大值是观察到的最大停顿，不等于可独立归因的背压时间。

峰值读 Linux /proc/self/status VmHWM，分别在请求结束和 Flush 返回采集；包含进程初始化、mixed fixture、trace 存储与引擎，不含后续验证内存峰值。CPU 通过 getrusage 差分，含后台线程。保留磁盘逻辑大小、旧/新 WAL 文件总数和字节数。

每例 Flush 后检查所有键，然后关闭 DB、以 WAL disabled 重开检查同一状态，证明全部已持久化至 SSTable，不依赖 WAL 恢复。该验证时间不计入性能结果。每例是独立进程、新建数据库。三轮中每个参数组合的新旧版本相邻运行，固定种子选择先后次序。

## 复现

已有结果与运行目录不能复用；复测先在脚本中设置新的 out 目录，保留旧目录。将旧 source-snapshot.tar.gz 解压到新目录 snapshots/sync，当前 src/lsm 复制到 snapshots/background（本轮额外保存了当前完整 src）。对两个快照使用：

```bash
cmake -S benchmarks/comparison -B build/compare-ENGINE -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DENGINE_ROOT=/absolute/path/to/snapshot/src/lsm
cmake --build build/compare-ENGINE -j 4
python3 benchmarks/comparison/run_comparison.py
python3 benchmarks/comparison/summarize_comparison.py
```

本次 WSL 虚拟盘与编译器沿用旧环境。未清页缓存，无法约束宿主磁盘缓存或其他宿主负载；未固定引擎 random_device。新旧 SSTable 查询实现等也存在变更，所以这是升级整体前后对比，不是只替换刷盘线程的消融实验。历史源码快照可成功重建；没有覆盖当前代码或原 baseline-20261115。
