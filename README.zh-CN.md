# svmb — AMD-V 嵌套 Hypervisor 研究驱动

[English](README.md) | 简体中文

Windows 内核驱动：加载即把所有 CPU 核心置入 AMD-V (SVM) 虚拟化，在此基础上实现
NPT 内存虚拟化、NPT 页权限钩子、CR3 保护套件（哨兵感知 / per-core 进程视图隔离 /
CR3 读伪造）、X-deny 系统调用感知与跨进程行为分析、进程内存块加解密（双帧分视）、
崩溃取证工具链——以及一条嵌套虚拟化研究线：承载 guest hypervisor（L2），含 per-core
影子两级 NPT 分页、VMLOAD/VMSAVE/INVLPGA 仿真、虚拟化行为记录器（vlog）与嵌套
中断投递取证。

**当前状态：研究/测试平台可用，未达生产上线标准。**

> **免责声明** — 这是一个教学研究项目。它是加载即虚拟化所在机器的内核驱动：
> 蓝屏、虚拟机楔死等故障是常态。请只在可丢弃的虚拟机中运行。不要在任何你
> 不拥有的机器上使用它或其衍生品，也不要用于任何恶意用途。

## 功能面

| 能力 | 说明 | 状态（vhv / QEMU TCG 双床） |
|---|---|---|
| 全核虚拟化 | DriverEntry 即虚拟化 6/6 核（AutoStart=1）；干净单相卸载 | ✅ 稳定（多轮 10min+ 浸泡、风暴 13.3T exits） |
| NPT 身份映射 | [0, RamEnd) 2M RWX 大页含 MMIO 洞 | ✅ 稳定（裸 NPT 小时级） |
| NPT 钩子框架 | slide 执行/数据翻转让、stub/回调层、stole-prefix ASM detour | ✅（NtCreateFile 等真内核钩子全绿） |
| CR3 哨兵感知 | watched 进程线程对象页 NPT W-deny → 每次调度写一个 trip + 方向位 | ✅ 机制全通（670 trips / 43×out=1 实证） |
| 区域写保护 | `cr3 protect/unprotectid/holdpage/regions`：L1 guest-MM 阻断 + L2 NPT 传感双层；内核 MDL 旁路探针 `probee2e`；shadow-kick（NCr3 scratch wiggle）使传感确定性单 exit；进程死亡自动清扫 | ✅ probee2e 30/30 + multitarget 全链路 + 2h 混合浸泡 48/48 |
| per-core 进程视图 | 目标进程 current 时该核发布 ProcView NCr3（策略页藏匿基础） | ✅ 机制全通；**仅 TlbMode=0** |
| CR3 读伪造 | XOR key 读伪装 | ⚠️ **vhv 平台不可用**（嵌套 CR3-read 拦截不透传）——裸机特性 |
| 系统调用/执行感知（sysaudit） | 对 MmCopyVirtualMemory、NtCreateFile、NtOpenProcess、NtReadVirtualMemory、NtWriteVirtualMemory 所在页做 NPT X-deny（SSDT+SSN 锚定、多锚点校验）；入口取指 NPF 记录调用方 pid/映像 + 参数；页重开后原指令原生重执行；250ms 定时器再武装。**零 guest 字节修改**——构造上对 PatchGuard 不可见 | ✅ E2E（真实 NtCreateFile/NTRD 条目含 pid 归因，零丢失） |
| 行为分析 | 1 秒窗口 per-(caller,target) 跨进程调用速率；边沿触发告警；阈值走注册表旋钮 | ✅ 实时运行（RD/WR/OP 三表） |
| 告警响应 | 可逆挂起违规进程（默认**关闭**，BehaveResponse）、拒绝列表、终止时 live-EPROCESS 映像复核（K3） | ✅ E2E（kill=1/suspend=1，4h 双政策浸泡 395 轮） |
| 内存块加解密（`crypt`） | 双帧分视：块内每页在目标自己的 NPT 视图上映射私有明文副本，**原帧**存 XOR 密钥流密文——目标持续读写明文，其他一切观察者（物理走读、扫描器）看到密文；退出代际门后的两阶段 seal（过渡期目标冻结）；免密钥解密 + 卸载期自动复原 | ✅ E2E（多块全周期 + hold + 活体目标穿越卸载复原，零坏字节读；离线套件 347/0） |
| 崩溃取证 | 面包屑飞行记录器（无锁环，实时 IOCTL + 冻结内存离线解码）、kdump 辅助 | ✅（存活与冻结 guest 均成功解码） |
| 嵌套虚拟化（L2 会话） | 外部 H 注册面（`hreg`：EFER 双视图、自有虚拟 HSAVE、注册者 CPUID SVM 解蔽）、反嵌套 `ic2.VMRUN` 默认武装且以干净 #BP 拒绝、L2 全回路（H 构建 L2 世界：页表 + 拦 CPUID 的 VMCB；控制权返回并验证反射出口）、L2 的 VMLOAD/VMSAVE 仿真 | ✅ 阶梯全绿（s0–s3 + 嵌套套件，QEMU TCG 床） |
| 影子两级 NPT（影子分页） | 在 svmb 自身 NPT 之上构建 per-core 影子会话翻译 L2 GPA：INVLPGA 范围失效合同、权限透传、2M 大页、窗外钩槽物化、跨核池变更自核化律；多核裁判（4 个 H 世界 × 4 核并发回路） | ✅ 端到端（r184；机制回归 11/11） |
| vlog——第三方 H 行为记录器 | 六类记录 guest hypervisor 的真实行为（反射出口含解码、INVLPGA、EFER/SVM MSR 写、…），无锁环，`vlog stats/dump/mask/reset` | ✅ |
| 嵌套投递取证 | 投递路径六门研究 + xAPIC MMIO IRR 仪器点名凶器中断序列；判决：QEMU TCG 嵌套投递两件套缺陷（异常帧压栈损坏 + 投递丢失）——svmb 侧设计空间以证据穷尽 | 📋 已记录（r190–r201） |
| 模块系统 | debugger / cr3_monitor / sysaudit，函数表 API | ✅ |
| 控制工具 | `svmbctl`：info/start/stop/storm/cr3 */exitprof/dbg */sysaudit/kdump/crumbs | ✅ |

