# svmb — Agent Working Rules

This file is the **source of truth** for any AI agent (Claude, ZCode, ...)
working on the svmb project. It encodes hard-won lessons from the
2026-09-06 / 07 bring-up and NPT debugging sessions. Violating a rule
here has cost real debugging time; the rule must be lifted, not bypassed.

## 1. Driver VM Debugging: use the `vm-driver-debug-loop` skill — strictly

**Rule**: Every Phase-1..Phase-7 step in the driver VM debug cycle MUST go
through the scripts in `build/vm_*.bat`, which are the Phase-bat fleet
maintained by the `vm-driver-debug-loop` skill at
the local vm-driver-debug-loop skill (outside this repo).

Skill phases and matching scripts:

| Phase | Skill step | Script |
|---|---|---|
| 0 | Compile driver / ctl | `build\build_debug.bat` |
| 1 | Reset VM (幂等) | `build\vm_reset_hard.bat` |
| 2 | Push driver to guest | `build\vm_push_driver.bat` |
| 3 | Clean / Create / Start svc | `build\vm_svc_clean.bat`, `vm_svc_create.bat`, `vm_svc_start.bat` |
| 3a | Set SerialLog=1 | `build\vm_set_seriallog.bat` |
| 3b | Set NptEnable=1 | `build\vm_reg_add_npt.bat` |
| 4 | Baseline info | `build\vm_ctl_info.bat` |
| 5 | Trigger (start / storm / hook) | `build\vm_ctl_start.bat`, `vm_ctl_run.bat` |
| 6 | Pull step log | `build\vm_pull_steps_log.bat` |
| 7 | Read VMware log + 串口黑盒 | `<vm-dir>\vmware.log`, `<windbg-test>\logs\serial_blackbox.log` |

**Why this rule is non-negotiable**:

- `vmrun` 串接 + 引号转义 + Session-0 假控制台会读 `condrv` 卡死等多个
  陷阱已经被这些 bat 固化；裸用 `vmrun` 每次都会重新踩。
- 取证通道（`svmb_steps_out.txt` + `vmware.log` + 串口黑盒）必须按
  Phase-6 的固定顺序拉，缺一步整个 freeze 现场就丢。
- 任何裸 `vmrun` / 裸 `powershell` 远程 IO 都视为绕过本规则，等同于
  本次 debug 结果不可信。

**When the rule allows flexibility**:

- 看 vmx 配置（grep vmx）、看 vmware.log 的最新行：这些是 host-only
  操作，不需要脚本包装。
- KDNET / WinDbg 命令：**必须经 windbg-mcp**（`mcp__mcp_windbg__*` 工具：
  `open_kd_session` / `run_kd_command` / `close_kd_session` 等），不走
  `vm-driver-debug-loop`，更**禁止裸跑 `kd.exe` / `windbg.exe` 命令行**
  （见例 6）。
- 跨主机文件传输 / SHA256 校验：走 `vmware-file-transfer` 技能。

**Concrete examples of the rule's bite (real cases)**:

1. **裸用 `vmrun runProgramInGuest` 忘了 `< NUL`** → 命令卡 60s 在
   Session-0 假控制台读 `condrv` → 误以为 VM 卡死 → 误判根因。
2. **裸用 `vmrun -gu/-gp` 放在操作子命令之后** → 报
   `requires valid user name`，重新调整。
3. **覆盖 guest .sys 之前没 sc stop** → 报"被占用" → driver 加载到
   半旧版本。
4. **vmrun reset 后没 ping 确认就开干** → VM 还没就绪，sc create 失败。
5. **KDNET 重连前没 close 旧 kd 会话** → 端口被占，新会话拿不到
   target。
6. **2026-09-10 (r60)**：熔解进行中时用**裸 `kd.exe` 命令行**一次性连接
   （`-c '...~* kb; .dump /f ...'`）→ kd 连接重试 + 转储 I/O 叠加在
   6 个满载 vcpu 线程上 → **宿主机整机卡死**，VM 被强杀，现场全丢。
   教训：① 内核调试一律走 windbg-mcp（会话可管理、可中断、可观测）；
   ② guest 熔解=宿主 CPU 已经被榨干，任何重 I/O（`.dump /f`、全核栈）
   都是雪上加霜——熔解期 kd 只做轻命令（vertarget / 单核栈），先评估
   再取证。

## 2. Knowledge persistence: write to CRASH_DEBUG_LOG.md

**Rule**: After every debugging round that reveals new facts (whether
resolved or unresolved), append a new round to `tests/CRASH_DEBUG_LOG.md`.
Each round has:

- **现象** (the user-observable failure)
- **与参考的对比** (参考实现、Cr3Encrypt, etc.)
- **修复尝试** (with commit hash and test result)
- **关键决策回顾** (a table of "what we thought vs. real cause")
- **TODO** (next bisect step)

**Why**: 2026-09-07 round-12 doc captured the v15 / v16 NPT fill
attempt + the truth that all three NPT strategies triggered the same
VM freeze — without that doc the next round would re-walk the same
blind alleys.

## 3. Hard reset (free VM) is the only recovery

**Rule**: When the VM freezes after ctl start (or any other trigger):

