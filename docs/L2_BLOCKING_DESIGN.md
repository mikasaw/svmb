# L2 拦截模式设计（sensing → blocking 升级路径）

状态：**方案 C 已由 r89 落地**（Policy per-region opt-in → trip 证据 →
KeInsertQueueDpc → IoQueueWorkItem → PASSIVE 终止写入者；拒绝名单 +
in-flight CAS + cr3→pid 反查复核；E2E 击杀腿 1/1，见
tests/CRASH_DEBUG_LOG.md Round 89）。归因边界（attach 归 owner、spoof
转嫁已 seed 进程）见 svmb_protocol.h Policy 注释。C- 挂起档已由 r91 落地（PsSuspendProcess 可逆冻结 + SUSP_LIST/SUSP_RESUME 无门控制面）。
以下为原始设计稿，保留存档。前置：r75 两层防御（L1 guest-MM 阻断 + L2
NPT 感知）、r76 fail-fast 消费者契约、r77 NCr3 shadow-kick。

## 1. 现状

| 层 | 机制 | 语义 |
|---|---|---|
| L1 | ZwProtectVirtualMemory(PAGE_READONLY) | 阻断。用户态/普通内核 VA 写由 guest MM 交付 AV，零注入 |
| L2 | NPT W-deny（区域 32 槽表） | 感知。DenyWrite exit = L1 绕过告警：resolve + re-arm + RegionTrip 事件 |

L2 目前**从不阻断**：旁路写最终落地（例如 r76 探针的内核 MDL 写）。
目标是给 L2 增加"阻断"档位。

## 2. 硬约束（本战役实测定律）

1. **注入事件不可靠**（r39/r66B/r70 家族）：vhv 对嵌套 VMCB 的异常
   注入半支持，#UD 重注入后 NMI 'TDO' 0x80。→ 不能把 AV 注回旁路写者。
2. **MTF 不透传嵌套 VMCB**（r41）：mtf=1 写入全部 VMCB、guest 存活、
   零 step 事件——MTF 单步在本 vhv 上**无效**（不只是危险）。
3. **退出路径保守按任意 IRQL 处理**：NPF 退出路径共享（r77 验收 P0
   实锤 CLOCK2 陷阱）；sentinel 页的 CLOCK2 实例来自时钟 ISR 写
   KTHREAD，区域页虽是用户数据页，仍必须假设同路径可达 CLOCK2 →
   路径上只允许 ≤DISPATCH 的操作，PASSIVE 工作（进程终止等）必须经
   DPC（任意 IRQL 合法）转投工作项。
4. **影子 NCr3 变值才重建**（r69/r77）：resolve 后必须 ShadowKick，
   否则重试风暴。
5. Release fail-fast（0xE2 'SVMC'）：任何未消费的 deny = 蓝屏。任何
   阻断方案也必须让每个 deny "被消费"（消费方式从 resolve 变为
   block，但契约不变）。

## 3. 方案对比

### 方案 A：写入仿真 + 丢弃（真·指令级阻断）

exit 时解码 GUEST_RIP 处的写指令，不提交存储，RIP 前进。

- 说明：解码正确的写仿真（跳过存储、整条指令前进）是 MMIO 仿真的
  标准做法，**不违反 r45 的机制本身**——r45 否决的是"页级 deny 下
  逐字节 AdvanceRip 爬出页外"。
- 真实否决理由：需要 x86 指令解码器子集（现成 mm/insn_len 只有长度
  解码；rep movs / 非规范寻址 / SMAP 交互全是雷）；部分写（字节使能）
  难以仿真；工程量与本框架体量不匹配。
- 结论：**不采**。

### 方案 B：写重定向到影子页（CoW）

区域页保持 W-deny；第一次旁路写时把该帧复制到影子帧，把**写者
核心**的 NPT 项临时指向影子帧（RW），写者的后续写全部落在影子页。

- 优点：内容零破坏（真页不可变），写者无感知继续跑。
- 缺点：NPT 是全局视图不是 per-写者视图——当前发布模型一个 Active
  对全体核（npt.h M4 PREREQUISITE 明言 per-core view 是前置）；
  影子页与真页的一致性、多写者聚合、谁读哪份，全是 M4 级工程。
- 结论：**依赖 per-core view（M4 前置）完成后再评估**。登记为
  远期路线。

### 方案 B'：NPT 诱饵帧换页（评审补充）

用现成 `SwapPage4k` 把区域 GPA 直接指向 scratch RW 帧（不复制、
不 CoW），后续旁路写落诱饵。无需解码、无注入。

- 否决理由：与 B 同卡全局视图前置——换页后**所有核**读写都看到
  诱饵帧（读写分裂、真内容不可达），语义破坏。登记备查。

### 方案 C：告警 + 策略性击杀（推荐 v1）

保持 sensing 全部机制（resolve+re-arm+kick），在 trip 消费尾部加一个
**策略位**：`Region.Policy = Alert`（现状）或 `Alert+Kill`。Kill 档：

1. trip 记录写入者身份（currentCr3、rip、gpa 留证）。
2. **KeInsertQueueDpc**（任意 IRQL 合法）→ DPC 转投 PASSIVE 工作项
   → 终止写入者进程。注意：不能直接 `IoQueueWorkItem`（其内部持
   队列自旋锁，>DISPATCH 非法——r77-P0 同类陷阱）；DPC 内也必须
   转 PASSIVE（ZwTerminate 需 PASSIVE）。CLOCK2 trip 与 ≤DISPATCH
   trip 走同一条 DPC 路径即可，无需降级取舍。
