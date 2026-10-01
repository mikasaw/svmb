# M4 连续开发批次二验收报告（feature-m3m4，T9–T13）

- 范围：`0e3965d..HEAD`（用户离开期间的第二批自主开发，5 任务 6 提交）
- 流程：每任务 实现 → Release+Debug 双配置零警告零错误 → 独立审查代理验收
  → 修复必修项 → 提交
- 总体结论：5/5 通过（4×APPROVED-WITH-FIXES 已修复 + 1 文档批）。
  **全部为离线可验证内容；两个审查代理各自抓到一个会导致挂死/状态损坏的
  真实缺陷，均在提交前修复。**

## 任务与结论

| 任务 | 提交 | 内容 | 验收结论 |
|---|---|---|---|
| T9 hook graveyard | 182a4a5 | Remove 不再释放节点/隐藏页/跳板——进有界 graveyard（32 上限，超限逐出最老），关闭 CODE_REVIEW §十-1 退出路径 UAF；stale 状态语义（exec→良性原字节页 / write→NX 页 DenyExecute 自旋）注释化；Deinit 双链回收 + HiddenShared 防共享页双释放；HashTable 契约注释更新 | APPROVED-WITH-FIXES（stale-flip 论证失真修正，注释级） |
| T10 每视图变异锁 | 873e1e7 | NptView 四个变更入口持 per-view 自旋锁；读路径无锁论证（原子读 + publish-once）；激活协议 §3 单写者前置项关闭 | APPROVED-WITH-FIXES（**MapRange→Split2M 递归取锁死锁必修**，Split2MLocked 抽取 + 未初始化视图硬ening） |
| T11 进程视图切换 | d19185d | `EnableProcessView` 实装：`NptManager::FillView` 任意视图填充；CR3 写出口按 `Cr3ViewSwitchDecision` 纯函数进出目标进程时切视图；ViewSwitchCount 入 stats | APPROVED-WITH-FIXES（**自测空引用蓝屏必修** + exit 发布者协议例外文档化 + 全局发布 vs 每核保真登记为 M4 前置 §十-5） |
| T12 IOPM 引用计数 | 8d2441d | 无调用方的布尔 API 重塑为 `RequirePort/ReleasePort` 按方向引用计数（0→1 置位/1→0 清位），validate-first 过度释放拒绝，节点双向归零回收 | APPROVED-WITH-FIXES（**混合方向过度释放回滚损坏状态必修**，改为 validate-first + 区分性/相邻位测试） |
| T13 文档收尾 | 本提交 | CODE_REVIEW §十 处置更新（2 项关闭、2 项收窄保持开放、1 项新登记）、ARCHITECTURE 同步 | 见下 |

## 审查代理抓到的关键缺陷（均已修复）

1. **T10 递归取锁死锁**：MapRange 持锁后对已存在大页做部分覆盖时调用
   `Split2M` → 同一自旋锁递归获取永久自旋；现有自测 `npt.boundary.map`
   即可必现挂死。修复：`Split2MLocked` 私有方法。
2. **T12 过度释放回滚破坏状态**：混合方向释放时对未递减方向凭空 `++` 计数、
   已清位不复位——后续 require 静默失效（拦截位缺失）。修复：validate-first。
3. **T11 自测空引用蓝屏**：`*m.View(999)` 空引用在 selftest IOCTL 中直接
   BSOD。修复：改用未初始化局部视图验证拒绝路径。
4. **T11 协议级发现**：exit 上下文发布视图违反激活协议第 1 条（持门要求）——
   内存安全核查通过后登记为「特许例外」，并把全局发布 vs 每核视图保真
   登记为 M4 藏页前置条件（§十-5）。

## 离线自测增量

grave（10 项）、shpage 既有回归、cr3vs（6）、fillview（7）、iopm（24）——
`svmbctl selftest` 总断言数已过百。

## 实机验证清单（依赖 VM，等待用户数据；继承批次一清单并追加）

7. `NptEnable=1` + `cr3 watch <目标> key processview`：进出目标进程时
   `dbg events`/stats 出现 view 切换（ViewSwitchCount 增长）。
8. hook 装卸循环后驱动卸载无池损坏（graveyard/共享页回收路径）。
