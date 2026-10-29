# svmb — AMD-V Nested Hypervisor Research Driver

English | [简体中文](README.zh-CN.md)

Windows kernel driver that puts every CPU core into AMD-V (SVM) mode right at
driver load, and on top of that builds: NPT memory virtualization, NPT-based
page-permission hooks, a CR3 protection suite (sentinel sensing / per-core
process-view isolation / read spoofing), X-deny syscall sensing with
cross-process behavior analytics, per-process memory-block encryption
(dual-frame split view), crash forensics tooling — and a nested-virtualization
research line: hosting guest hypervisors (L2) with per-core shadow two-stage
NPT paging, VMLOAD/VMSAVE/INVLPGA emulation, a virtualization-behavior
recorder (vlog), and nested interrupt-delivery forensics.

**Status: research / test platform — not production-ready.**

> **Disclaimer** — this is an educational research project. It is a kernel
> driver that virtualizes the machine it loads on: things can and do crash
> (bugchecks, wedged VMs). Run it only in disposable virtual machines.
> Do not use it, or anything derived from it, on machines you do not own, or
> for any malicious purpose.

## Feature Matrix

| Capability | What it does | Status (vhv / QEMU TCG beds) |
|---|---|---|
| All-core virtualization | Virtualizes 6/6 cores at DriverEntry (`AutoStart=1`); clean single-phase unload | ✅ stable (multi-round 10min+ soaks, 13.3T-exit storms) |
| NPT identity map | `[0, RamEnd)` as 2M RWX large pages incl. MMIO holes | ✅ stable (bare NPT, hour-scale) |
| NPT hook framework | slide exec/data transitions, stub/callback layers, stole-prefix ASM detour | ✅ (real-kernel hooks, e.g. NtCreateFile, fully green) |
| CR3 sentinel sensing | NPT W-deny on watched processes' thread-object pages → one trip + direction bit per schedule write | ✅ mechanism proven (670 trips / 43×out=1) |
| Region write protection | `cr3 protect/unprotectid/holdpage/regions`: L1 guest-MM blocking + L2 NPT sensing; `probee2e` MDL-bypass probe; shadow-kick wiggle for deterministic single-exit sensing; dead-process sweep | ✅ probee2e 30/30 + multitarget + 2h mixed soak 48/48 |
| per-core process view | publishes a ProcView NCr3 on the core while the target is current (policy-hiding base) | ✅ mechanism proven; **TlbMode=0 only** |
| CR3 read spoofing | XOR-key read disguise | ⚠️ **unusable on vhv** (nested CR3-read intercept does not pass through) — bare-metal feature |
| Syscall / exec sensing (`sysaudit`) | X-denies the pages of MmCopyVirtualMemory, NtCreateFile, NtOpenProcess, NtReadVirtualMemory, NtWriteVirtualMemory (SSDT+SSN anchored, multi-anchor validated); entry-fetch NPF records caller pid/image + args; page reopens and the instruction re-executes natively; 250ms timer re-arms. **Zero guest bytes modified** — PatchGuard-invisible by construction | ✅ E2E (real NtCreateFile/NTRD entries with pid attribution, zero lost) |
| Behavior analytics | 1s-window per-(caller,target) cross-process call rates; edge-triggered alerts; thresholds via registry knobs | ✅ live (RD/WR/OP tables) |
| Alert response | reversible suspend of offending processes (default **off**, `BehaveResponse`), deny-list, kill-time live-EPROCESS image re-verify (K3) | ✅ E2E (kill=1/suspend=1, 4h dual-policy soak 395 iters) |
| Memory-block encryption (`crypt`) | dual-frame split view: each block page maps to a private plaintext copy on the target's own NPT view while the ORIGINAL frame stores XOR-keystream ciphertext — the target keeps reading/writing plaintext, every other observer (physical walk, scanners) sees ciphertext; two-phase seal behind an exit-generation gate (target frozen across the transition); key-free decrypt + unload-time restore | ✅ E2E (multi-block cycle + hold + unload-restore with a live target, zero damaged reads; offline suite 347/0) |
| Crash forensics | breadcrumb flight recorder (lock-free ring, live IOCTL + offline suspended-RAM decode), kdump helper | ✅ (decoded both live and frozen guests) |
| Nested virtualization (L2 sessions) | external-H registration (`hreg`: EFER dual-view, own virtual HSAVE, CPUID SVM unmasked for the registrant), anti-nesting `ic2.VMRUN` armed by default with clean #BP rejection, full L2 round trip (H builds L2's world: page tables + VMCB with CPUID intercepted; control returns with the reflected exit verified), VMLOAD/VMSAVE emulated for L2 | ✅ stage gauntlet green (s0–s3 + nested suite, QEMU TCG bed) |
| Shadow two-stage NPT (shadow paging) | per-core shadow NPT sessions over svmb's own NPT translating L2 GPAs, INVLPGA-scoped invalidation contract, permission passthrough, 2M large pages, out-of-window hook-slot materialization, cross-core pool changes self-cored to the acting core; multi-core judge (4 H worlds × 4 cores concurrent round trips) | ✅ end-to-end (r184; mechanism regression 11/11) |
| vlog — third-party-H behavior recorder | six record classes of what a guest hypervisor actually does (reflected exits with decode, INVLPGA, EFER/SVM MSR writes, ...), lock-free ring, `vlog stats/dump/mask/reset` | ✅ |
| Nested delivery forensics | six-door delivery-path study + xAPIC MMIO IRR instrument naming the killer interrupt sequence; verdict: a two-part nested-delivery defect in QEMU TCG (exception frame-push corruption + lost deliveries) — the svmb-side design space is exhausted with evidence | 📋 documented (r190–r201) |
| Module system | debugger / cr3_monitor / sysaudit modules over a function-table API | ✅ |
| Control tool | `svmbctl`: info/start/stop/storm/cr3 */exitprof/dbg */sysaudit/kdump/crumbs | ✅ |

