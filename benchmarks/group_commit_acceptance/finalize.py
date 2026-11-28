# Historical experiment: review README.md before enabling execution.
import os as _benchmark_os
if _benchmark_os.environ.get("KVRAFT_RUN_HISTORICAL_BENCHMARK") != "1":
    raise SystemExit("Historical benchmark disabled: read benchmarks/README.md; explicitly set KVRAFT_RUN_HISTORICAL_BENCHMARK=1 in an isolated checkout.")

from pathlib import Path
import json,hashlib,subprocess,shutil
r=Path('/home/truth/Code/my_kvraft');o=r/'benchmarks/results/group-commit-acceptance-20261121'
records=json.loads((o/'parameter-probes.json').read_text())
assert all(x['returncode']==0 for x in records[:2])
assert all(x['returncode']==3 and 'put-return' in x['stderr'] for x in records[2:])
assert subprocess.check_output(['git','diff','--binary'],cwd=r)==(o/'worktree-before.patch').read_bytes()
assert {str(p.relative_to(r)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((r/'src/lsm').rglob('*')) if p.is_file()}==json.loads((o/'engine-hashes.json').read_text())
(o/'SUMMARY.md').write_text("""# Group Commit 正确性验收：发现问题，停止正式采样

环境：WSL2 ext4，GCC 11.4，Release -O3 -DNDEBUG，C++17，LSM_DIAGNOSTICS=OFF。完整环境、编译命令、原始工作树 patch、HEAD 和源码快照/hash 均在本目录。未修改引擎、原有测试、历史结果或笔记，未提交/推送。

## 结果

- 全部 16 组 LSM 测试通过（tests.txt）。
- 组提交专项连续三次通过（group-repeat.txt），覆盖共享同步、参数隔离、请求上限、字节预算边界、首个超预算请求、Sync 失败与部分 Append 失败。
- 独立参数探针发现：kMaxGroupRequests=0 时 DB 构造成功，Put 返回成功，Flush 返回，但 Get 不存在；独立临时数据库重复两次，均退出码 3（结果不符）。相同程序上限 1/32 均成功且校验通过。
- 按用户要求，发现引擎正确性问题后停止正式采样。没有运行主矩阵、历史性能对照或扩展，不给出任何性能收益、平均组大小或查询延迟结论。

## 原因（代码核对）

db.h 将 kMaxGroupRequests 标注为不能为 0，但构造函数未检查。db.cc 的 Write 在选择组成员前检查 group.size() >= options_.kMaxGroupRequests，0 时立即结束，group 为空。WriteGroup 在无合法写入时返回；Write 收尾也迭代空组，pending.error 为空，组长返回成功，却从未提交请求。

该路径还将局部 PendingWrite 指针留在 writeQueue_ 中；调用返回后指针悬空。这是代码推断的后续风险，本次仅做一次 Put/Flush/Get，不继续使用悬空请求来扩大故障。0 为文档禁止的配置，该问题不证明默认 32 或对照 1 的配置有数据错误，但应明确拒绝无效配置，而不能返回成功并丢失写入。

## 可复现证据

parameter-probes.json 保存上限 1/32/0/0 四次调用的输出、退出码和命令。parameter_probe.cc 使用临时目录，WAL off：
构造 DB → Put(key,value) → Flush → Get(key) 对比 value。
0 两次均输出 put-return，随后退出码 3，说明不是超时，而是返回成功但校验失败。目录由 TemporaryDirectory 清理，未操作用户数据库。

在新目录重新编译 parameter_probe.cc，链接本目录 test-build/liblsm_engine.a，参数为 临时数据库目录 和 请求数上限。精确构建命令位于 commands.jsonl；原始配置与测试日志保留。prepare.py 拒绝覆盖现有结果目录，复现时修改 o 到新路径。

probe.py 首轮预期为 timeout，实际四次均返回，因而最后的 harness 断言失败；原始 parameter-probes.json 完整保留。该断言不是引擎测试结果。finalize.py 依据实际结果确认上限 0 返回成功但值缺失，未重新覆盖原始记录。

## 六个性能问题的结论边界

1. 专项可控时序测试支持“能共享 Sync”，正式闭环负载的平均组大小未测量。
2. 同源码不合并与默认组提交的吞吐/API 延迟未采样。
3. 单线程/WAL-off 排队成本未测量。
4. 专项预算边界测试通过，但 0 请求上限未被拒绝；没有性能配置建议。
5. Get 仍使用 DB 锁，写组在同锁下执行是代码事实；读线程延迟影响尚无测量证据。
6. 本轮没有证据支持或否定主动攒组等待的收益，不实现该机制。

## 修改文件

仅新增 benchmarks/group_commit_acceptance/prepare.py、parameter_probe.cc、probe.py、finalize.py 和本结果目录。保存完整引擎与测试快照；现有 tracked diff 及引擎源码 hash 均核对未变。
""")
(o/'git-status-after.txt').write_text(subprocess.check_output(['git','status','--short'],cwd=r,text=True))
shutil.copytree(r/'benchmarks/group_commit_acceptance',o/'tool-snapshot',dirs_exist_ok=True)
(o/'source-hashes.json').write_text(json.dumps({str(p.relative_to(o)):hashlib.sha256(p.read_bytes()).hexdigest() for base in ['engine-snapshot','tests-snapshot','test-support-snapshot','tool-snapshot'] for p in (o/base).rglob('*') if p.is_file()},indent=2))
print('Confirmed correctness failure; formal sampling not started; engine unchanged.')
