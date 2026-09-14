# 基于 svmb 虚拟化框架读取进程内存 — 可行性评估

状态：**已实现（r87 策略 A / r88 策略 B + 加固 + 离线单测 / r89
GUESTVIEW + ReadVmEnable 旋钮）**。
IOCTL 0x825（SVMB_CR3_READVM）+ ctl
`cr3 readvm [faultin] [guestview] / readvm-self / readvm-hole / readvme2e`，
设计要点 3.1-3.3 落地：策略 A 零痕（present=0 零填充 + F_PARTIAL）；策略 B
`InFlags=SVMB_READVM_F_FAULTIN` MDL probe-lock 换入（有痕，SEH 包裹）；
GUESTVIEW（r89）：NPT-hooked 页读 guest 观察的补丁副本（当前 hook 仅
内核页而 readvm 限用户页，真值 vs 视图分歧 E2E 待用户页 hook 出现）。
加固：目标 VA 限 user 区（非 canonical 一并拒绝）、u64 回绕拒绝、未知
flags 拒绝；walk 每级页表帧过 PhysRanges::IsRam（MMIO 不碰）。
E2E：self MATCH / 跨进程活进程字节精确 / demand-zero 页 A=PARTIAL vs
B=resolved / 保护区读零 trip（tests/CRASH_DEBUG_LOG.md Round 87/88/89）。
以下为原始评估，保留存档。

结论先行：**可行，且大部分构件已
存在，预计一个开发轮（~250 行驱动 + IOCTL + ctl 命令）可落地。**

## 1. 目标定义

在 svmb（L1 guest 内核驱动，本身运行在被虚拟化的环境中）内实现：
**读取任意进程的虚拟内存，不 attach 进程、不开句柄、不走任何
guest 内核 API——对 guest 操作系统与目标进程完全不可见。**

## 2. 为什么可行：三个已存在的构件

### 2.1 物理内存的全局视图

svmb 的 NPT 把全部 RAM identity 映射（r12 起，2M RWX 大页 + 懒映射）。
驱动虽运行在 guest 内，但物理内存对它而言是可平铺访问的资源：
`MmCopyMemory`（MmPhysicalAddress 模式）可按物理地址直读 RAM，无需
任何 guest VA 映射。

### 2.2 进程 CR3 已在手（seed 表）

r11 起的 Cr3Seed 表在进程创建通知时即抓取每个进程的
DirectoryTableBase（r83 后从运行时解析的 DTB 偏移读取，全 build 适
用）。目标进程 CR3 = 页表遍历的根，**已经免费持有**。

### 2.3 读取不产生任何虚拟化事件

默认 NPT 视图对 RAM 是 RWX：**读操作不产生 NPF exit**（只有 W-deny /
X-deny 才触发）。也就是说，读取过程在 hypervisor 的事件通道上也是
"静默"的——不存在日志、不存在 trip、不存在性能扰动。

## 3. 实现设计（r87 草案）

### 3.1 VA→GPA 手工页表遍历

```
输入：目标 CR3（seed 查询 pid）、目标 VA
CR3 & ~0xFFF          → PML4 物理基址
MmCopyMemory 读 PML4E = PML4[ (va>>39)&0x1FF ]     (物理读 8B)
逐级：PDPTE → PDE → PTE
分支：
  PDE.PS=1  → 2M 大页：GPA = (PDE & 0x000FFFFFFFFFF000) | (va & 0x1FFFFF)
  PTE     → 4K 页：GPA = (PTE & 0x000FFFFFFFFFF000) | (va & 0xFFF)
任一级 Present=0 → 该 VA 当前不在物理内存（见 3.3）
```

### 3.2 物理读

`MmCopyMemory(dst, {MmPhysicalAddress, gpa}, len, MM_COPY_MEMORY_PHYSICAL)`
——按 4K 边界分块（页表项换算以页为单位），一次 IOCTL 最多搬运
一页，多次调用遍历任意长度。

### 3.3 未命中页（present=0）策略

| 策略 | 行为 | 适用 |
|---|---|---|
| A（默认） | 返回"页不在物理内存"，跳过 | 反作弊/监控场景：只读常驻帧，**零扰动**（不触发换页、不给目标任何可观察信号） |
| B（可选开关） | attach 目标进程 + 触碰页面使其换入，再读 | 完整性优先；有痕迹（工作集变动） |

### 3.4 IOCTL 与 ctl

```
SVMB_IOCTL_CR3_READVM (0x825)
{ Pid, Va, Size, OutBuf } → 按 4K 分块填充，OutResolved 记录实际可读
字节数（present 页部分）
ctl: svmbctl cr3 readvm <pid> <va-hex> <size>
```

## 4. 与现有机制的关系

### 4.1 对本框架自身保护（区域/哨兵）——读是"上帝视角"

- 读**不触发** W-deny（区域 L2 传感只对写敏感），哨兵同理；
- 即：**hypervisor 层读取完全绕过本项目自建的两层防护**。这不是
  缺陷而是信任根定义：防护面向 guest 内攻击者；持设备句柄的驱动
  使用者就是信任根本身。审计（SECURITY_AUDIT.md）中设备 SDDL 把
  该能力限制在管理员。

### 4.2 与页隐藏（ProcView/hook 翻转）的交互

svmb 的页隐藏在 **NPT 视图层**实现（不同视图=不同 PML4 页表）。物理
读拿到的 GPA 经 guest 页表遍历得出，对应目标进程视角的真实数据帧；
若某页被 hook 翻转（GPA→替身 SPA），物理读读到的是替身内容——
**读"真相"需查 npt_hook_mgr 的 original-page 记录**。实现时按
"hook 记录优先，无记录直读"的顺序处理。

### 4.3 与 r76 探针的区别

r76 PROBE 是**写**路径的 L2 传感测试工具（需先有区域+故意触发
W-deny）；本特性是**读**路径的常规能力（无 NPF、无传感事件、无
区域依赖），二者互不影响。

## 5. 风险与双用途声明

隐蔽读取进程内存是反作弊/EDR/取证领域的标准原语，同时也是典型
rootkit 原语。本仓库的约束：
- 能力入口受设备 SDDL 限制（仅 SYSTEM/Administrators）；
- 用途限定为研究/测试平台（对自身防护体系的上帝视角验证、以及
  后续"检测不可见读取"的防御研究）；
- 文档明示：该能力一旦实现，**本驱动自身即为其所保护系统的最高
  信任根**——驱动被攻陷等同于整个防护体系被攻陷。

## 6. 工作量与验收

| 项 | 估计 |
|---|---|
| 页表遍历器（4K/2M 分支、物理读封装） | ~250 行 |
| IOCTL 0x825 + ctl `cr3 readvm` | ~120 行 |
| 验收 | 读 notepad 已知内容 vs ReadProcessMemory 对照；读区域保护页验证绕过；watch 场景下零 trip 扰动验证 |
| 轮次 | r87 一轮 |