## 成熟度与已知限制

- **Release 生产轨道已开**（r73+）：Release/`SVMB_PRODUCTION` 构建 + 双测试签名
  + 回归全绿。NPF fail-fast（0xE2 'SVMC'）是设计行为——NPT deny 必须被
  已注册消费者接住。
- **armed-watch 在 TlbMode=0 下未完全收敛**：成熟 boot 上 45min 浸泡存活（r64），
  冷启动早期武装仍会急性楔死（r65）。attached churn 下的通用冻结在积极调查中
  （r104–r108：已用面包屑记录器把死点细化到跳级粒度；根因尚未修复）。
  裸 NPT 与区域保护在两种 TLB 模式下均稳定。
- **TlbMode 语义**：`0` = 每 VMRUN 全量刷 TLB（默认，armed 操作必需——
  vhv 嵌套 TLB 弱点的承重墙补偿）；`1` = 从不刷新（仅裸 NPT 场景）。
- **采样语义**（设计使然）：250ms 开窗内或缓存 TLB 翻译之后的调用不会被感知；
  紧凑进程内循环可以滑过再武装窗口。
- **mem_crypt 视图同步契约**（r133）：per-core NCr3 只在 VM exit 时重发布，
  受保护目标必须在块访问点取一次被拦 CPUID（同步点，见
  `tests/mcrypt_target.c`）；无 exit 纪元若迁移到仍持有陈旧 base NCr3 的核，
  读到的是原帧——seal 后即密文。结构性修复（per-core view selection）为
  后续工作（`npt.h` M4 prerequisite）。U/S=0 base 视图读陷阱方案已实测并
  否决：内核自己的工作集/压缩触碰会在数秒内拆掉它。
- **嵌套线在 QEMU TCG 床上验证**（r139–r201）：TCG 嵌套中断投递缝是已证实的
  QEMU 两件套缺陷（异常帧压栈损坏 + 投递丢失，凶器向量经 xAPIC IRR 仪器点名）——
  TCG 下 hypervisor 套 hypervisor 的长会话死亡并非 L1 之过。完整证据链见
  `tests/CRASH_DEBUG_LOG.md` r190–r201。
- 硬编码 Windows 10 1903 (18362) 偏移；IOCTL 尚无权限模型；无微软正式签名
  （仅本地测试证书）。
- 裸机（真 AMD 硬件）未验证。

## 仓库布局

```
driver/   内核驱动源码（core/ mm/ modules/ platform/ hw/）
app/      svmbctl 控制工具
shared/   driver<->ctl 协议头
build/    编译脚本 + vm_*.bat 部署/取证脚本舰队 + guest 辅助脚本
tests/    CRASH_DEBUG_LOG.md（逐轮调试史）+ l2test L2 测试驱动
docs/     ARCHITECTURE.md / MODULE_GUIDE.md / 设计与审计文档
tools/    离线取证辅助（冻结内存面包屑解码器、QEMU 全内存 ELF→DMP64 转储
          合成器、崩溃签名探针）
AGENTS.md AI 代理工作规则（全部踩坑定律）
```

构建产物落 `x64/{Debug,Release}/`（已 gitignore）。

## 构建

```
build\build_debug.bat      :: Debug，driver + ctl，自动测试签名
build\build_release.bat    :: Release（SVMB_PRODUCTION），双签名
```

MSBuild + WDK (10.0.28000.0) + VS 工具集。`svmb.sys` 落 `x64\Debug\svmb.sys`
（vcxproj 钉死）。Release 签名需要本地证书库中名为 `svmb-test` 的测试证书。

## 测试环境

测试 VM：VMware Workstation 开嵌套虚拟化（`vhv.enable = TRUE`），guest
Windows 10 x64 1903 (18362)，BCD `testsigning` 开启，自动登录已配置。
宿主/guest 路径、VMX 位置与凭据是**你的**本地配置：把 `build\vmenv.template.bat`
复制为 `build\vmenv.bat`（已 gitignore）并填入你的值。`build\vm_*.bat`
舰队从该文件读取——仓库内不存任何凭据。

