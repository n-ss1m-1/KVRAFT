# 工具归档检查记录

日期：2026-11-28。本次整理与提交不重新建立性能结论，不修改历史结果。

## 已完成

- 117 个新增历史 Python 脚本通过语法解析和默认停止执行检查；检查时未导入其模块逻辑。
- 13 个独立 CMake 项目使用对应历史引擎快照，在新的临时构建目录全部 Release 重建通过。
- batch_acceptance：256 条、混合写删、每批 16 条、WAL-off，小规模运行及重开校验通过。
- group_commit_v2：256 条、2 线程、每批 4 条、WAL-off，机制工具运行及数据校验通过。
- source_expansion：2048 条唯一键、WAL-off、等待收敛的小规模运行及数据校验通过。
- 主引擎源码哈希、原有非 benchmark 工作树差异与整理前一致。

13 个构建项目为 crc_acceptance、block_cache_acceptance、batch_acceptance、group_commit_v2、compaction_acceptance、compaction_diagnosis、dual_background、write_tail_diagnosis、auto_compaction、compaction_rewrite、source_expansion、source_expansion_resume、source_expansion_timeout_audit。

group_commit_acceptance 是早期参数探测，没有独立 CMake 项目，不计入 13 项重建。顶层早期 baseline/comparison/diagnostics 已有跟踪源码，本次不把它们计为重新验收。

## 边界

历史快照来自本机独立结果归档，不随工具提交。仅克隆源码不能完成这些构建；必须另备 README 指定的快照。没有运行完整正式矩阵、重做统计报告或验证所有历史源码变换脚本。

一些脚本仍使用原实验路径和带日期的目录；它们被明确归类为历史工具，并加显式执行保护。归档没有把它们重构为统一可移植测试框架。

check_archive.py 用于不执行实验的语法、清单哈希和本地源码 include 检查；新增或调整归档文件时应同步更新 archive_inventory.json。哈希清单不包含自身。
