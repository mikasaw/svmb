# M3 第一阶段骨架验收报告（feature-m3m4）

- 验收对象：`578735b..HEAD`（2072c33 骨架 + 14b3785 发布回调接线 + 6a36fbd 修复）
- 审查方式：独立审查代理，含完整重编译验证（Release 0 error/0 warning）
- 结论演变：**R1: APPROVED-WITH-FIXES（无 P0）→ 全部必修项已落地（6a36fbd）**

## 专项确认通过项

1. **start 路径零影响（本轮最重要验收目标）：成立。** `NptEnabled()==false` 时 ConfigureVmcb 的 else 分支与基线逐位一致；NPF handler 注册无条件但 NpEnable=0 时硬件不产生 NPF exit、即使假想触发也在首行 decline；HC_UPDATE_BARRIER 无既有调用方；`NptInstance()` 在 Start 时必非空（初始化失败即不创建设备）。
2. **fault 类语义**：NPF 属 fault 类（IsFaultClassExit），handler 返回 true 时 rip 不前进、指令重执行，与框架 Dispatch 一致。
3. **跨核 VMCB 写**：与 InterceptManager 同款"VMRUN 采样 + CleanBits 失效"论证，写序 NCr3→TlbControl→CleanBits 由 x86 store order 保证；惰性"从无到有"映射无需 INVLPGA（APM：导致 NPF 的 walk 不缓存）。
4. **槽管理/回调生命周期**：id 单调不复用、活跃视图销毁拒绝、回调随对象消亡、Unload 持门禁且先 Stop 后 teardown。

## Findings 与处置

| 编号 | 级别 | 问题 | 处置 |
|---|---|---|---|
| R1-1 | P1 | `_svmb_invlpga` asm 丢弃 ASID 参数（ECX 残留 GVA 低 32 位被当 ASID） | asm 改为 eax←gva、ecx←asid（svm_entry.asm） |
| R1-2 | P1 | `npf.denywrite` 自测在 RAM 预填充视图上必然失败（MapRange 不降低已存在表项，RO 被丢弃） | 分类测试改用新建视图（确定性），并补大页 NOT_SPLIT→Unhandled 用例 |
| R1-3 | P2 | NptManager::Deinit 未随注册表扩展（槽不清理、Active_/Enabled_ 残留 → 门控不变量破坏，重 Init 永久失败） | Deinit 对称清理 |
| R1-4 | P2 | 发布回调不受 NptEnabled 门控（NPT 关时向活跃 VMCB 写非零 NCr3） | SetActiveView 门控化；Enable() 补发布 |
| R1-5 | P2 | 激活协议缺失：发布/VMRUN 采样/ack 无协议定义 | 激活协议三条硬性前提写入 mm/npt.h（门禁持有、发布序、ack 收集留 M4） |
| R1-6 | P2 | NPF 惰性映射多核竞态（上层表项无锁 RMW） | 并发硬前提写入 mm/npf.h（激活前必须单写者或加每视图锁） |
| R1-7 | P3 | lazy-map 失败路径不限流；deny 计数终身不重置 | 失败路径纳入限流；计数导出/重置留作 GET_INFO 扩展项 |
| R1-8 | P3 | hypercall 注释截断；a1 无语义 | 注释修复，a1 声明保留为 sequence cookie（M4 ack 协议接口） |
| R1-9 | P3 | tlb.h "next configure" 措辞不准（运行核经 CleanBits 于 next VMRUN 采样） | 措辞修正 + 发布方契约引用 |
| R1-10 | P3 | 自测缺 Enable-无视图/未知 id/槽耗尽/回调连通性 用例 | 全部补齐（含回调门控断言） |
| R1-11 | P3 | vcpu.h `ActiveNCr3` 字段无人维护；NPT_ASID/TLB_CTL_DO_NOTHING 死常量 | 保留为协议常量并登记；ActiveNCr3 待激活路径接线（M4 条目） |

## 遗留（转为 M3/M4 联调条目）

1. R1-5/R1-6 的协议与并发前提在 `Enable()` 上线（tier-3）前必须实现：per-view 写锁或单写者约束、ack 收集通道。
2. `vcpu.h ActiveNCr3` 接线到发布路径。
3. guest #PF 通道与 NPF 的边界（user 态读内核叶分类目前落 Unhandled，tier-3 需补 U/S 位）。

**结论：代码级验收通过（APPROVED，附上述联调条目）。** start 排障线不受本分支影响。