## Maturity & Known Limits

- **Release track is on** (r73+): Release/`SVMB_PRODUCTION` builds + dual
  test-signing + green regressions. NPF fail-fast (`0xE2 'SVMC'`) is by
  design: every NPT deny must be consumed by a registered consumer.
- **armed-watch is not fully converged under TlbMode=0**: 45min soak alive on
  a mature boot (r64), but early-boot arming still wedges acutely (r65).
  The generic freeze under attached churn is under active investigation
  (r104–r108: the death point is narrowed to hop granularity with the
  breadcrumb recorder; root cause not yet fixed). Bare NPT and region
  protection are stable in both TLB modes.
- **TlbMode semantics**: `0` = flush-all on every VMRUN (default; required
  for armed operations — the load-bearing compensation for a nested-TLB
  weakness on this vhv); `1` = never flush (bare-NPT scenarios only).
- **Sampling semantics** (by design): calls inside the 250ms open window or
  behind cached TLB translations are not sensed; a tight in-process loop can
  slide past the re-arm.
- **mem_crypt view-sync contract** (r133): the per-core NCr3 is republished
  only at VM exits, so a protected target must take an intercepted CPUID at
  its block-access points (the sync point — see
  `tests/mcrypt_target.c`); an exitless epoch that migrates to a core still
  holding a stale base NCr3 reads the original frame, which is ciphertext
  post-seal. Structural fix (per-core view selection) is future work
  (`npt.h` M4 prerequisite). A U/S=0 base-view read-trap was measured and
  rejected: the kernel's own working-set/compression touches disarm it
  within seconds.
- **Nested line verified on the QEMU TCG bed** (r139–r201): the TCG nested
  interrupt-delivery seam is a proven two-part defect in QEMU (exception
  frame-push corruption + lost deliveries, killer vectors named via an xAPIC
  IRR instrument) — long hypervisor-in-hypervisor sessions under TCG die
  through no fault of the L1. See `tests/CRASH_DEBUG_LOG.md` r190–r201 for
  the full evidence chain.
- Hardcoded Windows 10 1903 (18362) offsets; IOCTLs have no permission
  model yet; no Microsoft signature (local test cert only).
- Bare metal (real AMD hardware) is unverified.

## Repository Layout

```
driver/   kernel driver source (core/ mm/ modules/ platform/ hw/)
app/      svmbctl control tool
shared/   driver<->ctl protocol headers
build/    build scripts + the vm_*.bat deploy/forensics fleet + guest helpers
tests/    CRASH_DEBUG_LOG.md (per-round debug history) + l2test L2 test drivers
docs/     ARCHITECTURE.md / MODULE_GUIDE.md / design & audit documents
tools/    offline forensics helpers (suspended-memory crumb decoder, QEMU
          full-memory ELF→DMP64 dump synthesizer, crash signature probe)
AGENTS.md AI-agent working rules (hard-won operational laws)
```

Build outputs land in `x64/{Debug,Release}/` (gitignored).

## Build

```
build\build_debug.bat      :: Debug, driver + ctl, auto test-signed
build\build_release.bat    :: Release (SVMB_PRODUCTION), dual-signed
```

MSBuild + WDK (10.0.28000.0) + VS 2022-class toolset. `svmb.sys` lands at
`x64\Debug\svmb.sys` (pinned in the vcxproj). Release signing expects a
local test certificate named `svmb-test` in your cert store.

## Test Environment

Test VM: VMware Workstation with nested virtualization enabled
(`vhv.enable = TRUE`), guest Windows 10 x64 1903 (18362), BCD
`testsigning` on, auto-logon configured. Host/guest paths, VMX location and
credentials are **your** local configuration: copy
`build\vmenv.template.bat` to `build\vmenv.bat` (git-ignored) and fill in
your values. The `build\vm_*.bat` fleet reads them from there — no
credentials are stored in the repo.

