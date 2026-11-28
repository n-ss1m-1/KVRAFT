# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,hashlib,subprocess,shutil,statistics
R=Path('/home/truth/Code/my_kvraft');O=R/'benchmarks/results/compaction-diagnosis-20261125'
text='''
## 结论与证据等级

### 1. 热点退化已复现

诊断关闭三轮：稳定吞吐744634→429788 op/s，合并后为57.7%；接近历史59%。同一当前源码与历史源码哈希完全一致，复用原multi-64前后文件副本；oracle、32个热点键、8192次trace均一致，trace-hashes.json可核对。热点全为存活键，每侧相同OS预热、独立新DB和空块缓存。

机制稳定阶段所有候选块均缓存命中，实际加载0；代表轮Usage为60707→8157 B，配置容量均40531456 B。Usage取LoadDataBlock返回前读取，查询完成后稳定值不变；不是进程RSS，也不等于全部工作集填充。

原热点取最小32个存活键，明显存在位置偏置：最新版本所在块的平均1起始位置2.53→13.25，中位2→11。所有热点文件、offset、块字节和记录数见hot-key-positions.json。按实际请求计，扫描记录2.907→14.103/请求（包括Bloom放行未找到及跨块）；并非只计目标块位置。表检查10.691→3/请求，Bloom拒绝9.474→2，块缓存访问1.217→1.025/请求，不能把表检查减少等同整体提速。少量跨块来自FindStartBlock遇块首目标的起始块选择，保留原语义。

代表机制轮抽样均值：块内Get约411→1884 ns/候选块，索引89→102ns，缓存Get85→67ns。索引项平均138→1033，实际缓存访问时间未增加。其他机制轮原始数据保留；这些为1/32请求抽样，绝非精确全量阶段占比。

两个随机热点集的扫描记录/请求分别24.916→16.781、21.306→16.164，原热点的扫描退化方向消失。仅机制验证各一轮，不能当作正式吞吐对照，足以说明原最小键热点不能代表一般热点。记录位置和线性解码共同支持本次退化解释；没有做替代算法或性能优化，不能声称已测得优化收益。缓存维护不是这组退化的主要证据来源，仍不能排除所有未测CPU/分配成本。

### 2. 509ms未稳定复现，但发现相同类别的长等待

正式诊断关闭写入追加至六轮。控制最大批次18.8–23.3ms；合并组六轮约20.36、21.67、24.30、18.13、216.28、214.30ms，中位22.99ms。不能复用历史509ms作为当前稳定值；分布强烈依赖后台任务领取时序，所有慢轮保留。

每轮16384逻辑Put、256个独立WriteBatch×64项，单写线程，WAL off，MemTable1MiB。组提交每组1请求（无另一个写线程可合并）；机制轮4次自动轮换、5个Flush任务（包括最终Flush导致的轮换），正式提交背压3次。完整计数、阶段和等待在*.probe.json；phase3写、phase4最终Flush、phase6后台，验证单独分类。后台Flush虽由前台阶段产生仍归执行任务，不用全局阶段标签。

独立机制三轮长批次208.75–213.52ms。以r0为例：API213.52ms，immutable条件等待213.36ms；相交区间含合并构建184.08ms（其中三次Writer合计97.77ms）、发布持锁4.83ms、清理2.79ms、随后Flush17.04ms。父子嵌套且可能重叠，以上不能相加解释墙钟。队列等待整轮约0.025ms、首次DB锁整轮0.377ms；不支持将这个长批次主要归因MANIFEST锁。最慢批次与后台事件的精确交集保存在*-slow-overlap.json。

机制轮合并构建总187.58–197.96ms，Writer95.20–100.59ms，MANIFEST Save4.42–5.08ms，发布持锁4.50–5.17ms。Writer包括写入/fsync/rename/目录同步，未细分为纯磁盘耗时。构建计时包含归并、解码、Builder和Writer；扣除Writer只是同线程包围区间残差，不能作为独立测得的归并CPU。

代码支持：Compact与Flush共享后台线程；当immutable占用时，WriteGroup条件等待需等后台完成合并/清理并刷盘释放immutable。时间线支持本轮等待路径关联，不声称仅凭相交区间已证明任意调度改动的因果收益。未注入受控延时。

### 3. 诊断开销与未知项

开启诊断明显改变写入任务领取时序：正式前三轮长等待未出现，机制三轮均出现；因此禁止用机制轮吞吐或延迟替代正式结果，也不把差值称为固定计时开销。全量计数、Add聚合计时、固定事件写入仍可能影响排程。查询配对也有波动，见开销表；抽样时钟本身不机械扣除。没有继续用诊断长等待值扩大正式性能结论。

既有diagnostics.h已有扫描/表/Bloom/背压等，但其Add逐条计时且缺少缓存和Compaction事件。本次复用相同位置与定义，以独立probe取代逐条Add时钟，整组Add计时；原LSM_DIAGNOSTICS始终OFF，PROBE只用于机制副本。没有启用引擎缓存统计API或更改生产接口。

固定8线程槽位永久保留到进程结束，槽位只分配不复用；线程退出数据不丢失，每槽8192事件，超限仅记dropped。注册无分配、不抛异常，无全局统计锁；统计和时间线仅在全部线程及后台join后输出。所有轮overflow与events_dropped应为0。Usage会额外获取缓存锁，因此缓存Get样本仅计Get调用，但整体机制开销包括Usage查询；不能将机制缓存锁成本外推生产。

失败：无引擎正确性失败；诊断脚本开发期有字符串匹配失败，仅影响隔离副本，未进入正式采样。所有完成正式/机制轮均重开验证逻辑状态；validate.py再独立解析每轮所有物理表，按sequence恢复最新状态并与原oracle及16384条新增操作核对，包括删除、原sequence。单独compact-invariants还验证一键一记录、FlushedSequence不变及清理。正常关闭重开不是断电恢复测试。

### 4. 最值得研究的方向（本轮未实施）

1. 块内定位/解码：目标是减少从块首扫描与重复解析分配；本次热点线性扫描2.9→14.1条/请求、抽样块内Get显著增加，是明确证据。但应以多个键位置/热点集验收，避免只改善某个偏置夹具。
2. 后台合并与刷盘调度：目标是降低immutable等待长合并占用的背压长尾。当前证据优先指向后台可用性，尚不支持优先缩短MANIFEST锁来解决约200–500ms长等待。任何后续流式/增量或调度方案均需单独实现与正确性验收。

所有结论限本WSL固定负载，不推广生产。没有优化引擎、提交、推送、修改笔记或覆盖历史结果。
'''
with (O/'SUMMARY.md').open('a') as f:f.write(text)
(O/'REPRODUCE.md').write_text('''# 复现与边界

先将工具各脚本中compaction-diagnosis-20261125替换为新唯一结果目录，prepare拒绝覆盖。历史夹具保留在compaction-acceptance-20261125。本次source comparison确认与历史完全一致。

```sh
cd /home/truth/Code/my_kvraft
python3 benchmarks/compaction_diagnosis/prepare.py
python3 benchmarks/compaction_diagnosis/instrument.py
cmake -S benchmarks/compaction_diagnosis -B benchmarks/results/NEW/build -DCMAKE_BUILD_TYPE=Release -DLSM_DIAGNOSTICS=OFF -DRESULT_ROOT=/home/truth/Code/my_kvraft/benchmarks/results/NEW
cmake --build benchmarks/results/NEW/build -j4
python3 benchmarks/compaction_diagnosis/run.py
python3 benchmarks/compaction_diagnosis/extra.py
python3 benchmarks/compaction_diagnosis/positions.py
python3 benchmarks/compaction_diagnosis/validate.py
python3 benchmarks/compaction_diagnosis/summarize.py
python3 benchmarks/compaction_diagnosis/finalize.py
```

run包含pilot、三轮交错plain/probe对照与两个随机热点机制轮，extra仅增加三个写入配对。正式采样无并行编译或其他测试。plain链接original原引擎、probe链接instrumented，均Release C++17无LTO；LSM_DIAGNOSTICS=OFF。内部probe是独立诊断而非正式性能。配置见build/CMakeCache.txt、compile_commands.json，命令见commands.jsonl。固定seed20261115、16Bkey、128Bvalue、1MiB阈值、默认32请求/1MiB组预算、目标4MiB。查询缓存40531456B，写入缓存0。

隔离补丁instrumentation.patch相对original，包含空白规范化；dd.h在tool-snapshot，同样属于诊断补丁依赖；工具可由instrument.py完全重建。工具相对原工作树全为新增，tools.patch保存新增文件。original与instrumented完整保存，hashes.json/variant-hashes.json记录哈希，worktree-before.patch为已有差异。主树没有修改。仅新增benchmarks/compaction_diagnosis/与benchmarks/results/compaction-diagnosis-20261125/。

时间线用steady_clock绝对ns，同一进程可直接对齐；前台API CSV相对起点，最慢对应以引擎WriteApi事件确认。条件等待含重新获取锁；QueueLock只计首次队列锁，QueueWait含队列条件等待与重新获取锁，不含末尾完成收尾锁的独立时间，该残差仍在API。FirstLock为WriteGroup首次DB锁。Add从整组MemTable Add开始至try块末，包含循环/错误检查，不逐项读时钟。

后台compact_task从CompactTables进入（领取后）至清理结束；不包含领取前排队/任务锁等待，公开Compact时间另保留。merge_and_build含Writer；publish_hold从获取发布锁后至释放前（Timer销毁先于lock），包含Manifest Save；cleanup含旧Reader释放/unlink/目录同步。flush_task入口在领取之后，flush_build不含Writer，flush_writer含完整文件持久化，flush_publish_hold含Save与immutable释放；任务结束包含其余尾部。任务领取精确排队/锁耗时未新增统计，不能声称已分离。已有自动刷盘次数用任务数与Rotate数区分，最终Flush单独上下文4。

查询Tables计SSTableReader::Get入口，包括Bloom拒绝；IndexItems的units为各次定位时索引项数之和，index.count为调用数；sample每32请求，从每进程计数0开始，不是每条解码读时钟。BlockGet累计扫描数全量；cache_get含Get，Usage锁在此计时外但会产生整体开销。缓存Usage采用全部候选块加载函数返回时读取，稳定阶段没加载、值不变。命中块bytes/records/position由独立文件解析定位给出，不是OS缓存指标。查询机制不是块级随机抽样，不能用样本均值外推总体CPU占比。

延迟CSV预分配，计时内不输出；热点与写入验证在性能计时外。oracle和trace生成不计时。写入预构造256批独立batch，每批64条，不含在线攒批等待。Compact与写线程并发启动无强制暂停或延迟；控制组sleep只是原工具标记同长度窗口，不影响前台工作量，不能拿包含sleep的总时间强算因果收益。
''')
# Confirm no dropped diagnostics, preserve hashes and new-file patches.
for p in (O/'runs').glob('*.probe.json'):
 v=json.loads(p.read_text());assert v['overflow']==0 and v['events_dropped']==0,p