内核调试走 KDNET + WinDbg。部署/触发/取证一律走 `build\vm_*.bat` 脚本
（这些脚本固化了 `AGENTS.md` 中的硬性规则）。

第二条床承载嵌套虚拟化线（r139+）：QEMU TCG（`-accel tcg -cpu
max,hypervisor=off`），同款 Win10 1903 guest，`-serial file:` 串口黑盒，
QMP 冻结时刻全内存快照喂给 `tools/` 的离线转储合成器。

## 注册表旋钮（HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters）

| 名 | 默认 | 说明 |
|---|---|---|
| NptEnable | 0 | 1=NPT 启用 |
| TlbMode | 0 | 0=每 VMRUN FLUSH_ALL（armed 必需）；1=从不刷新（仅裸 NPT） |
| GuestAsid | 1 | VMCB GuestAsid |
| AutoStart | 1 | 加载即虚拟化；0=显式启动 |
| SerialLog | 0 | 串口飞行记录（vhv 上部分不可靠） |
| WiggleMode | 2 | 0=off 1=背靠背 kick 2=真 wiggle（DPC per-VCPU 恢复） |
| ReadVmEnable | 1（Debug）/ 0（Release） | 虚拟化层内存读（CR3_READVM）能力门 |
| Cr8Repoll | 0 | 1=每 exit 重读 CR8/TPR（QEMU 床 bring-up 辅助） |
| NestedIntrPunt | 0 | 嵌套会话 INTR 门（r197）：OwnIdt 会话武装 ic1 bit0 + 0x60 自服务排空；0 保持 accept 行为逐位一致 |
| BehaveRdThs / WrThs / OpThs | 128 / 32 / 16 | 行为分析告警阈值（每 1s 窗口） |
| BehaveResponse | 0 | 1=挂起告警进程（可逆）；kill 路径仅限拒绝列表 |

## 快速开始（测试平台）

1. 准备：AMD CPU + VMware Workstation（嵌套虚拟化开启）+ Win10 x64
   1903 测试 VM（BCD testsigning 开启、自动登录配好）；
   `copy build\vmenv.template.bat build\vmenv.bat` 并填入你的值。
2. 构建：`build\build_release.bat`（调试用 `build_debug.bat`）。
3. 部署：`build\vm_push_release74.bat` → `build\vm_svc_start.bat`
   （加载即全核虚拟化；裸 NPT + 区域保护无需任何 attach）。
4. 体验：
   - `svmbctl cr3 blockself` — 两层写阻断 E2E
   - `svmbctl cr3 holdpage 60` + 另一终端 `cr3 protect/unprotectid`（多目标）
   - `svmbctl mod attach cr3_monitor` 后 `svmbctl cr3 watch notepad.exe`
   - `svmbctl cr3 readvm-self` — 虚拟化层内存读 E2E
   - `svmbctl mod attach sysaudit` 后 `svmbctl sysaudit` — 系统调用感知 + 行为表
   - `svmbctl crypte2e` — 内存块加解密 E2E（enc 后操作员看到密文、目标读明文
     → dec 复原）
   - `svmbctl l2test 0` 再 `l2test 1` — 反嵌套基线 + L2 全回路
   - `svmbctl hreg` / `hunreg` — 外部 H 注册面
   - `svmbctl vlog stats` — guest hypervisor 干了什么（反射出口普查）
5. 回归脚本：`build/guest/multitarget.ps1`、`build/guest/blocksoak.bat`。

## 关键文档

- `AGENTS.md` — 代理工作规则：流程、通道、铁律（每条都有真实事故背书）
- `tests/CRASH_DEBUG_LOG.md` — round 11 起逐轮调试记录（现象/对照/修复/
  决策回顾/TODO）；r139–r201 嵌套战役轮次记录影子分页的构建过程与 QEMU TCG
  投递缺陷取证
- `docs/ARCHITECTURE.md`、`docs/MODULE_GUIDE.md`
- `docs/L2_BLOCKING_DESIGN.md` — 区域保护 sensing→blocking 升级设计
- `docs/SECURITY_AUDIT.md` — 安全审计（发现/处置/遗留）
- `docs/HYPERVISOR_MEM_READ.md` — 虚拟化层进程内存读取可行性评估
- `docs/MEM_CRYPT.md` — 进程内存块加解密：双帧分视、两阶段 seal、视图同步
  契约（含 U/S=0 陷阱方案的实测否决）

## 致谢与参考

- AMD APM Vol.2 第 15 章（SVM）——本实现遵循的规范
- [SimpleSvm](https://github.com/tandasat/SimpleSvm)（及
  [SimpleSvmHook](https://github.com/tandasat/SimpleSvmHook)）by tandasat ——
  用于交叉核对 NRIP/GIF/NPT 钩子语义的教学型 hypervisor

## License

[MIT](LICENSE)