3. 终止的是**写入者进程**（不是线程）：通过 cr3 → pid 反查（见
   §4 边界），`PsLookupProcessByProcessId` + `ObOpenObjectByPointer`
   （**不要用 PsOpenProcess**——2004+ 导出，钉死测试机是 Win10
   18362/1903，会让驱动加载失败）+ `ZwTerminateProcess`。
4. 区域槽照常 resolve+re-arm（写入者死前那次写已落地——**阻断的是
   后续**，语义是"旁路一次即死刑"，威慑模型）。

- 优点：机制面小；与注入限制零冲突；r76 fail-fast 契约不受影响。
- 缺点与边界（诚实清单）：
  - 第一次旁路写仍然落地（指令级不阻断）。
  - **kill 覆盖边界**：seed 表只收录驱动加载后创建的进程；System/
    csrss/wininit 等加载前进程反查失败 → 只告警（这既是覆盖边界
    也是天然保护，但不能当作设计保证）。
  - **需新增构件**：cr3→pid 反查函数（现 Cr3Seed 只有 pid→cr3 的
    `Lookup`/`LookupByImage`，需加 List_ 遍历反查——节点 Graveyard_
    永生使其无锁可行）；PASSIVE 工作队列是**新管道**（现有 PASSIVE
    先例只有 arm worker 系统线程；device object 存在，可行）。
  - **单 killItem 复用竞态**：IoQueueWorkItem 对在队/在跑的同一
    work item 重复排队非法 → in-flight 标志（CAS 置位）或 per-trip
    分配。
  - **关键进程保护**：pid<4/System 的拒绝基本被 lookup 边界覆盖，
    但 csrss/wininit/services 等加载后系统进程需要**映像名拒绝
    名单**（Cr3ImageMatch 现成）兜底，否则击杀即带崩系统。
  - 写入者=owner 自己：Policy=1 的区域 owner 也受限（这正是防篡改
    语义，文档明示）。
  - kill 风暴（写者循环重试）：CAS 限频沿用 s_lastKick 模式。
- 结论：**v1 采此**。

### 方案 C-：告警 + 挂起（评审补充，可逆档）

kill 的前置档位：挂起写入者（可逆，误报可恢复）。注意
PsSuspendProcess 同为 2004+ 导出，需走线程挂起路径（遍历线程
PsSuspendThread）或作为 kill 的观察期。轻量，值得在实现轮一句话
评估是否并入。

### 方案 D：MTF 单步跳写

- 否决：**r41 定律——MTF 不透传嵌套 VMCB，本 vhv 上根本无效**
  （主引）；r39 注入家族为同族旁证。

### 方案 E：L1 完备化（评审补充，记录在案）

对区域帧的内核直映/别名 PTE 也上 readonly，让内核物理映射旁路写
同样由 guest MM 交付 AV（零注入，沿用已验证的 L1 机制）。
- 否决理由：波及全系统（直映覆盖全部物理内存），MmCopyMemory 类
  合法直映写者被误伤，系统级风险不可接受。但它是 L1→L2 谱系里
  应记录的一档。

## 4. 方案 C 落地草图（供实现轮展开）

```
协议：SVMB_CR3_PROT 追加 Policy 字段。注意：r78 已把唯一的 Reserved
      改名为 InUnprotectId，Policy 只能追加在 Entries[8] 之后 →
      sizeof 增大；driver main.cpp 的 `>= sizeof` 最小长度校验会让
      旧 ctl 响亮失败（BUFFER_TOO_SMALL）——ctl 必须同步重建（尾部
      追加纪律照旧，此轮是有感 ABI 变更，提交说明里明示）。
驱动：
  ProtRegion 增 Policy、WriterCr3/WriterRip/WriterGpa 字段
  SentinelRegionConsume 命中分支尾部：
    if (Policy==Alert+Kill) {
        记录 writerCr3/rip/gpa
        KeInsertQueueDpc(killDpc)          // 任意 IRQL 合法
    }
  killDpc(DISPATCH)：IoQueueWorkItem(killItem)   // 单实例 + in-flight
                                                     CAS 标志防重复排队
  killWorkItem(PASSIVE)：
    seed 反查 cr3->pid（新增 List_ 遍历；反查失败只告警）
    拒绝名单：pid<4 + 映像名拒绝名单（cr3_image_match 现成）：
    csrss.exe/wininit.exe/winlogon.exe/services.exe/lsass.exe/smss.exe
    PsLookupProcessByProcessId + ObOpenObjectByPointer + ZwTerminateProcess
    日志 "prot: KILL policy writer pid=%u cr3=%llx rip=%llx gpa=%llx"
测试：
  victim = holdpage（owner，Policy=1 区域）
  writer = 另一进程对 owner 区域发 CR3_PROBE
  期望：writer 进程被终止、owner 区域完好、系统存活
风险：
  - 写入者=owner 自己（自写被杀）→ 文档明示 Policy=1 的区域 owner
    也受限（这正是防篡改语义）
  - writerCr3 属于加载前/已退出进程 → 反查失败只告警
  - kill 风暴（写者循环重试）→ CAS 限频沿用 s_lastKick 模式 +
    in-flight 标志天然合并
```

## 5. 决策记录

| 项 | 决定 |
|---|---|
| v1 | 方案 C（告警+击杀，opt-in per region；C- 挂起档实现轮评估） |
| 远期 | 方案 B（等 per-core view/M4）；B'、E 已否决备查 |
| 永不 | 方案 D（r41：MTF 本 vhv 无效）；A 本轮不采（解码器工程量，非机制禁止） |
