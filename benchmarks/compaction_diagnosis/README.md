# 热点与背压诊断

这是历史实验工具；先阅读 [总说明](../README.md)。对应结果归档为 `../results/compaction-diagnosis-20261125/`，未随源码提交。

已有匹配历史快照时，可直接重建最终源码，无需执行准备/改写脚本：

```bash
cmake -S benchmarks/compaction_diagnosis -B /tmp/compaction_diagnosis-rebuild \
  -DCMAKE_BUILD_TYPE=Release -DRESULT_ROOT=/absolute/path/to/compaction-diagnosis-20261125
cmake --build /tmp/compaction_diagnosis-rebuild -j 4
```

依赖早期 Compaction 验收夹具或对照归档；仅复制本目录不足以运行完整流程。

脚本清单：

- `extra.py`
- `finalize.py`
- `inspect_results.py`
- `instrument.py`
- `peek.py`
- `positions.py`
- `prepare.py`
- `run.py`
- `summarize.py`
- `validate.py`

prepare/variants/instrument/setup/adjust 等为准备或源码改写；run/formal/stage/extra/resume 等负责采样或补跑；summarize/check/validate/finalize/collect 等负责校验、报告或归档。它们不是可以按文件名排序执行的流水线，部分分析脚本导入 run 后也可能触发工作。确切顺序以对应历史归档的命令记录为准。

执行前必须审阅顶层代码、替换硬编码路径、使用新的输出目录，并设置 `KVRAFT_RUN_HISTORICAL_BENCHMARK=1`。默认执行保护不改变历史工作负载或测量公式。