0. **FIRST check what actually died** (r45 law): "VMware Tools 未在
   客户机中运行" from vmrun is the storm-signature — vmtoolsd starved
   by exit pressure — NOT proof of kernel death. kd can still break in
   (CTRL+BREAK) while Tools is dead: kernel alive → treat as Tools
   loss, reset only if you need the Tools channel back. Kernel truly
   frozen = kd break times out too. Skipping this check cost r44 its
   whole "long-period death" narrative and r45 two boot cycles.
1. **DO NOT** try `sc stop svmb` from host — vmrun exec hangs forever
   (Tools is dead, so the runProgramInGuest never returns).
2. **DO NOT** try `svmbctl stop` — same reason, plus the IOCTL itself
   will hang in the kernel because the dispatcher can never exit.
3. **DO** run `build\vm_reset_hard.bat` (vmrun reset hard). 30-90s block
   is normal; ping to confirm recovery.

**Why**: Trying any path that goes through guest-VM after a freeze has
zero chance of returning; only the host-side vmrun reset is reliable.

## 4. Build and version discipline

**Rule**:

- 任何代码改动后必须 `build\build_debug.bat`（MSBuild 直接编 driver
  + ctl，自动测试签名）。
- svmb.sys 必须落在 `<repo>\x64\Debug\svmb.sys`
  （P1 路径，钉死在 driver/svmb.vcxproj 的 `<OutDir>` 之后）。其它
  路径（如 `driver\x64\Debug\svmb.sys`）一律视为 stale 产物。
- 部署前必跑 `sha256sum` 双向校验：`host certutil -hashfile` vs
  guest 同命令。

**Why**: 双目录漂移陷阱已经踩过（VS IDE 编 .sln 落到 `<repo>\x64\Debug\`,
msbuild 直接编落到 `<driver>\x64\Debug\`），拷错版本浪费一晚。

## 5. Skill-loading rules

- `vm-driver-debug-loop`：driver VM debug 循环。**唯一** Phase 1-7
  流程入口。
- `vmware-vm-control`：开/关/重启 VM 的电源操作（含 AutoAdminLogon）。
  重置场景下和 `vm-driver-debug-loop` 互补。
- `vmware-file-transfer`：host ↔ guest 文件传输 + SHA256 校验。
- `windbg-vm-kernel-debug`：WinDbg 内核调试（KDNET / BSOD dump / kd 命令）。
  **执行通道只许 windbg-mcp**（`mcp__mcp_windbg__*`），禁止裸 `kd.exe`
  命令行（r60 实测会把宿主机拖死）。
- `computer-use` / `browser-use`：仅用于截屏或 web 抓取，不用于
  驱动调试（误用会绕过 Phase 1-7 流程）。

## 6. What this file is NOT

- 不是 driver 的架构文档（看 `docs/ARCHITECTURE.md`）。
- 不是 API 文档（看 `docs/MODULE_GUIDE.md`）。
- 不是 round 11+ 调试记录（看 `tests/CRASH_DEBUG_LOG.md`）。

## 7. GitHub publication: Plan A (orphan single-root commit)

**Rule**:

- `main` 是公开发布分支，**永远是 1 个根提交**。完整开发历史只在
  本地 `master`，**永远不要 push `master`**。
- 发布流程：先在 `master` 自由开发，导出完整树到新 root commit，再
  `git push -f origin <sha>:refs/heads/main`。
- root commit 的 author 必须是 GitHub noreply 邮箱
  （`{user_id}+{login}@users.noreply.github.com`），否则 GitHub 上
  显 `www` 之类无关联身份，访客点不动。

**Why**:

- 历史里含旧凭据/路径/IP/转储等敏感残留，方案 A 是唯一可靠的发布面。
- master 不 push 是物理隔离：哪怕历史曾含敏感信息，也无法外泄。

**The author trap (real case, 2026-09-14)**:

- `git commit-tree` 不读 git config 之外的 author 字段，必须显式注入。
- `export GIT_AUTHOR_EMAIL=...` 在带 `&&` 的复合命令里**作用域只到下
  一条命令**就结束，`commit-tree` 实际仍用本地 config。表现：本地
  `git log` 显示 mikasaw，push 后 `gh api .../commits/main` 仍是 www。
- 正确做法：**用 `git -c user.name=... -c user.email=... commit-tree`**
  把身份注入到那次调用本身，绕过环境变量作用域。

**Reproduction commands**:

```
# build the public root from the master tree
TREE=$(git rev-parse master^{tree})
git -c user.name=mikasaw \
    -c user.email=57830391+mikasaw@users.noreply.github.com \
    commit-tree $TREE -m "svmb: initial public release
... (see README/CRASH_DEBUG_LOG pointers) ..." > NEW

# force-push and verify via API
git push -f origin $(cat NEW):refs/heads/main
git branch -f main $(cat NEW)
gh api repos/<login>/svmb/commits/main \
  -q '.commit.author.name + " <" + .commit.author.email + ">"'
# expect: mikasaw <57830391+mikasaw@users.noreply.github.com>
```

**Never**:

- `git push origin master`（历史泄露）
- 用本地 `www@local` 之类的非 noreply 邮箱做 root commit
  （GitHub 上作者无法跳到你的 profile）

This file is **only** the agent-side working rules. Keep it short,
keep it normative, keep it real (every rule has a real case behind it).
