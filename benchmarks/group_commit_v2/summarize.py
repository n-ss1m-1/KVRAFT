# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,csv,statistics,hashlib,subprocess,shutil,difflib
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/group-commit-acceptance-20261121-v2'
rows=[json.loads(x) for x in (o/'runs.jsonl').read_text().splitlines()]
good=[x for x in rows if x['status']=='ok']
fields=['name','kind','wal','threads','limit','budget','batch','n','timed','readers','status','operations','calls','request_ns','total_ns','flush_ns','close_ns','assembly_ns','p50_ns','p95_ns','p99_ns','max_ns','reader_calls','reader_ns','reader_p50_ns','reader_p95_ns','reader_p99_ns','reader_max_ns','rss_kib','tables']
with (o/'results.csv').open('w') as f:
 w=csv.DictWriter(f,fields,extrasaction='ignore');w.writeheader();w.writerows(rows)
def med(xs,k):return statistics.median(x[k] for x in xs)
lines=['# Group Commit 验收（当前源码，2026-11-21）','','## 正确性与环境','',
'16/16 LSM 测试通过；组提交专项连续三次，每次 280 检查通过。构造函数在文件系统操作前拒绝 memTableSizeLimit/maxGroupRequests/maxGroupBytes=0；blockCacheCapacity=0 合法，上次发现的零请求上限问题已由当前源码处理。本次不改引擎。','',
'WSL2 ext4，Release -O3 -DNDEBUG，C++17，诊断 OFF，无 LTO。16 B key、128 B value，种子 20261115+t；线程 t 独占 [t*N/T,(t+1)*N/T) 的 key，并在区间内固定随机打乱。各线程数使用相同键值集合，各配置同线程数 trace 完全相同。N=16384 不随线程数增加，MemTable=1 MiB，blockCacheCapacity=0。不开系统缓存清理。','',
'## 主矩阵（三轮中位数）','',
'A=maxGroupRequests 1，B=32；两者 maxGroupBytes 均 1 MiB，只有运行配置不同。同当前源码比较只回答请求合并收益，不能测出队列机制自身成本。','',
'| WAL | 线程 | 上限 | 逻辑 op/s | API/s | 含 Flush op/s | 请求 ms | Flush ms | 关闭 ms | API p50/p95/p99/max µs |',
'|---|---:|---:|---:|---:|---:|---:|---:|---:|---|']
for wal in [1,0]:
 for t in [1,2,4,8]:
  for g in [1,32]:
   xs=[x for x in good if x['kind']=='formal' and (x['wal'],x['threads'],x['limit'])==(wal,t,g)]
   if len(xs)!=3:continue
   lines.append(f"| {wal} | {t} | {g} | {16384e9/med(xs,'request_ns'):,.0f} | {med(xs,'calls')*1e9/med(xs,'request_ns'):,.0f} | {16384e9/med(xs,'total_ns'):,.0f} | {med(xs,'request_ns')/1e6:.3f} | {med(xs,'flush_ns')/1e6:.3f} | {med(xs,'close_ns')/1e6:.3f} | "+'/'.join(f"{med(xs,k)/1000:.2f}" for k in ['p50_ns','p95_ns','p99_ns','max_ns'])+' |')
lines+=['','## 主矩阵逐轮（保留慢轮）','','| 配置 | 请求 ms | Flush ms | 含 Flush op/s | p50/p95/p99/max µs |','|---|---:|---:|---:|---|']
for x in good:
 if x['kind'] in ['formal','paired']:
  lines.append(f"| {x['name']} | {x['request_ns']/1e6:.3f} | {x['flush_ns']/1e6:.3f} | {x['operations']*1e9/x['total_ns']:,.0f} | "+'/'.join(f"{x[k]/1000:.2f}" for k in ['p50_ns','p95_ns','p99_ns','max_ns'])+' |')
