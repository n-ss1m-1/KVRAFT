# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,statistics,csv,hashlib,shutil,subprocess
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/batch-acceptance-20261120'
rows=[json.loads(x) for x in (o/'runs.jsonl').read_text().splitlines()]
fields=['name','kind','round','wal','batch','n','timed','status','submit_ns','flush_ns','total_ns','close_ns','assembly_ns','clear_ns','api_sum_ns','p50_ns','p95_ns','p99_ns','max_ns','tables','sst_bytes','rss_kib']
with (o/'results.csv').open('w') as f:
 w=csv.DictWriter(f,fieldnames=fields,extrasaction='ignore');w.writeheader();w.writerows(rows)
def med(rs,k):return statistics.median(x[k] for x in rs)
lines=['# 显式批量写入验收（2026-11-20）','',
'主矩阵：当前同一引擎快照，Release -O3 -DNDEBUG，C++17，诊断关闭；单前台线程，随机唯一键，16 B key / 128 B value，种子 20261115，16384 条逻辑操作，MemTable 1 MiB，块缓存容量固定为 0。WSL2 ext4，不清系统缓存。每轮配置顺序固定种子打乱，串行运行，采样期间无编译或其他测试。','',
'## 主矩阵三轮中位数','',
'提交吞吐按逻辑操作计；延迟按一次 API 提交计，不能当作批内单条延迟。总吞吐包含最终 Flush；不包含打开、关闭或校验。各列中位数分别计算，不能相加反推另一列。','',
'| WAL | 方式/批量 | 提交 op/s | 含 Flush op/s | 提交 ms | Flush ms | 关闭 ms | 组批 ms | API p50/p95/p99/max µs | SST 数 | 峰值 KiB |',
'|---|---|---:|---:|---:|---:|---:|---:|---|---:|---:|']
for wal in [1,0]:
 for b in [0,1,4,16,64,256,1024]:
  rs=[x for x in rows if x['kind']=='formal' and x['wal']==wal and x['batch']==b]
  if len(rs)<3:continue
  lines.append(f"| {wal} | {'Put' if b==0 else b} | {med(rs,'n')*1e9/med(rs,'submit_ns'):,.0f} | {med(rs,'n')*1e9/med(rs,'total_ns'):,.0f} | {med(rs,'submit_ns')/1e6:.3f} | {med(rs,'flush_ns')/1e6:.3f} | {med(rs,'close_ns')/1e6:.3f} | {med(rs,'assembly_ns')/1e6:.3f} | "+'/'.join(f"{med(rs,k)/1000:.2f}" for k in ['p50_ns','p95_ns','p99_ns','max_ns'])+f" | {med(rs,'tables')} | {med(rs,'rss_kib')} |")
lines+=['','## 逐轮主矩阵','',
'| 配置 | 提交 ms | Flush ms | 含 Flush op/s | p99 µs | max µs |','|---|---:|---:|---:|---:|---:|']
for x in rows:
 if x['kind']=='formal':
  lines.append(f"| {x['name']} | {x['submit_ns']/1e6:.3f} | {x['flush_ns']/1e6:.3f} | {x['n']*1e9/x['total_ns']:,.0f} | {x['p99_ns']/1000:.2f} | {x['max_ns']/1000:.2f} |")
lines+=['','## 独立机制核验','',
'仅机制程序包装 write/fsync，用 /proc/self/fd 路径识别 WAL，非空 WAL 的 fsync 计为数据 Sync；空文件 fsync 计为新 WAL Sync。只对成功系统调用计数。本次无短写/失败注入，WAL write 次数与批次记录数相等；字节为成功写入的实际字节。SSTable、MANIFEST、目录 fsync 不计入 WAL Sync。阶段标签是前台 thread_local；后台未写 WAL 数据，不将其 fsync 混入前台。','',
'| WAL | 批量 | 提交数据 Sync | 提交新 WAL Sync | 提交 WAL 字节 | 最终 Flush 数据/新 WAL Sync |',
'|---|---:|---:|---:|---:|---|']
for x in rows:
 if x['kind']=='extras' and x['name'].startswith('mechanism'):
  c=x['mechanism'][1];d=x['mechanism'][2]
  lines.append(f"| {x['wal']} | {x['batch']} | {c['data_sync']} | {c['new_sync']} | {c['bytes']} | {d['data_sync']}/{d['new_sync']} |")
lines+=['','## WAL-off 计时开销对照','',
'独立三轮，相同逻辑数据，full 为逐提交计时，total-only 不计各次 API、组批、Clear，也不写延迟向量；均保留提交/Flush/关闭墙钟计时。小样本差异同时受后台刷盘调度、文件持久化波动影响。','',
'| 批量 | full 提交 ms | total-only 提交 ms | full/total-only | full 总 ms | total-only 总 ms |',
'|---|---:|---:|---:|---:|---:|']
for b in [0,1,16,1024]:
 rs=[x for x in rows if x['name'].startswith('overhead') and x['batch']==b]
 a=[x for x in rs if x['timed']];z=[x for x in rs if not x['timed']]
 if len(a)==3 and len(z)==3:lines.append(f"| {b} | {med(a,'submit_ns')/1e6:.3f} | {med(z,'submit_ns')/1e6:.3f} | {med(a,'submit_ns')/med(z,'submit_ns'):.3f} | {med(a,'total_ns')/1e6:.3f} | {med(z,'total_ns')/1e6:.3f} |")
