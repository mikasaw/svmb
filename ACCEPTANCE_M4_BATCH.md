# M4 连续开发批次验收报告（feature-m3m4，T1–T8）

- 范围：`7980565..5d4aafd`（8 个提交，用户离开期间的自主开发批次）
- 流程：每任务 实现 → Release+Debug 双配置零警告零错误 → 独立审查代理验收
  → 修复必修项 → 提交
- 总体结论：8/8 通过；4 项 APPROVED、4 项 APPROVED-WITH-FIXES（必修项全部
  当轮落地）。**全部为离线可验证内容；实机验证项见文末清单。**

## 任务与结论

| 任务 | 提交 | 内容 | 验收结论 |
|---|---|---|---|
| T1 自测加固 | 08ba601 | `DrResolveRead`/`Cr3ResolveRead` 纯函数抽取（行为逐位不变）；GuestGpr 16 索引往返；NPF U/S 用例；R8/R15 偏移 static_assert | APPROVED |
| T2 NPF hook 激活 | 9348138 | hook 页 DATA/EXEC 滑动状态机（原页 P\|RW\|NX ↔ 隐藏页 P\|X）；`NpfSetSlideHook` seam（mm 不依赖 modules）；`TlbInvlpgaLocal`；NptEnable 门修复（先 SetActiveView(0)）；两处自相矛盾的旧断言修复 | APPROVED-WITH-FIXES（并发注释更正 + 竞态登记 §十 + OwnsView 守卫 + 双重分配泄漏） |
| T3 CR3×种子表 | 8998f05 | 按映像名布防（`Cr3ImageMatch` 纯函数 + `LookupByImage` + 创建通知自动布防 + ProcessWatch 事件）；basename 存储修正；`cr3 watch/unwatch` 命令；**IOCTL_CR3_CONFIG/CR3_STATS 接线（原为死代码）** | APPROVED-WITH-FIXES（IOCTL 接线为必修项） |
| T4 MSRPM 三缓冲 | 21f183d | 双页 swap → 三页角色轮转：退役页两轮宽限（≥3 次背靠背 build 才可能命中滞后核，旧为 ≥2）；AppyAll 集成核验 | APPROVED-WITH-FIXES（staleness 注释量化更正：两轮而非一轮） |
| T5 同页多 hook | 4cb891f | footprint 两两不相交约束下补丁合成进共享隐藏页；按 hook 独立撤销（UndoPatch）；哈希键 pagePa→targetVa；页级资源恰一次回收 | APPROVED-WITH-FIXES（**Deinit 摘链后释放 + NextAll 遍历修复**，UAF/泄漏为必修项；含 Deinit-with-live-hooks 回归测试） |
| T6 文档/脚本 | 86f1b11 | ARCHITECTURE/MODULE_GUIDE 全面刷新（对照源码逐条核实）；run_tests.ps1 加 cr3 watch/unwatch + dbg events；CODE_REVIEW §十 登记 | APPROVED-WITH-FIXES（§十 交叉引用 + DR 事件措辞，纯文档） |
| T7 激活 ack | 6866689 | 视图发布完成后推 `SvmbDbgEvtViewActivate`（离线静默）；svmbctl dbg events 输出事件名 + extra | APPROVED |
| T8 独立状态码 | 5d4aafd | 重叠拒绝返回 `NPT_HOOK_STATUS_OVERLAP(0xC0DE0003)`，与不可解码目标的 NOT_SUPPORTED 区分；测试断言锐化 | 自包含小改（3 处断言/常量），随批次复核 |

## 审查代理抓到的关键缺陷（均已修复）

1. **T5 Deinit UAF/泄漏（最重要）**：节点未摘链即释放 → `CountPageHooks`
   遍历已释放内存（Verifier special pool 下必崩），共享页资源必然泄漏；
   叠加既有 `h->Next`（哈希桶链）误用，"页级资源恰一次回收"在多 hook 卸载
   时失效。修复 + 回归用例。
2. **T3 IOCTL 死代码**：`Cr3MonitorConfigure/Stats` 自 M4 骨架起从未被
   IOCTL 层调用——`cr3 watch` 对用户撒谎。已接线。
3. **T2 NptEnable 门**：`Enable()` 要求 Active_ 非空，原调用路径必返
   DEVICE_NOT_READY；两处旧自测断言与实现语义矛盾（若实机跑 selftest 会红）。

## 实机验证清单（依赖 VM，等待用户数据）

1. **前置（阻塞项）**：master 线 `start` 后 vCPU shutdown 根因——按
   tests/VM_SETUP.md 取 `exit trace` 日志尾部（SerialLog=1 串口或 WinDbg）。
2. `svmbctl selftest` 全绿（现含 T1–T5 新增约 60 条断言）。
3. `tests/run_tests.ps1` 全绿（含 cr3 watch/unwatch、dbg events 步骤）。
4. `NptEnable=1` 联调：NPT 上线 → `npt add` 装 hook → 执行流走补丁页（M4
   slide 实测）→ `dbg events` 出现 view 事件。
5. `mod attach debugger` + `DBG_CONFIG{HideDr=1}` → guest DR 读写落阴影；
   `cr3 watch <目标> <key>` → CR3 读伪造实测。
6. CODE_REVIEW §十 Live-NPT 前置项在启用 live hook IOCTL 前解决。