Kernel debugging goes through KDNET + WinDbg. Deployment / trigger /
forensics always run through the `build\vm_*.bat` scripts (see `AGENTS.md`
for the hard rules these scripts encode).

A second bed carries the nested-virtualization line (r139+): QEMU with TCG
(`-accel tcg -cpu max,hypervisor=off`), same Win10 1903 guest, serial
black-box via `-serial file:`, and QMP for freeze-time full-memory snapshots
that feed the `tools/` offline dump synthesizer.

## Registry Knobs (`HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters`)

| Name | Default | Meaning |
|---|---|---|
| NptEnable | 0 | 1 = enable NPT |
| TlbMode | 0 | 0 = FLUSH_ALL per VMRUN (required for armed ops); 1 = never flush (bare NPT) |
| GuestAsid | 1 | VMCB GuestAsid |
| AutoStart | 1 | virtualize at driver load; 0 = explicit start only |
| SerialLog | 0 | serial flight recorder (partially unreliable on vhv) |
| WiggleMode | 2 | 0=off 1=back-to-back kick 2=real wiggle (DPC per-VCPU restore) |
| ReadVmEnable | 1 (Debug) / 0 (Release) | gate for the hypervisor-level memory read (CR3_READVM) |
| Cr8Repoll | 0 | 1 = re-poll CR8/TPR at every exit (QEMU bed bring-up aid) |
| NestedIntrPunt | 0 | nested-session INTR gate (r197): arm ic1 bit0 for OwnIdt sessions + 0x60 self-serve drain; 0 keeps accept behavior bit-identical |
| BehaveRdThs / WrThs / OpThs | 128 / 32 / 16 | behavior-analytics alert thresholds (per 1s window) |
| BehaveResponse | 0 | 1 = suspend alerting processes (reversible); kill path stays deny-list-only |

## Quick Start (test platform)

1. Prepare: AMD CPU + VMware Workstation (nested virtualization on) + a
   Win10 x64 1903 test VM (`testsigning` on, auto-logon);
   `copy build\vmenv.template.bat build\vmenv.bat` and fill in your values.
2. Build: `build\build_release.bat` (or `build_debug.bat`).
3. Deploy: `build\vm_push_release74.bat` → `build\vm_svc_start.bat`
   (load = all-core virtualization; bare NPT + region protection need no
   module attach).
4. Try:
   - `svmbctl cr3 blockself` — two-layer write-blocking E2E
   - `svmbctl cr3 holdpage 60` + `cr3 protect/unprotectid` from another shell
   - `svmbctl mod attach cr3_monitor` then `svmbctl cr3 watch notepad.exe`
   - `svmbctl cr3 readvm-self` — hypervisor-level memory read E2E
   - `svmbctl mod attach sysaudit` then `svmbctl sysaudit` — syscall sensing
     + behavior table
   - `svmbctl crypte2e` — memory-block encryption E2E (enc → operator sees
     ciphertext while the target reads plaintext → dec → restore)
   - `svmbctl l2test 0` then `l2test 1` — anti-nesting baseline + full L2
     round trip
   - `svmbctl hreg` / `hunreg` — external-H registration face
   - `svmbctl vlog stats` — what the guest hypervisor did (reflected-exit
     census)
5. Regression: `build/guest/multitarget.ps1`, `build/guest/blocksoak.bat`.

## Key Documents

- `AGENTS.md` — agent working rules: channels, iron laws (each backed by a
  real incident)
- `tests/CRASH_DEBUG_LOG.md` — per-round debug history from round 11 on
  (symptom / comparison / fix / decision review / TODO); the r139–r201
  nested-virt campaign rounds document the shadow-paging build-out and the
  QEMU TCG delivery-defect forensics
- `docs/ARCHITECTURE.md`, `docs/MODULE_GUIDE.md`
- `docs/L2_BLOCKING_DESIGN.md` — sensing→blocking escalation design
- `docs/SECURITY_AUDIT.md` — security audit findings and dispositions
- `docs/HYPERVISOR_MEM_READ.md` — hypervisor-level process-memory read
- `docs/MEM_CRYPT.md` — per-process memory-block encryption: dual-frame
  split view, two-phase seal, view-sync contract (and the measured
  U/S=0-trap rejection)

## Credits & References

- AMD APM Vol.2 Chapter 15 (SVM) — the specification this follows
- [SimpleSvm](https://github.com/tandasat/SimpleSvm) (and
  [SimpleSvmHook](https://github.com/tandasat/SimpleSvmHook)) by tandasat —
  educational hypervisors used for cross-checking NRIP/GIF/NPT-hook semantics

## License

[MIT](LICENSE)
