# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from paths import *
# Keep overhead tasks in raw CSV, exclude them from comparative mechanism aggregates.
p=T/'summarize.py';s=p.read_text().replace("for x in rows:mg[x['scale'],x['budget'],x['factor'],x['work']].append(x)","for x in rows:\n if not x['run'].startswith('overhead'):mg[x['scale'],x['budget'],x['factor'],x['work']].append(x)");p.write_text(s)
p=T/'finalize.py';s=p.read_text().replace("'all_verified':True,'failed':0,'timeout':0","'all_valid_runs_verified':True,'failed':1,'timeout':1,'invalid_records_directory':'runs/invalid','historical_invalid_manifest':'resume-state.json'");p.write_text(s)
(O/'REPRODUCE.md').write_text((O/'REPRODUCE.md').read_text()+"""

## 2026-11-28 环境恢复与续跑
旧目录 source-expansion-20261127 保留未改；本目录 source-expansion-resume-20261128 复制完整记录，三份空JSON和一个无完整结果轮未计入完成。缺失轮用本目录独立数据库补跑。恢复前主引擎哈希、git diff --binary均一致；根文件系统rw，D盘初始13.3GiB，随后用户清理至约71GiB。每次采样前检查/mnt/d空闲，低于5GiB停止，不清理历史数据。

续跑工具 benchmarks/source_expansion_resume/。命令记录本目录commands.jsonl只列新增过程；历史命令仍在旧目录。原始构建及18组测试沿用相同已验证引擎快照和可执行文件（build/original/observer/mechanism为指向历史目录的链接）；counter-free独立重建在本目录counter-project/counter-build。不存在并行采样与构建。

执行过程：setup_resume.py建立断点工具 → all.py（extras中止）→ setup_finish.py标记无效 → finish.py完成余项。初次finish_sampling.py有Python语法错误，未启动采样，修正后执行。复现不要直接覆盖这些目录；paths.py改为新目录，使用tool-snapshot，先准备独立快照/构建，再按resume.py、finish_sampling.py配置启动。run.py会跳过已有verified结果，失败轮保留，不自动重试。

无效轮 matrixm-262144-2-unique 为详细机制的120秒收敛等待超时（进程130.28秒，返回2），不是全量校验成功。原始stdout/stderr/json保存在runs/invalid，数据库保留。未增加超时、未重跑、未作为机制证据。原历史损坏见resume-state.json，保留在原位置。

RECOVERY-STRATA.md按故障前后分层；formal-rounds.csv为完整逐轮性能；raw-integrity.json核对所有有效记录的延迟条数。详细机制明显干扰时序，跨轮任务数不保证固定。机制汇总排除overhead专用对照，后者只见LEVELS.md。counter-*为轻边界计数对照，不能将一次配对差值解释为精确统计成本。

缓存查询使用原夹具只读数据文件，但DB打开会产生其正常元数据访问；未复制每轮完整夹具。所有query均禁用自动合并及WAL，逻辑SST布局不变。先OS预热、独立空缓存、first再steady；4096查询不足代表长时间在线缓存命中率。
""")
print('updated metadata')