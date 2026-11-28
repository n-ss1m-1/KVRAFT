# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,subprocess,hashlib,shutil
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/batch-acceptance-20261120'
rows=[json.loads(s) for s in (o/'runs.jsonl').read_text().splitlines()]
assert subprocess.check_output(['git','diff','--binary'],cwd=r)==(o/'worktree-before.patch').read_bytes()
assert len([x for x in rows if x['kind']=='formal'])==42
assert all(x['status']=='ok' for x in rows)
assert len([x for x in rows if x.get('recovery_verified')])==7
checks=[x for x in rows if x['name'].startswith('mechanism')]
assert len(checks)==14 and all(x['live_verified'] and x['reopen_verified'] for x in checks)
text="""

## 结论

- 正确性通过：15/15 LSM 测试，批次专项 1154 检查通过。42 个正式轮均全量禁 WAL 重开校验；14 个独立机制配置另做 Flush 后存活 DB 校验，7 个独立不 Flush 的正常析构/WAL 重开恢复通过。不是断电测试。
- WAL-on：含 Flush 吞吐 Put 为 679 op/s，批量 4/16/64/256/1024 为 2613/9636/34468/98117/205397。批量扩大 4 倍，后续吞吐增幅约 3.7/3.6/2.8/2.1 倍，256 后边际收益减弱，1024 仍有明显收益，未证明平台期。
- 数据 WAL Sync 精确对应提交次数，从 16384 减至 16；新 WAL Sync 另列。WAL 字节从 2834432 降至 2638016，约少 6.9%，字节减少远小于吞吐变化。没有改变同步策略。
- WAL-off 不单调：total-only 中 Put 含 Flush 335098 op/s，256/1024 为 368291/370625，约 +9.9%/+10.6%，4–64 没有收益。大批次 SST 数由 5 变 4，组批复制、校验、锁调用、插入及刷盘边界均参与，不能将全部差异归因于少加锁。
- API 是整次提交：WAL-on p50 从 Put 1.446 ms 到批量 1024 的 3.450 ms；1024 p99/max 中位数 7.750 ms，每轮仅 16 次提交，尾分位支持有限。WAL-off 1024 p50 为 0.503 ms，也有约 10 ms 长调用。摊销成本不是单条请求延迟。代码证实整批在 DB 锁内处理，但 API 包含背压和锁等待，不是纯持锁计时。
- WAL-on Flush 中位数约 8–14 ms，WAL-off 16–20 ms；Flush 未随批量吞吐同比缩短。关闭通常约 0.4–1.8 ms，含 join/可能剩余 WAL 清理。进程峰值约 14–16 MiB，含基准自身。
- 波动：Put 三轮提交 22.733–24.155 s，批量 1 为 24.021–52.044 s；额外反序配对 Put 23.615/23.101 s，批量 1 为 26.645/24.315 s。五轮提交中位数 Put 23.615 s、批量 1 26.645 s。相同 Sync/字节，慢轮长尾明显，证据不足以把差异确定归因于接口/harness，全部慢轮保留。
- 开销轮 full 提交中位数比 total-only 高约 12%–14%（Put、1、16），同时存在调度/持久化波动，不能视作纯计时开销占比。已补全 WAL-off 三轮 total-only，吞吐优先参考该表；亚微秒延迟受扰动。不混用机制轮吞吐。
- 离线导入可优先试 256–1024；若重视单次批量处理时长，可从 16–64 起做真实读写混合验证。不能据此决定在线最优 batch、声称改善在线 p99 或推断读查询延迟；未含攒批等待、并发/读线程。本轮未追加可选混合负载。

## 修改、异常与复现补充

本次仅新增 benchmarks/batch_acceptance/ 与本结果目录。工具入口为 CMakeLists.txt、bench.cc、prepare.py、run.py、summarize.py、finalize.py；其余 adjust.py、fix_mechanism.py、adjust_mixed.py、add_paired.py、add_totalonly.py、extend_summary.py 是工具修订过程记录，复现不要执行它们。未修改引擎、原有测试、笔记或历史结果，未提交/推送；git diff --binary 与开始快照逐字相同，engine hash 相同。

使用新结果目录修改脚本 o 路径后，依次执行 prepare.py，run.py pilot/formal/extras/paired/totalonly，summarize.py。prepare 保存同版本源码快照并编译运行 15 个测试，拒绝覆盖结果目录；run 拒绝覆盖同名结果。每轮精确命令在 runs.jsonl。每轮 stdout/stderr、JSON、延迟 CSV 和总 results.csv 均保留。

首版 pilot-1-0 实际数据校验成功，但机制断言失败，因链接包装未拦截同编译单元内部 WriteRecord；修正为系统调用包装后 pilot-v2 和全部机制断言通过。原记录保留，不作为有效机制结果。正式轮无失败/超时；不声称测试异常性能。source-hashes.json 保存引擎/工具/测试快照 SHA256；配置、环境、HEAD、原未提交 patch 均在本目录。
"""
(o/'SUMMARY.md').write_text((o/'SUMMARY.md').read_text()+text)
(o/'validation.json').write_text(json.dumps(dict(formal_rounds=42,mechanism_configs=14,recovery_checks=7,total_only_rounds=21,overhead_rounds=24,extra_paired_rounds=4,engine_hash_unchanged=True,tracked_diff_unchanged=True),indent=2))
shutil.copytree(r/'benchmarks/batch_acceptance',o/'tool-snapshot',dirs_exist_ok=True)
(o/'source-hashes.json').write_text(json.dumps({str(p.relative_to(o)):hashlib.sha256(p.read_bytes()).hexdigest() for base in ['engine-snapshot','tool-snapshot','tests-snapshot','test-support-snapshot'] for p in (o/base).rglob('*') if p.is_file()},indent=2))
print('verified',len(rows),'records')