lines+=['','## WAL-off 仅总时间对照（三轮中位数）','','| 批量 | 提交 ms | 含 Flush ms | 含 Flush op/s |','|---|---:|---:|---:|']
for b in [0,1,4,16,64,256,1024]:
 rs=[x for x in rows if x['kind']=='totalonly' and x['batch']==b]
 if len(rs)==3:lines.append(f"| {b} | {med(rs,'submit_ns')/1e6:.3f} | {med(rs,'total_ns')/1e6:.3f} | {16384e9/med(rs,'total_ns'):,.0f} |")
lines+=['','## WAL-on 接口对照额外配对轮','','| 轮次 | 批量 | 提交 ms | p99 µs | max µs |','|---|---:|---:|---:|---:|']
for x in rows:
 if x['kind']=='paired':lines.append(f"| {x['round']} | {x['batch']} | {x['submit_ns']/1e6:.3f} | {x['p99_ns']/1e3:.2f} | {x['max_ns']/1e3:.2f} |")
lines+=['','## 测量边界、正确性及限制','',
'- steady_clock 单调计时。trace、oracle、延迟容量在开始前准备；提交墙钟含组批、API、Clear、循环和计时记录。WriteBatch 在各批间复用容量，首次分配包含在组批中；Put 内部组批包含在 Put API 中，外部组批项为 0。',
'- API 延迟含首次加锁、条件变量等待及唤醒重获取锁。大批次调用全程持锁，但本工具未独立测量持锁区间，API 时长不能等同纯持锁时长；没有读线程，不能推断读延迟影响。',
'- 采样结束后 Flush，返回后立即开始销毁计时，销毁后采集进程峰值，再重开禁 WAL 全量核对；最后序列化。峰值含 trace、oracle、延迟向量和引擎，不是引擎净内存。',
'- 为同时满足立即销毁和 Flush 后存活 DB 校验：每种配置另有 mechanism/check 独立轮，在 Flush 后做存活 DB 全量校验，再关闭和禁 WAL 重开校验；该轮不纳入性能表。正式轮由相同 oracle 在关闭后禁 WAL 重开验证 SSTable 全量数据。',
'- 15/15 LSM 测试通过，批次专项 1154 checks、0 failures，详见 tests.txt。独立恢复检查成功写入后不 Flush，正常析构再开 WAL 重开验证；不是进程崩溃或断电测试。',
'- MemTable 只在批前检查阈值，大批次允许超过阈值；刷盘边界及文件数量并非保证完全相同。报告实际 SST 数，机制轮可从提交阶段新 WAL Sync 数核对自动切换。',
'- 预生成输入没有在线攒批等待，摊销 API 成本 api_sum_ns/n 不代表在线单条请求延迟。批量 1024 每轮仅 16 个提交，p99 与 max 相同，尾部统计支持有限。',
'- 首版机制 pilot-1-0 未拦截同编译单元内部 WriteRecord，机制值无效但实际写入校验通过；原始记录保留，pilot-v2 修正为系统调用包装且断言通过。正式性能二进制无包装。',
'- 失败和超时保留在 runs.jsonl；runner 断言失败也保留控制台文件。正式测试无失败时不能声称已测异常性能。', '',
'## 复现','',
'构建与每个进程的完整命令分别在 configure.txt/build.txt、runs.jsonl 的 command 字段；不要原地重跑 prepare.py，它拒绝覆盖结果目录。复制工具后改用新的结果目录，先 prepare，再 pilot、formal、extras、paired、totalonly、summarize。引擎取 engine-snapshot，不能用未来源码替换以复现本次结果。源码 hash 在 engine-hashes.json，原始未提交差异在 worktree-before.patch。']
(o/'SUMMARY.md').write_text('\n'.join(lines)+'\n')
shutil.copytree(r/'benchmarks/batch_acceptance',o/'tool-snapshot',dirs_exist_ok=True)
current={str(p.relative_to(r)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((r/'src/lsm').rglob('*')) if p.is_file()}
assert current==json.loads((o/'engine-hashes.json').read_text()),'engine changed'
(o/'git-status-after.txt').write_text(subprocess.check_output(['git','status','--short'],cwd=r,text=True))
(o/'source-hashes.json').write_text(json.dumps({str(p.relative_to(o)):hashlib.sha256(p.read_bytes()).hexdigest() for base in ['engine-snapshot','tool-snapshot','tests-snapshot'] for p in (o/base).rglob('*') if p.is_file()},indent=2))
print('\n'.join(lines[:27]))
