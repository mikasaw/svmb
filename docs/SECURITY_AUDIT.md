# svmb 安全审计（r85）

审计基线：r84 @ f20fb69 工作区 + r85 修复。审计执行：独立子代理
（只读），修复与本文档由主代理完成。威胁模型：TM1 = 非特权本地进程
（含低完整性）；TM2 = 管理员/SYSTEM。

## 总体结论

**无 P0**。信任边界的门成立：设备 SDDL
（`SDDL_DEVOBJ_SYS_ALL_ADM_ALL`，仅 SYSTEM/Administrators 可打开）、
16 个 IOCTL 全 METHOD_BUFFERED + 逐 case 长度校验、IOCTL 全局门锁与
卸载互斥、服务键默认 ACL 挡住 TM1 改旋钮、hypercall CPL 门。TM1 →
内核提权路径未发现。产品化风险集中在三件事：

1. Release 未摘除测试武器库（P1-1，**已修**）
2. fail-fast 被日常路径引爆（P1-2，**已修**）
3. region 保护对换页不设防（P1-3，**部分缓解，列为 r86 首项**）

## 发现与处置

### P1-1 Release 未编译掉测试/攻击性 IOCTL — **已修复**

CR3_POISON（腐化哨兵水印）、CR3_PROBE（内核 MDL 写任意进程页 = 官方
L1 旁路后门）、NPT_HOOK/NPT_PROBE（任意内核 VA 钩子 / 全系统文件打开
拒绝引擎）、STRESS 在 SVMB_PRODUCTION 下全部存活——单次管理员 IOCTL
即得内核任意代码执行。

**修复**：main.cpp DevCtrl 顶部 SVMB_PRODUCTION 门控，上述 5 个 IOCTL
一律 `STATUS_NOT_SUPPORTED`。实测：poison → err 50 拒绝；probee2e 的
probe 步拒绝；blockself/regions/stats 不受影响。

### P1-2 未认领 NPF deny → Release 整机 0xE2 — **已修复**

hook 页 deny（flip-flood / stale shadow）等日常路径可产生"无消费者
认领"的 deny → Release 直接 0xE2 bugcheck（两次在案蓝屏）。r76 修复
只覆盖区域/哨兵消费者，hook 路径仍是窗口；且 watch 活动时 orphan
分支会静默吞掉钩子 deny（钩子完整性失效）。

**修复**：npf.cpp 移除 SVMB_PRODUCTION 分叉——两构建统一走 r45 语义
（resolve，不跳指令，页重新真实执行），日志响告。**政策变更声明**：
这推翻了 r36/r73 的"错模型即死"取舍；依据是 r76/r85 实证——消费者
契约存在未知缺口时，整机死亡不是可接受的失败模式。deny 消费者契约
仍是第一道防线，此处为兜底网。

### P1-3 region 页未锁定：换页后 deny 流放到无辜帧 — **已修复（r86）**

REGISTER 用 MmGetPhysicalAddress 采样帧但**不锁页**：帧被 OS 正常换
页回收复用后，re-arm DPC 按 GPA 盲目 re-deny 会把活陷阱留在无辜帧上
（每 100ms 一轮 NPF/日志/事件洪泛 + 假 RegionTrip 洗掉真告警）；同时
保护静默降级为仅 L1。死亡清扫只覆盖进程退出，不覆盖工作集修剪。

**处置（r86 已实现）**：REGISTER 时 IoAllocateMdl +
MmProbeAndLockPages(UserMode, IoReadAccess) 钉住区域帧（PFN 锁阻止
换出与复用），MDL 存槽、UNPROTECT/死亡清扫/卸载统一在 guest-MM 与
NPT 恢复完成后解锁。GPA 直接取 MDL PFN 数组（钉帧与 deny 按构造一
致，消除 WRITECOPY COW 偏斜窗）。行为变更：未命中页从拒绝变为
fault-in 后接受。钉页上限 32×64 页 = 8MB。注意：宽限环容量（P3-17）
不随本修复关闭——teardown 竞态窗与帧复用是两个独立问题。

### P2 项（已修）

| # | 内容 | 修复 |
|---|---|---|
| P2-5 | HV_CONTROL cycle 无上限（每轮设计性泄漏 vCPU 页） | RepeatCount 封顶 16 |
| P2-6 | IOCTL start = round-27 已知致命进入上下文 | SVMB_PRODUCTION 拒绝 action 0/2 |
| P2-8 | death sweep 与 REGISTER 槽复用竞态（恢复参数被新 region 覆盖 → 新区域 L1 保护被误还原） | UnprotectSlot 先快照后清槽（含 NPT 恢复循环） |
| P2-9 | IsKernelVa 收 non-canonical → 单 IOCTL 蓝屏 | 改 canonical 内核区段判定 |
| P2-10 | deny-cb 日志 cap 失效（比较预增值）+ 洪泛 | 改用后增值封顶 |

### P2 已知风险（测试平台接受，产品化前重评）

- **P2-4** 全核 CPUID 拦截 = TM1 非特权 DoS 放大 + HC_PROBE/HC_VERSION
  在场/版本预言机。薄 hypervisor 固有代价；产品化前评估 leaf 区间
  收窄/伪装应答。

### P3 清单（随重构顺带或观测）

