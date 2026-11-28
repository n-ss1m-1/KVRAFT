# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,hashlib,subprocess,shutil,collections,difflib
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/group-commit-acceptance-20261121-v2'
rows=[json.loads(x) for x in (o/'runs.jsonl').read_text().splitlines()]
assert len(rows)==146 and all(x['status']=='ok' and x['verified'] for x in rows)
assert all(x['reader_calls']==x['reader_sampled'] for x in rows if x['readers'])
assert subprocess.check_output(['git','diff','--binary'],cwd=r)==(o/'worktree-before.patch').read_bytes()
assert json.loads((o/'engine-hashes.json').read_text())=={str(p.relative_to(r)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((r/'src/lsm').rglob('*')) if p.is_file()}
old=r/'benchmarks/results/batch-acceptance-20261120/engine-snapshot';new=o/'engine-snapshot'
patch=[]
for p in sorted(new.rglob('*')):
 if p.is_file():
  a=old/p.relative_to(new)
  if a.exists() and a.read_bytes()!=p.read_bytes():patch+=difflib.unified_diff(a.read_text().splitlines(True),p.read_text().splitlines(True),fromfile='historical/'+str(p.relative_to(new)),tofile='current/'+str(p.relative_to(new)))
(o/'historical-source-differences.patch').write_text(''.join(patch))
with (o/'SUMMARY.md').open('a') as f:f.write("""

## 最终判定：测量证实

- 正确性验收通过：16 组测试、组提交专项 3×280 检查通过；146 个进程（4 试跑、48 主矩阵、16 主机制、4 正常关闭恢复、33 扩展、9 扩展机制、24 开销、8 追加配对）全部成功且数据验证通过。正常关闭恢复不是断电试验。不证明所有异常/并发交错均已覆盖。
- 确实共享 Sync：机制轮固定 4096 个 Put，默认 1/2/4/8 线程的数据 Sync 为 4096/2733/1655/942，平均 1.000/1.499/2.475/4.348 请求/Sync；不合并始终 4096。两配置记录数和字节数相同（4096、708608 B），排除将减少 WAL 记录误解为合并机制。表内保留实际分布。
- 三轮主矩阵 WAL-on 请求吞吐 B/A：1/2/4/8 线程约 1.32/1.55/5.05/4.41 倍。1 线程的表面收益被后续配对否定为稳定结论；4 线程倍数受环境漂移明显放大，不能当作可复用固定系数。
- 关键配置追加两轮反序配对：单线程 A/B 为 72.508/73.467 秒和 74.421/71.772 秒，方向相反；8 线程为 71.746/16.685 秒和 72.734/16.658 秒，约 4.30/4.37 倍。合并主三轮与追加轮的五轮中位数：单线程 72.200/71.772 秒（相近），8 线程 72.639/16.658 秒（4.36 倍）。8 线程 API p99 五轮中位数 54.807→14.726 ms。请求 API 包含排队；不是在线到达端到端延迟。
- WAL-off 主三轮 1 线程 B 请求吞吐约低 4.1%，8 线程约高 25.2%；但 8 线程 API p99 从 344.97 升至 847.47 µs，吞吐不等于尾延迟改善。8 线程开销轮 full/total-only 请求墙钟约 1.01，观测开销较小；单线程该比例 1.422/0.928，强烈受调度/刷盘波动影响，无法量化纯时钟开销或稳定的几个百分点成本。两者都有队列，不能据此说队列没有成本。
- 请求上限确实生效：独立机制上限 2/4/8 的最大实际组分别 2/4/8，平均 1.999/3.996/4.390。上限 8 与 32 在 8 个闭环线程下没有容量上的额外并发机会；不同采样时段的性能相差很大，无法用跨时段中位数认定 8 优于 32。
- 显式批量可叠加：16 条/API 时 A/B 含 Flush 吞吐中位数 3427/34771 op/s；64 条/API 为 33515/89328。机制分别从 1024→236 Sync、256→63 Sync，仍每请求独立 WAL 记录。绝对倍数同时受 WAL 波动影响。
- 字节预算确实限制大请求：1024 条/API，单请求 164876 B；200000 B 预算 16 请求/16 Sync/最大组1，1 MiB 预算16请求/5 Sync/最大组6。正式三轮含 Flush 吞吐 88446→252295 op/s（2.85 倍），但仅16次调用，尾分位接近最大值，且 MemTable/刷盘边界改变，不能归因于单一因素。
- 固定1读+8写扩展：A/B Get 吞吐中位数 10973→18838/s，Get p99 3078→1528 µs。所有 Get 均计入，未触及预分配容量上限。此夹具中未见合并损害 Get 的净效果；不同进程的运行时长和机会数不同，不把原始 Get 总次数直接比较成吞吐。
- 最终 Flush 与关闭均已单列。主矩阵 WAL-on Flush 中位数约 8–28 ms，关闭约 1.7–5.1 ms；WAL-off Flush 约 13–44 ms。大批量1024扩展的 Flush 占总时间比重更明显，请求吞吐408694而含Flush252295 op/s。不可省略最终 Flush 来宣称持久化总吞吐。

## 代码推断

当前 WriteGroup 持 DB 锁执行组内合法性/sequence分配、逐请求Append、单次Sync、MemTable插入与发布。组变大可能增加单次持锁工作，同时减少锁切换/Sync。API 还包含队列等待及条件变量/背压，不是纯持锁时长。本次未插入锁阶段计时，不能将 Get 净变化拆成持锁与调度各占多少。

## 证据不足与建议

- 现有数据支持多线程 WAL-on 使用已有默认不主动延时合并；支持请求/字节双上限保护，并为大显式批量留足预算。不能挑出普适最优请求上限/字节预算；不能说加大到32必然比8快。1 MiB只是本次有效配置，不是生产最优值。
- 单线程没有合并机会，未见可复现收益或稳定额外成本。WAL-off 的并发调度成本明显，当前实验不能测出引入队列相对旧实现的净成本。
- 历史显式批量源码快照可读，但与当前6个文件不同，涵盖配置/校验、估算、WAL常量迁移和执行路径。已保存逐文件完整diff。本轮未可靠隔离出“仅新增队列”历史变体，因此不强做历史因果对照，也不把历史已有吞吐与当前相除。没有修改历史快照。
- 默认8线程的机制平均4.35（分布包含许多单请求组）说明存在研究组形成机会；并不证明主动等待会获益。主动攒组可能减少Sync也可能增加低负载/在线排队延迟，需要后续独立到达率实验，本轮未实施。
- WAL波动很大（单线程某配置35–74秒），读写扩展与上限扩展也在不同时段，绝对倍数/最优参数证据不足。关键配置已反序配对；不盲目扩大所有组合。
- 校验采用关闭后直接读取Manifest/SSTable，再禁WAL重开，不在Flush返回至销毁之间插入Get，因此满足立即关闭计时；不声称测了Flush后存活DB的读取延迟。
- 机制阶段0包含后台任务以及打开/预填阶段；后台槽不受前台阶段标签切换，不能进一步把阶段0所有文件同步拆成请求期/Flush期，未伪造此类归属。

## 修改清单与复现命令补充

仅新增 benchmarks/group_commit_v2/CMakeLists.txt、bench.cc、prepare.py、run.py、summarize.py、finalize.py，以及本结果目录。原始工作树 patch 逐字一致，src/lsm 哈希逐文件一致。旧字段工具没有运行，不修改旧工具或旧结果。

在新输出目录中依次运行 prepare.py，再配置构建性能项目，然后 run.py pilot、formal、mechanism、extensions、extension-mechanism、overhead、paired，最后 summarize.py 与 finalize.py；脚本路径/结果目录需成套修改，不原地覆盖。本目录 perf-configure.txt、perf-build.txt 保存原始构建输出；build/CMakeCache.txt和compile_commands.json保留完整编译参数。精确采样命令在runs.jsonl。finalize.py生成结论与diff并核对源码不变，应在summarize.py之后执行，避免摘要被重写。

示例性能构建参数：cmake -S benchmarks/group_commit_v2 -B 新结果目录/build -DCMAKE_BUILD_TYPE=Release -DLSM_DIAGNOSTICS=OFF -DENGINE_ROOT=新结果目录/engine-snapshot；cmake --build 新结果目录/build -j 4。所有编译/测试在正式性能进程外进行；各轮串行，不并行构建或其他验收。
""")
(o/'validation.json').write_text(json.dumps(dict(counts=dict(collections.Counter(x['kind'] for x in rows)),all_success=True,engine_unchanged=True,tracked_diff_unchanged=True,reader_all_latencies_recorded=True),indent=2))
shutil.copytree(r/'benchmarks/group_commit_v2',o/'tool-snapshot',dirs_exist_ok=True)
(o/'source-hashes.json').write_text(json.dumps({str(p.relative_to(o)):hashlib.sha256(p.read_bytes()).hexdigest() for base in ['engine-snapshot','tests-snapshot','test-support-snapshot','tool-snapshot'] for p in (o/base).rglob('*') if p.is_file()},indent=2))
(o/'modifications.txt').write_text('\n'.join(str(p.relative_to(r)) for p in sorted((r/'benchmarks/group_commit_v2').iterdir()) if p.is_file())+'\nNew result directory: '+str(o)+'\nExisting tracked diff and engine hashes unchanged.\n')
print('Final validation passed: 146 runs; engine and existing tracked diff unchanged.')