h=json.loads((O/'hashes.json').read_text());assert all(hashlib.sha256((R/p).read_bytes()).hexdigest()==v for p,v in h.items());assert subprocess.check_output(['git','diff','--binary'],cwd=R)==(O/'worktree-before.patch').read_bytes()
(O/'variant-hashes.json').write_text(json.dumps({str(p.relative_to(O)):hashlib.sha256(p.read_bytes()).hexdigest() for folder in ['original','instrumented'] for p in sorted((O/folder).rglob('*')) if p.is_file()},indent=2))
shutil.copytree(R/'benchmarks/compaction_diagnosis',O/'tool-snapshot',dirs_exist_ok=True)
with (O/'tools.patch').open('wb') as f:
 for p in sorted((R/'benchmarks/compaction_diagnosis').glob('*')):
  if p.is_file():f.write(subprocess.run(['git','diff','--no-index','--','/dev/null',str(p)],stdout=subprocess.PIPE).stdout)
(O/'tool-hashes.json').write_text(json.dumps({str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((R/'benchmarks/compaction_diagnosis').glob('*')) if p.is_file()},indent=2))
(O/'git-status-after.txt').write_bytes(subprocess.check_output(['git','status','--short'],cwd=R));print('finalized; primary engine and tracked patch unchanged')