lines+=['','## 独立机制（不混入正式性能）','','| 配置 | 完成 API | WAL 记录 | 数据 Sync | 平均请求/Sync | WAL 字节 | 新 WAL Sync | 组大小:次数 |','|---|---:|---:|---:|---:|---:|---:|---|']
for x in good:
 if x['mechanism_enabled']:
  c=x['mechanism'][1];hist=', '.join(f"{i}:{v}" for i,v in enumerate(c['hist']) if v)
  lines.append(f"| {x['name']} | {x['calls']} | {c['records']} | {c['data_sync']} | {c['records']/c['data_sync'] if c['data_sync'] else 0:.3f} | {c['wal_bytes']} | {c['new_sync']} | {hist} |")
lines+=['','## 扩展（三轮中位数）','','| 上限 | 批条数 | 字节预算 | 读线程 | 写 op/s | 含 Flush op/s | API p50/p95/p99/max µs | Get/s | Get p50/p95/p99/max µs |','|---|---:|---:|---:|---:|---:|---|---:|---|']
keys=sorted(set((x['limit'],x['batch'],x['budget'],x['readers']) for x in good if x['kind']=='extensions'))
for key in keys:
 xs=[x for x in good if x['kind']=='extensions' and (x['limit'],x['batch'],x['budget'],x['readers'])==key]
 if len(xs)!=3:continue
 rd=[x['reader_calls']*1e9/x['reader_ns'] if x['reader_ns'] else 0 for x in xs]
 lines.append('| '+' | '.join(map(str,key))+f" | {16384e9/med(xs,'request_ns'):,.0f} | {16384e9/med(xs,'total_ns'):,.0f} | "+'/'.join(f"{med(xs,k)/1000:.2f}" for k in ['p50_ns','p95_ns','p99_ns','max_ns'])+f" | {statistics.median(rd):,.0f} | "+'/'.join(f"{med(xs,k)/1000:.2f}" for k in ['reader_p50_ns','reader_p95_ns','reader_p99_ns','reader_max_ns'])+' |')
lines+=['','## WAL-off 计时开销（三轮中位数）','','| 线程 | 上限 | full 请求 ms | total-only 请求 ms | full/total-only |','|---|---:|---:|---:|---:|']
for t in [1,8]:
 for g in [1,32]:
  a=[x for x in good if x['kind']=='overhead' and (x['threads'],x['limit'],x['timed'])==(t,g,1)]
  b=[x for x in good if x['kind']=='overhead' and (x['threads'],x['limit'],x['timed'])==(t,g,0)]
  if len(a)==3 and len(b)==3:lines.append(f"| {t} | {g} | {med(a,'request_ns')/1e6:.3f} | {med(b,'request_ns')/1e6:.3f} | {med(a,'request_ns')/med(b,'request_ns'):.3f} |")