- P3-11 seed Graveyard 无界（~48B/进程，长周期池消耗）→ 加界
- P3-12 HashTable pid 键桶倾斜（key>>12）→ seed 表改 key%N
- P3-13 offsets 扫描残余（MmIsAddressValid TOCTOU、并发 resolver 撕裂
  窗口窄、g_offsetsOk 非屏障）→ 已按 r83 记录观测
- P3-14 哨兵 watermark 两处裸读 ObjVa → 顺带加 MmIsAddressValid
- P3-15 CR3_PROT Size 未强制页对齐（Pages 截断 vs SizeBytes 判定区间
  不一致）、Base 未显式查 user-VA
- P3-16 HookId 复用 × 在飞 stub 身份混淆 → Remove 时 drain
- P3-17 宽限环 256 项固定（≥5 满区域同窗 teardown 驱逐）→ 并入 P1-3
- P3-18 卸载守卫 'SVMB' bugcheck = 有意策略（保留）；SetDefaultPolicy
  无调用方（死代码，注释明确）

## 上线前必须修清单（更新）

1. ~~P1-1 测试钩子编译掉~~（本轮）
2. ~~P1-2 fail-fast 日常可达~~（本轮）
3. ~~P1-3 region 帧身份防护~~（r86 已修复，MDL 钉页）
4. 微软 attestation 签名（外部流程）
5. Windows 版本自适应（r83 已建机制，需在更多 build 上验证扫描）
6. 安全监控：NonPaged 池（Graveyard）、RegionTrips 假阳率

## r87 增记：CR3_READVM（0x825）能力登记

新增只读披露原语：seed 表 CR3 → 物理读页表遍历 → MmCopyMemory 搬运。
**不在 production IOCTL 拒绝清单（有意）**：与 P1-1 移除的测试武器
（任意写 / 代码执行类）性质不同——READVM 是只读能力，其门即设备
SDDL（SYS/Administrators），持句柄者就是信任根本身（见
docs/HYPERVISOR_MEM_READ.md §4.1/§5）。加固已含：路径级 MMIO 门
（WalkVa 每级页表帧 + 数据 GPA 均过 PhysRanges::IsRam）、缓冲双重
校验（IOCTL 层 + 模块层）、Size≤64K、无任何 guest VA 解引用。
信任根声明更新：本能力上线后，驱动被攻陷等同于整个防护体系被攻陷
（含"上帝视角读"），文档 §5 双用途声明适用。

## r94 增量审计：r89-r92 攻击面（kill / suspend / stealth / READVM+GUESTVIEW / RDTSC）

**判决：CLEAN——无 P0/P1。** TM1 复核：SDDL 线下无新提权路径；四个新
IOCTL（0x825/0x826/0x827/0x828）全部 METHOD_BUFFERED + 逐 case 长度校
验；r85 门禁（SDDL、缓冲校验、门锁、注册表 ACL、hypercall CPL 门）对
新攻击面全部继续成立。

### P2（生产前应修 → r96 硬化轮）

| # | 发现 | 修法 |
|---|---|---|
| ST1 | stealth 模块无生产闸门：`mod attach stealth` 在 SVMB_PRODUCTION 下可隐藏 hypervisor 存在性（对 EDR/attestation 的对抗能力不应在生产裸露） | PRODUCTION 构建下 Attach 对 "stealth" 响亮拒绝，或注册表显式 opt-in 旋钮 |
| R1 | CR3_READVM 输出缓冲调用者不配合时（demand-zero 页）完成复制内核触碰自旋且持 gIoGate = 全机楔死（r89 律），一次普通 API 误用即触发 | 走查前 MmProbeAndLockPages(UserMode, IoWriteAccess) 预探针输出区间（mem_read FaultInPages 同款），调用者配合去前提化 |

### P3 加固注记（11 项）

K1 归因可借 CR3 转嫁（取证误伤非提权；响应限定 writer_pid==region.Pid）；
K2 pid<4 守卫未盖 System(=4)（今日靠未被 seed 偶然保护；改 pid<=4 +
kDeny 增 system/registry/memcompression）；K3 拒绝名单映像冒名仅降级
响应（kill 时活 EPROCESS 复核映像名）；K4 suspend-null-pfn 落 KILL 分
支（else if 显式化）；K5 kill workitem 缺失时 REGISTER 不拒（镜像
SUSPEND 响亮拒绝）；S1 表满溢出冻结者卸载后搁浅（挂起前预检槽位）；
S2 无门快路径绕过拒绝过滤器位置（检查上提 DevCtrl）；S3 SVMB_SUSP.Op
被忽略（ABI 冗余无害）；ST2 CPUID 链同叶重叠无仲裁（当前不相交，未来
模块注记）；ST3 demo_cpuid 存悬垂 api 指针（改拷贝）；R2 帧复用 TOCTOU
=信任根取证假信心（走查后复核 pid+cr3，死目标打 OutFlags）；R3
ReadVmEnable 生产默认应 0。

### Production 拒绝清单复核

READVM（只读 + SDDL/ReadVmEnable 双闸门 + 信任根声明在案）、SUSP 对
（驱动自施冻结的控制平面，加门=设计性死锁）、RDTSC（平凡）不入清单
=可辩护。唯一缺口是结构性位置问题（S2）。

### 流程注记

r94 与 r93 四小时混合浸泡并行执行（审计子代理全程宿主只读，未触碰
guest）；r94 无代码改动、无独立提交；P2 修复排 r96 硬化轮（浸泡结束后
部署验证）。
