# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,subprocess,hashlib,shutil,csv,collections,statistics
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/dual-background-20261125';T=R/'benchmarks/dual_background';med=statistics.median
# Percentages and total throughputs supplement all original raw per-run values.
vs=json.loads((O/'per-run.json').read_text())
with (O/'threshold-distribution.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['run','batches_or_gets','over10_count','over10_pct','over50_count','over50_pct','over100_count','over100_pct','total_op_s'])
 for v in vs:
  n=1024 if v['scenario']=='write' else 65536;w.writerow([v['run'],n,v['over10'],100*v['over10']/n,v['over50'],100*v['over50']/n,v['over100'],100*v['over100']/n,v['total_op_s']])
text='''
## 结论：测量证实

- 正式写入/控制各8轮，单独合并/均匀查询各5轮，隔离机制各3轮；111进程全部正确性通过。主树引擎和原tracked差异不变。
- WAL off合并组：历史串行79321→双线程190603逻辑op/s（2.40倍）；每批64条的API max中位491.268→22.284ms。包含Compact等待、最终Flush和关闭总时间957.725→517.828ms。当前发布逻辑+串行控制81241op/s、max473.545ms、总991.058ms，支持后台拆分对本负载的重要性；控制也受任务领取时序影响，不能将单个中位差值当作纯线程数因果量。
- 稳定不等于消灭长尾：双线程WAL-off仍有329.904ms、1063.022ms慢轮；其余轮约20–24ms。串行也有写先完成、Compact后做的轮次。逐轮max、超10/50/100ms数量/比例、相交请求数完整保留。公共窗口相交仅说明前台与调用同时活动，不是精确构建区间。
- 无合并WAL-off写吞吐180524→193786op/s，max中位21.835→20.318ms，未见明显基础回退。无合并WAL-on波动严重，串行和当前最大延迟中位约472/315ms；合并组16903→19502op/s（1.15倍）、max473→29ms，但不将同步场景差值全归合并调度。未采集CPU利用率/I/O队列指标。
- 无写入时合并451.877→453.315ms，总460.811→461.295ms，峰值31.72→31.73MiB，变化很小。写入并发时峰值77.58→78.85MiB，约+1.27MiB（WAL-off中位），含批次/oracle/程序，不是Builder精确增量。
- 查询无合并31685→31745op/s几乎相同；调用合并时36226→34899op/s（约-3.7%），p99约82→88µs，max中位5.32→5.89ms。全程混合合并前后阶段，不能解读为精确构建期间损失；单读线程、缓存关闭、均匀trace，不能外推缓存热点或多读线程。
- 三轮机制中，双线程均有16次Flush在合并构建区间完成发布，串行为0；总Flush任务17、前台自动Rotate16（最终Flush另一次），每组仅1请求。背压均15次：串行累计679–753ms、最大447–489ms；双线程累计235–252ms、最大20–25ms。事件槽按线程永久独占、退出后保留，Flush phase5与Compact phase6分开，无overflow/dropped。
- Compact发布锁等待均约0–0.001ms，持锁约4.6–5.6ms；不是主要大停顿证据。双线程机制构建473–496ms，串行427–468ms，有更长现象，但不凭此断言I/O或CPU竞争。Writer含同步、rename及目录同步；父子阶段嵌套，前后台时间重叠，不能求和解释墙钟。

## 代码推断 / 证据不足

历史与当前差异包括两后台、shared_ptr输入生命周期、发布时保留新刷盘表、候选故障检查和线程启动失败处理；准确差异serial-vs-current.patch。隔离current-serial只保留当前逻辑、使用历史单循环immutable优先调度；仅改副本线程启动和调度函数，API/持久化顺序不变，补丁current-serial-vs-current.patch（方向是控制→当前）。它是成功路径性能控制，未单独重新证明所有启动故障语义，与生产当前版不同。

双线程Flush可在Compaction构建期间释放immutable，由时间线直接证实；资源竞争仅为可能解释。异常长尾、WAL同步波动未作资源采样或受控延迟实验，原因证据不足。后续建议优先跟踪异常慢轮资源与发布阶段；流式Writer仍值得在更大输出下研究，但本次只看到约1.27MiB峰值增量，不能证明必须立即实现或已控制规模内存。未实施任何优化。

## 机制开销与边界

正式LSM_DIAGNOSTICS与独立PROBE均关闭；机制保留LSM_DIAGNOSTICS=OFF、仅PROBE开启，不能混用吞吐。机制三轮构建、背压见原始JSON与事件；同工作量下额外计时改变时序，因此不机械减固定开销。API总、QueueLock/CV、首次DB锁、每组Add、Rotate和后台完整任务事件全部保存；单条Add不读时钟。条件等待含唤醒后重新加锁。后台task入口是领取之后、领取前队列/锁未精确分离，不能声称测到纯执行时间。publish_hold包含Save，cleanup在返回通知后发生。Flush发布完成区间以释放锁前为边界，immutable已在其中释放。

准备/复制/预热/校验不计时；写请求wall为worker完整区间，total从启动worker到Compact与worker均结束、显式Flush、立即关闭join，可能包含写结束后继续合并的时间；API批次分位数不是单条逻辑操作延迟。预构造batch不含在线攒批等待。所有前台结束后Flush再销毁；校验在关闭后，禁用WAL重开全量Get，再按MANIFEST读取所有表核对最新物理sequence/type/value；Compact-only还检查输出唯一和FlushedSequence不变。每轮正式物理文件集合须与MANIFEST一致，原64张输入在合并后清理；新增Flush表无需全被这轮Compact合并。不是断电测试。

实际Flush次数由MANIFEST generation增量减本轮Compact发布数推得，publication-counts.json保存；output新表数由meta.minSequence界定，可能部分新写记录被本轮合并，不能把new_only_tables当作总刷盘次数。输入快照、输出表、oracle、时间线都保留。不清系统缓存；各侧独立无缓存Reader遍历预热再开DB，缓存容量0，不称冷盘。结论限本次WSL ext4固定负载。
'''
with (O/'SUMMARY.md').open('a') as f:f.write(text)
# overhead comparison, preserve temporal rather than matched causal interpretation
with (O/'mechanism-overhead.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['version','plain_8_median_op_s','probe_3_median_op_s','ratio_probe_plain'])
 for version in ['serial','engine-snapshot']:
  a=[v['op_s'] for v in vs if v['run'].startswith('r') and v['scenario']=='write' and v['wal']==0 and v['comp']==1 and v['version']==version];b=[v['op_s'] for v in vs if v['version']==version+'-probe'];w.writerow([version,med(a),med(b),med(b)/med(a)])
(O/'REPRODUCE.md').write_text('''# 复现

仅新增benchmarks/dual_background/和benchmarks/results/dual-background-20261125/。原有代码/历史文件不改。

prepare.py源于原验收工具，结果目录拒绝覆盖。重跑前将所有脚本中的dual-background-20261125替换为新的唯一名称。版本：serial=原验收engine-snapshot；engine-snapshot=当前；current-serial=当前发布逻辑+历史单调度控制；*-probe=独立诊断。源码与两测试快照完整保存。

```sh
cd /home/truth/Code/my_kvraft
python3 benchmarks/dual_background/prepare.py
python3 benchmarks/dual_background/variants.py
cmake -S benchmarks/results/NEW/historical-test-project -B benchmarks/results/NEW/historical-test-build -DCMAKE_BUILD_TYPE=Release -DLSM_DIAGNOSTICS=OFF
cmake --build benchmarks/results/NEW/historical-test-build -j4
ctest --test-dir benchmarks/results/NEW/historical-test-build -E lsm-db-compaction -V
cmake -S benchmarks/dual_background -B benchmarks/results/NEW/build -DCMAKE_BUILD_TYPE=Release -DLSM_DIAGNOSTICS=OFF -DRESULT_ROOT=/home/truth/Code/my_kvraft/benchmarks/results/NEW
cmake --build benchmarks/results/NEW/build -j4
python3 benchmarks/dual_background/run.py
python3 benchmarks/dual_background/extra.py
python3 benchmarks/dual_background/summarize.py
python3 benchmarks/dual_background/counts.py
python3 benchmarks/dual_background/finalize.py
```

构建均GCC11.4 Release -O3 -DNDEBUG -std=c++17无LTO；配置/精确编译命令在build/CMakeCache.txt、compile_commands.json。当前测试18组日志tests.txt；历史适用17组history-tests.txt；旧串行DB调度专项跳过。正式采样顺序在commands.jsonl，stdout/stderr与JSON保留；超时180s失败即停止，未发生失败或超时。

统一夹具multi-64：65536逻辑键四版本、64表，16B key、128B value、seed20261115，最新13108删除键。初始全部已Flush且无待刷盘内存。主场景单写线程1024批×64操作，新key独立；1MiB阈值、4MiB输出软目标、组请求32/字节1MiB、缓存0，WAL按场景。第4个完成批次后Compact；读单线程65536均匀请求，第512次后Compact，无最小键热点偏置。无Comp组不人为sleep。预生成trace/oracle和批次不计时；复制/预热不计时。

逐请求CSV在runs/*-timeline.csv，相对worker起点；phase按请求开始分为前/窗口/后，跨边界计数单列timeline-stages.csv；窗口为公共API排队/构建/发布，不等于后台执行。mechanism事件在*.probe.json，steady_clock绝对ns，可在同进程对齐。固定8线程槽和8192事件/槽，无全局统计锁；输出在join后，dropped=0；LSM_DIAGNOSTICS=OFF避免旧共享非原子TaskTrace。Flush与Compact永久不同slot及phase，无线程共享槽。

原始逐轮与中位见SUMMARY.md、per-run.csv；阈值次数及比例threshold-distribution.csv，包含Flush/关闭的吞吐total_op_s在per-run.csv；实际发布计数publication-counts.json。RSS是getrusage历史峰值，读取于关闭后、重开校验前，含预生成batch/oracle/Reader，不能当Builder精确内存。

准确变体差异serial-vs-current.patch、current-serial-vs-current.patch；诊断补丁serial-probe.patch、engine-snapshot-probe.patch及工具dd.h。variants.py可重建。源码哈希variant-hashes.json，主树起始哈希engine-hashes.json，已有tracked修改worktree-before.patch，preservation.json核对结束一致。tools.patch是相对起始树新增工具补丁。未提交推送、未修改笔记或引擎。
''')
h=json.loads((O/'engine-hashes.json').read_text());assert all(hashlib.sha256((R/p).read_bytes()).hexdigest()==v for p,v in h.items());assert subprocess.check_output(['git','diff','--binary'],cwd=R)==(O/'worktree-before.patch').read_bytes()
(O/'preservation.json').write_text(json.dumps({'engine_hashes_identical':True,'original_tracked_diff_identical':True},indent=2));(O/'git-status-after.txt').write_bytes(subprocess.check_output(['git','status','--short'],cwd=R))
folders=['serial','engine-snapshot','current-serial','serial-probe','engine-snapshot-probe'];(O/'variant-hashes.json').write_text(json.dumps({str(p.relative_to(O)):hashlib.sha256(p.read_bytes()).hexdigest() for d in folders for p in sorted((O/d).rglob('*')) if p.is_file()},indent=2))
shutil.copytree(T,O/'tool-snapshot',dirs_exist_ok=True)
with (O/'tools.patch').open('wb') as f:
 for p in sorted(T.glob('*')):
  if p.is_file():f.write(subprocess.run(['git','diff','--no-index','--','/dev/null',str(p)],stdout=subprocess.PIPE).stdout)
(O/'tool-hashes.json').write_text(json.dumps({str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(T.glob('*')) if p.is_file()},indent=2));(O/'CHANGE_LIST.txt').write_text('\n'.join(str(p.relative_to(R)) for p in sorted(T.glob('*')) if p.is_file())+'\nbenchmarks/results/dual-background-20261125/\n')
print('finalized, source unchanged')