lines+=['','## 测量边界与限制','',
'- 单调 steady_clock；trace 和每线程延迟容量在开始前准备。C++17 mutex/CV 屏障只用于启动，无逐请求共享统计锁。闭环固定并发，完成后才发下一次请求，不能代表独立到达流量的排队尾延迟。',
'- API 前后计时含入队、锁等待、排队、执行、背压与唤醒。WriteBatch 构造/填充在 API 外、在线程内单独计时；复用 batch 并 Clear。Put 内部单项 batch 构造计入 Put API。批量 API 延迟不可除以条数称为单条延迟。',
'- 请求墙钟为最早工作线程开始至最晚写线程结束；另存 barrier_to_end_ns 和 drain_ns。所有写线程结束/join、读线程停止/join 后 Flush，total_ns 为屏障释放到 Flush 返回，含线程收尾及读线程退出。Flush 返回后仅设置线程局部阶段并立即销毁 DB，close_ns 含析构/join，随后才校验与序列化。',
'- 为兼顾立即销毁与 Flush 后校验：关闭后直接读取 MANIFEST/SSTable 全量核对，再禁 WAL 重开全量核对；两次均不计时。独立 recovery 场景不 Flush，正常关闭后启 WAL 重开全量核对，不是崩溃/断电模拟。',
'- WAL 机制程序仅包装 write/fsync，按 /proc/self/fd 识别 .wal，fstat 区分空新 WAL 与非空数据 WAL，目录/其他 fsync 分列。每个线程独立计数槽，join 后归并；后台拥有独立槽，其时间阶段不随前台切换。阶段 0=准备/后台，1=提交，2=最终 Flush，3=关闭，4=校验。每轮 JSON 保留所有阶段，不把 Sync 合并统计。',
'- 机制轮校验每次完整 write 恰为一条 WAL 记录，短写计 partial；正式吞吐无包装。每个组长同线程自上次数据 Sync 以来完整记录数得到组大小分布，仅限本轮无错误且 partial=0 时成立，不把逻辑条数当请求数。机制程序有 readlink/fstat 开销，分布可能偏移，不能当作正式轮精确组分布。',
'- 读线程只查 64 个固定已验证且不被修改的 key，提前 Put/Flush；缓存关闭。主要观察锁竞争，也含 SSTable 查询，不代表一般业务混合比例。读线程闭环不限速，无主动等待；读延迟数组预分配，若超出容量只保存前缀样本，JSON 中 reader_sampled 可核对；不能将读写吞吐相加解释为写吞吐。',
'- RSS 为关闭后、校验前进程峰值，包含 trace、线程栈、延迟数组和引擎，不是引擎净内存。最大延迟单轮变化不作为稳定改善证据。各指标分别取三轮中位数，不能把中位数阶段相加当成中位总时间。',
'- 主矩阵每个线程数请求字节仅 173 B，最多 8 请求，远低于 1 MiB；不能用主矩阵证明字节预算影响。扩展 1024 条/请求为 164876 B，200000 B 预算只容 1 请求，1 MiB 最多 6 请求，8 线程具备触发机会。',
'- 不清系统缓存，不推广为生产环境性能。不实现主动攒组等待、Group Commit 调优或锁修改。','',
'## 复现与交付','',
'prepare.py 创建全新结果目录、保存源码/测试快照、构建并运行所有测试；拒绝覆盖历史。CMake 性能项目 ENGINE_ROOT 指向本目录 engine-snapshot，诊断 OFF，Release/C++17；perf-configure.txt、perf-build.txt、build/compile_commands.json 保存编译证据。run.py 的 pilot/formal/mechanism/extensions/extension-mechanism/overhead 分批串行执行。复现须先将脚本输出路径改为新目录。所有精确子进程命令与原始 stdout/stderr/JSON 在 runs.jsonl 及每轮文件，results.csv 为便于比较的扁平表，*.latency.csv 保留线程/调用顺序。',
'本次仅新增 benchmarks/group_commit_v2/ 及当前结果目录，不运行或修改引用旧字段的历史工具。工具直接使用 blockCacheCapacity/maxGroupRequests/maxGroupBytes。引擎、原有测试、笔记、旧结果未修改，未提交或推送。']
(o/'SUMMARY.md').write_text('\n'.join(lines)+'\n')
before=json.loads((o/'engine-hashes.json').read_text());now={str(p.relative_to(r)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((r/'src/lsm').rglob('*')) if p.is_file()}
assert before==now
assert subprocess.check_output(['git','diff','--binary'],cwd=r)==(o/'worktree-before.patch').read_bytes()
(o/'git-status-after.txt').write_text(subprocess.check_output(['git','status','--short'],cwd=r,text=True))
shutil.copytree(r/'benchmarks/group_commit_v2',o/'tool-snapshot',dirs_exist_ok=True)
(o/'source-hashes.json').write_text(json.dumps({str(p.relative_to(o)):hashlib.sha256(p.read_bytes()).hexdigest() for base in ['engine-snapshot','tests-snapshot','test-support-snapshot','tool-snapshot'] for p in (o/base).rglob('*') if p.is_file()},indent=2))
print('records',len(rows))
