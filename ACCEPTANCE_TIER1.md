# Tier-1 离线功能验收报告

- 验收对象：dev 分支 4522319..57cab3f（mm/ 物理内存、NPT 视图、指令解码、npt_hook_mgr、cr3_seed、MSRPM 影子、dbg_events、IOCTL 接线、svmbctl 命令、docs、run_tests.ps1）
- 验收方式：独立审查代理对 `master...dev` 全量 diff 逐文件审查（正确性/APM 语义/生命周期/并发/自测覆盖/master 合并兼容性），产出 REJECTED 结论与必修清单；随后逐项修复并复验编译。
- 构建：Release + Debug 双配置零错误零警告。

## 审查结论演变

| 轮次 | 结论 | 说明 |
|---|---|---|
| R1（审查代理） | REJECTED | 2×P0、1×P1、6×P2、5×P3 |
| R2（修复后） | 代码级闭环 | 全部 P0/P1/P2 已修复；P3 中可落地项一并处理 |

## R1 发现与处置

| 编号 | 级别 | 问题 | 处置 |
|---|---|---|---|
| R1-1 | P0 | 解码器 E8/E9（call/jmp rel32）、EB（jmp rel8）落入 in/out dx 桶返回 0，合法指令被拒/错解码；自带 `insn.call_rel32` 断言必失败 | E8/E9→F_IMMV（66→rel16）、EB→F_IMM8、EA→F_BAD；已修 |
| R1-2 | P0 | F_IMMV 尺寸 `rexW?8:...` 对 0x68/0x69/0x81/0xC7/0xF7/0F 8x 等错误加宽；`48 81/48 C7` prologue 高频指令长度错 | 仅 B8-BF/A0-A3 允许 REX.W 提升为 imm64；补 7/8 字节样本断言 |
| R1-3 | P1 | DriverEntry 中 gCr3Seed->Init 成功后的任一失败路径直接 return，进程通知回调指向将被卸载的镜像 → 卸载后进程创建即崩 | 统一 `OfflineTeardown()`（顺序：cr3_seed → hooks → npt → ring），所有失败路径调用 |
| R1-4 | P2 | Split2M 对未映射 PDE 返回 SUCCESS，与头文件契约矛盾 | 补 P 位检查，返回 NPT_STATUS_NOT_MAPPED |
| R1-5 | P2 | RegisterPage OOM 只打日志：表页失去 VA 解析 → INVALID_PARAMETER + 永久泄漏 | RegisterPage 返回 NTSTATUS，全部 5 个调用点 OOM 回滚刚分配页 |
| R1-6 | P2 | Cr3Seed::Lookup 在哈希锁外读节点，并发终止路径可 UAF | 终止节点进入 Graveyard_（Deinit 统一释放），运行期节点内存永有效 |
| R1-7 | P2 | MODE_CC 跳板无条件拷 16 字节，目标距页尾 <16B 越页读 | clamp 到 pageRemain |
| R1-8 | P2 | IOCTL/Unload 持 FAST_MUTEX（APC_LEVEL），SELFTEST/Unload 调 PASSIVE-only API（MmGetPhysicalMemoryRanges 等）违反 IRQL 契约 | 换 PASSIVE 睡眠锁（CAS 标志 + 自动重置 KEVENT，纯 ntddk）；等待与持有全程 PASSIVE |
| R1-9 | P2 | 协议定义了 0x820（CR3_CONFIG）但驱动无 case | 补 validate-only case（M4 cr3_monitor 接管），与 0x830 对称 |
| R1-10 | P2 | 0F 二字节表：0F 05/06/08/09/0B/0E/30-37/C8-CF 无操作数指令被加幻影 ModRM；0F 70-73 死代码 | Classify2 重构：显式 no-operand 单例、70-73 移出死分支、77 emms、C8-CF bswap |
| R1-11 | P3 | SELFTEST IOCTL 失败时返回错误码，R3 丢细节 | 恒返 SUCCESS，结果经 res.Failed/LastFail 表达 |
| R1-12 | P3 | phys_mem RamEnd_ 取末元素而非最大值 | 改 max |
| R1-13 | P3 | CD int imm8（2B）、CA retf imm16（3B）、D7 xlat（1B）解码错 | 分别 F_IMM8/F_IMM16/0 |
| R1-14 | P3 | 自测缺 48 81/48 C7 样本、Split2M 未映射负例、大页上 SwapPage 负例、环双圈回绕 | 全部补齐 |
| R1-15 | P3 | ring Deinit 不清 Lost_（无害）、MsRpm 三缓冲才完全闭合 P2-4 残余窗口、hook RefSplit 返回值忽略 | 记录为已知限制；残余窗口为 fail-open 方向且窗口极小，按计划并入 M3 |

## 已核对正确的主体（审查确认）

NPT 四级索引与 PTE 位域符合 APM；2M→4K 拆分镜像（perm 位携带）；MapRange 边界块语义（部分覆盖只映射覆盖 4K，绝不映射 RAM 范围外页）；Deinit 树遍历释放无泄漏无重复；原始物理页在 hook 安装/摘除全程零修改；跳板/隐藏页分配释放配对；同页拒绝二次 hook；cr3_seed 回调签名（PEPROCESS）与创建/终止对称；MSRPM 影子翻转/清空语义；dbg ring FIFO/丢失计数；池指针全局（规避内核 atexit）；与 master 崩溃修复为纯 fast-forward 关系、冲突面仅 intercept_manager.cpp ApplyAll 与 main.cpp 增量。

## 遗留（不在本批范围）

1. **实机验证**：`svmbctl selftest` + `tests/run_tests.ps1` 必须在测试 VM（驱动已签名加载）执行至全绿——本批仅完成静态与编译级验证，R1 指出"自测从未实际运行"的教训即在于此。
2. P2-4 完全闭合需三缓冲（M3 与 NPT 视图原子发布一并做）。
3. 多 hook 共页、hook 激活（NPF exit）、CR3/DR/MTF handler 属第三档，接口已预留。

**结论：代码级验收通过（APPROVED-WITH-VERIFICATION-PENDING）——合并/联调前必须完成 VM 内自测全绿。**
