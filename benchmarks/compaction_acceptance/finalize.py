# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,hashlib,shutil,subprocess
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/compaction-acceptance-20261125'
text='''
## 结论：测量证实

- 18组测试及全部性能轮正确性通过：输出每user_key一条最新记录，删除标记保留，sequence与oracle一致，FlushedSequence不变；join后正式物理表与MANIFEST一致，禁用WAL重开全量校验通过。
- 默认4MiB：16/64张输入均输出3张表。多版本262144条降至65536条，13108个最新删除标记全部保留；64表文件39.400→9.439MiB，约缩小76.0%。无旧版本记录数不变，文件10.258→11.058MiB，反增约7.8%，不应称为压缩空间收益。
- 64表多版本，稳定均匀查询：无缓存32046→39096op/s（1.22倍），足量缓存247656→314221（1.27倍）。范围内缺失查询提升131.53倍/32.28倍，收益高度依赖本夹具。足量缓存固定热点757305→446255（0.59倍），不是所有查询都受益。轮间范围及尾延迟见完整表；不把单轮max解释为稳定改善。
- 64表多版本Compact默认五轮中位543.750ms，关闭/join8.331ms，从调用到关闭551.109ms；各字段各自取中位数，不能要求中位数之和严格相等。1MiB目标输出9表，515.420ms，峰值28.43MiB；4MiB峰值31.71MiB。基线26.69MiB仅三轮，RSS含程序与oracle，不能将差值当Builder精确占用。
- 合并期间三轮：WAL-off批64写吞吐211177→28958逻辑op/s，API最大延迟中位20.07→509.02ms；最终Flush32.04→30.19ms，并未明显增加。长等待主要可能已落在提交阶段，不能只看最终Flush。控制总时间含为标记窗口而sleep，不能与合并组总时间直接归因为吞吐差异。
- 读全程吞吐30309→37093op/s，但覆盖合并后更快的查询，不能证明构建期间无干扰。调用窗口内读p99与长暂停详见impact-windows.csv；合并读全程max中位约5.52ms，控制约1.68ms，三轮不足以外推稳定最坏延迟。

## 结论：代码推断

- MergeTables使用每输入表迭代器+最小堆，跳过同user_key旧版本，保留最新删除；输出按EncodedEntrySize累计软目标拆分。Bloom按target/minEntrySize上界预估，解释了输出元数据开销增加的可能来源，实际非记录字节含Bloom/索引/块计数/footer/CRC，未逐项测占比。
- 无缓存命中仍须读取候选块、CRC、构造验证及块内扫描，减少表数不保证大幅提升命中。热点记录在新旧块内位置/扫描长度变化，可解释热点缓存场景变慢；本轮未做独立诊断，不能确认占比。
- 合并构建锁外，MANIFEST发布持DB锁；同一后台串行处理合并与immutable Flush，写入遇immutable背压可能等待合并结束。这与写长调用相符，但未量出各阶段份额。
- Builder保留一张输出表的块和Bloom，写完reset。拆分降低当前输出缓冲，但输入Reader/迭代器/缓存/程序仍消耗内存。不能把目标字节当进程内存硬上限。

## 证据不足与下一步优先级

1. 优先研究后台调度与背压阶段：写长暂停已有明确迹象。下一轮应独立测背压、构建、发布锁及清理，尚不能断言锁改造能解决，也不实施改动。
2. 流式Writer值得研究：1MiB拆分降低约3.28MiB峰值；但本次仅65K逻辑键、约9MiB输出，无法证明流式Writer必要性或规模上限。需更大输出及内存分解才可作决定。
3. 针对缓存热点回退研究块内扫描与记录布局，当前只有现象与代码解释。缓存容量充足不等于每次命中，未测Usage或命中率，不给出虚构机制比例。

本轮没有WAL-on写并发、多读线程、大于65K键的内存扩展测试。所有结论限WSL ext4和固定闭环工作负载，不能推广生产。诊断始终关闭；正式运行无并行构建或其他验收。保留慢轮、v1口径结果、全部历史结果及现有工作树。
'''
with (O/'SUMMARY.md').open('a') as f:f.write(text)
(O/'REPRODUCE.md').write_text('''# 复现

现有结果目录拒绝覆盖；复现时先把prepare.py/run.py及后续脚本中的结果目录统一改为一个新的唯一名称。工具位于benchmarks/compaction_acceptance；最终工具完整副本在tool-snapshot。引擎使用engine-snapshot，当前工作树已有修改完整保留于worktree-before.patch；HEAD见git-head.txt。engine-hashes.json及preservation.json验证原引擎和tracked diff不变。环境见environment.txt。

```sh
cd /home/truth/Code/my_kvraft
python3 benchmarks/compaction_acceptance/prepare.py
cmake -S benchmarks/compaction_acceptance -B benchmarks/results/NEW/perf-build -DCMAKE_BUILD_TYPE=Release -DLSM_DIAGNOSTICS=OFF -DENGINE_ROOT=/home/truth/Code/my_kvraft/benchmarks/results/NEW/engine-snapshot
cmake --build benchmarks/results/NEW/perf-build -j4
python3 benchmarks/compaction_acceptance/run.py
python3 benchmarks/compaction_acceptance/rerun_reads.py
python3 benchmarks/compaction_acceptance/extra.py
python3 benchmarks/compaction_acceptance/summarize.py
python3 benchmarks/compaction_acceptance/finalize.py
```

原始commands.jsonl记录逐进程参数；构建参数见configure-perf.txt、perf-build/CMakeCache.txt、compile_commands.json和flags.make。Release GCC11.4 -O3 -DNDEBUG -std=c++17，无诊断、无LTO。test-build保存18组构建和测试，tests.txt为详细输出。

run.py先pilot再三轮成本、查询、影响；extra.py补multi-64查询和输出目标两轮（配对顺序交错）。查询v1/v2实现相同；v2仅修正impact读的计时边界，原v1二进制bench-v1、原源码bench-v1.cc保留。若从最终源码重新运行，最初impact读已经是修正口径，不需要将其称为旧口径。rerun_reads仍可独立复核。v1失败轮无，所有完成轮保留。

runs/*.json为逐轮汇总；*-uniform/hot/missing/deleted-0/1.csv保存逐请求延迟；*-timeline.csv为并发完整时间线；impact-windows.csv按开始时间分段。0=首次trace，1=相同trace再访。fixtures为关闭后夹具，copies为独立运行副本，均保留。oracle.bin是工具生成的最新状态文件，不是引擎文件。容量来自SST footer/index逐块size求和（排除块CRC），capacities.json保存值。没有清系统缓存。

峰值RSS以getrusage统计进程从启动至测量点的历史峰值，含oracle/预热/线程/延迟数组等；不测其他进程。无合并进程作为粗略基线；没有单独诊断轮和内部时钟开销对照，极短缺失查询受计时记录开销影响。API字段是单次公共调用，不含真正请求到达前排队；批64延迟不是单条逻辑操作延迟。

仅新增benchmarks/compaction_acceptance/和benchmarks/results/compaction-acceptance-20261125/；没有引擎改动、笔记改动或Git提交。
''')
# Reconstruct original source of v1 timing boundary, retain it separately.
s=(R/'benchmarks/compaction_acceptance/bench.cc').read_text();s=s.replace('std::vector<std::optional<std::string>> readResults(writing?0:count);','').replace('else readResults[i]=db->Get(keys[trace[i]]);','else {auto v=db->Get(keys[trace[i]]);need(v&&*v==m.at(keys[trace[i]]).value,"concurrent read");}').replace('if(!writing)for(size_t i=0;i<count;++i)need(readResults[i]&&*readResults[i]==m.at(keys[trace[i]]).value,"concurrent trace mismatch");','');(O/'bench-v1.cc').write_text(s)
shutil.copytree(R/'benchmarks/compaction_acceptance',O/'tool-snapshot',dirs_exist_ok=True)
files=list((R/'benchmarks/compaction_acceptance').glob('*'))
(O/'tool-hashes.json').write_text(json.dumps({str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files if p.is_file()},indent=2))
(O/'CHANGE_LIST.txt').write_text('\n'.join(str(p.relative_to(R)) for p in files if p.is_file())+'\nbenchmarks/results/compaction-acceptance-20261125/ (new result artifacts)\n')
with (O/'tools.patch').open('wb') as f:
 for p in files:
  if p.is_file():f.write(subprocess.run(['git','diff','--no-index','--','/dev/null',str(p)],stdout=subprocess.PIPE).stdout)
assert subprocess.check_output(['git','diff','--binary'],cwd=R)==(O/'worktree-before.patch').read_bytes()
old=json.loads((O/'engine-hashes.json').read_text());assert all(hashlib.sha256((R/p).read_bytes()).hexdigest()==v for p,v in old.items())
print('finalized',len(list((O/'runs').glob('*.json'))),'process records')
