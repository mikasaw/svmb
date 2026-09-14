; svmb - SVM VMRUN loop and register plumbing.
; Reference-hypervisor technique: GIF stays enabled across guest execution
; (VMRUN sets GIF, #VMEXIT clears it - so exit handlers run interrupt-free),
; host state is a snapshot of guest state (thin hypervisor), devirtualization
; is requested by the exit handler via GuestRegs.Extra1/Extra2.
;
; Register conventions inside the loop (see core/regs.h):
;   Extra1 = devirtualize flag & regs base pointer (set by HC_EXIT_VMM handler)
;   Extra2 = guest resume rip for devirtualization
;
; Stack geometry inside _svmb_vmm_loop (relative to final rsp):
;   [rsp+00] vcpu  [rsp+08] guestVmcbPa  [rsp+10] hostVmcbPa
;   [rsp+18] stackTop [rsp+20] oldRsp [rsp+28] oldRsp copy
;   [rsp+30..57] scratch / MACHINE_FRAME area

extern SvmbVmExitEntry : Proc
extern SvmbFillMachineFrame : Proc

include offsets.inc

.code

BACKUP_REGISTERS Macro baseAddrReg
	movaps xmmword ptr [baseAddrReg + 000h], xmm0
	movaps xmmword ptr [baseAddrReg + 010h], xmm1
	movaps xmmword ptr [baseAddrReg + 020h], xmm2
	movaps xmmword ptr [baseAddrReg + 030h], xmm3
	movaps xmmword ptr [baseAddrReg + 040h], xmm4
	movaps xmmword ptr [baseAddrReg + 050h], xmm5
	movaps xmmword ptr [baseAddrReg + 060h], xmm6
	movaps xmmword ptr [baseAddrReg + 070h], xmm7
	movaps xmmword ptr [baseAddrReg + 080h], xmm8
	movaps xmmword ptr [baseAddrReg + 090h], xmm9
	movaps xmmword ptr [baseAddrReg + 0A0h], xmm10
	movaps xmmword ptr [baseAddrReg + 0B0h], xmm11
	movaps xmmword ptr [baseAddrReg + 0C0h], xmm12
	movaps xmmword ptr [baseAddrReg + 0D0h], xmm13
	movaps xmmword ptr [baseAddrReg + 0E0h], xmm14
	movaps xmmword ptr [baseAddrReg + 0F0h], xmm15
	mov [baseAddrReg + 100h], r15
	mov [baseAddrReg + 108h], r14
	mov [baseAddrReg + 110h], r13
	mov [baseAddrReg + 118h], r12
	mov [baseAddrReg + 120h], r11
	mov [baseAddrReg + 128h], r10
	mov [baseAddrReg + 130h], r9
	mov [baseAddrReg + 138h], r8
	mov [baseAddrReg + 140h], rbp
	mov [baseAddrReg + 148h], rsi
	mov [baseAddrReg + 150h], rdi
	mov [baseAddrReg + 158h], rdx
	mov [baseAddrReg + 160h], rcx
	mov [baseAddrReg + 168h], rbx
Endm

; rax is intentionally NOT restored: it keeps pointing at the GuestRegs base
; so the caller can read Extra1/Extra2 right after the macro.
RESTORE_REGISTERS Macro baseAddrReg
	movaps xmm0, xmmword ptr [baseAddrReg + 000h]
	movaps xmm1, xmmword ptr [baseAddrReg + 010h]
	movaps xmm2, xmmword ptr [baseAddrReg + 020h]
	movaps xmm3, xmmword ptr [baseAddrReg + 030h]
	movaps xmm4, xmmword ptr [baseAddrReg + 040h]
	movaps xmm5, xmmword ptr [baseAddrReg + 050h]
	movaps xmm6, xmmword ptr [baseAddrReg + 060h]
	movaps xmm7, xmmword ptr [baseAddrReg + 070h]
	movaps xmm8, xmmword ptr [baseAddrReg + 080h]
	movaps xmm9, xmmword ptr [baseAddrReg + 090h]
	movaps xmm10, xmmword ptr [baseAddrReg + 0A0h]
	movaps xmm11, xmmword ptr [baseAddrReg + 0B0h]
	movaps xmm12, xmmword ptr [baseAddrReg + 0C0h]
	movaps xmm13, xmmword ptr [baseAddrReg + 0D0h]
	movaps xmm14, xmmword ptr [baseAddrReg + 0E0h]
	movaps xmm15, xmmword ptr [baseAddrReg + 0F0h]
	mov r15, [baseAddrReg + 100h]
	mov r14, [baseAddrReg + 108h]
	mov r13, [baseAddrReg + 110h]
	mov r12, [baseAddrReg + 118h]
	mov r11, [baseAddrReg + 120h]
	mov r10, [baseAddrReg + 128h]
	mov r9,  [baseAddrReg + 130h]
	mov r8,  [baseAddrReg + 138h]
	mov rbp, [baseAddrReg + 140h]
	mov rsi, [baseAddrReg + 148h]
	mov rdi, [baseAddrReg + 150h]
	mov rdx, [baseAddrReg + 158h]
	mov rcx, [baseAddrReg + 160h]
	mov rbx, [baseAddrReg + 168h]
Endm

CALL_WITH_SHADOW Macro functionName
	sub rsp, 30h
	call functionName
	add rsp, 30h
Endm

; ---- descriptor table / segment helpers ----

_svmb_sgdt Proc
	sub rsp, 10h
	sgdt [rsp]
	mov ax, [rsp]
	mov word ptr [rdx], ax
	mov rax, [rsp + 2]
	mov qword ptr [rcx], rax
	add rsp, 10h
	ret
_svmb_sgdt Endp

_svmb_sidt Proc
	sub rsp, 10h
	sidt [rsp]
	mov ax, [rsp]
	mov word ptr [rdx], ax
	mov rax, [rsp + 2]
	mov qword ptr [rcx], rax
	add rsp, 10h
	ret
_svmb_sidt Endp

_svmb_sldt Proc
	sldt ax
	mov word ptr [rcx], ax
	ret
_svmb_sldt Endp

_svmb_str Proc
	str ax
	mov word ptr [rcx], ax
	ret
_svmb_str Endp

_svmb_cs Proc
	mov ax, cs
	ret
_svmb_cs Endp

_svmb_ds Proc
	mov ax, ds
	ret
_svmb_ds Endp

_svmb_es Proc
	mov ax, es
	ret
_svmb_es Endp

_svmb_fs Proc
	mov ax, fs
	ret
_svmb_fs Endp

_svmb_gs Proc
	mov ax, gs
	ret
_svmb_gs Endp

_svmb_ss Proc
	mov ax, ss
	ret
_svmb_ss Endp

; ---- debug register helpers (DR0-3 have no VMCB save-area slots) ----

_svmb_read_dr0 Proc
	mov rax, dr0
	ret
_svmb_read_dr0 Endp

_svmb_read_dr1 Proc
	mov rax, dr1
	ret
_svmb_read_dr1 Endp

_svmb_read_dr2 Proc
	mov rax, dr2
	ret
_svmb_read_dr2 Endp

_svmb_read_dr3 Proc
	mov rax, dr3
	ret
_svmb_read_dr3 Endp

_svmb_write_dr0 Proc
	mov dr0, rcx
	ret
_svmb_write_dr0 Endp

_svmb_write_dr1 Proc
	mov dr1, rcx
	ret
_svmb_write_dr1 Endp

_svmb_write_dr2 Proc
	mov dr2, rcx
	ret
_svmb_write_dr2 Endp

_svmb_write_dr3 Proc
	mov dr3, rcx
	ret
_svmb_write_dr3 Endp

_svmb_read_dr7 Proc
	mov rax, dr7
	ret
_svmb_read_dr7 Endp

_svmb_write_dr7 Proc
	mov dr7, rcx
	ret
_svmb_write_dr7 Endp

; invlpga(gva in rcx, asid in rdx) - hardware form: invlpga rax, ecx.
; The ASID operand MUST be loaded into ECX from rdx (the C-side signature is
; _svmb_invlpga(u64 gva, u32 asid) via the Win64 ABI rcx/rdx); leaving ECX
; as-is would use the low 32 bits of the GVA as the ASID.
_svmb_invlpga Proc
	mov rax, rcx        ; gva (32-bit GVA operand)
	mov ecx, edx        ; asid
	invlpga rax, ecx
	ret
_svmb_invlpga Endp
; r50 FIX: was 'mov eax, ecx' - that zeroed the upper 32
; bits of the GVA, so INVLPGA never flushed any kernel-address
; translation (a silent no-op for every 64-bit VA).


; ---- guest-side hypercall helper ----
; u64 _svmb_hypercall(u64 nr /*rcx*/, u64 a1 /*rdx*/, u64 a2 /*r8*/, u64* out /*r9*/)
; Win64 ABI: rbx is non-volatile and must survive this call - push/pop it.
; a3 (vmmcall rdx) is unused by the protocol and explicitly zeroed so handlers
; see a defined value (the CPUID fallback channel passes a3 = 0 as well).
; VMMCALL exit preserves all guest GPRs through the exit/restore path, so
; volatile registers survive and rax comes back as the return value.
_svmb_hypercall Proc
    push rbx
    mov r10, r9
    mov rax, rcx
    mov rbx, rdx
    mov rcx, r8
    xor edx, edx
    vmmcall
    mov [r10], rax
    pop rbx
    ret
_svmb_hypercall Endp

; ---- two-phase register save/restore for entering virtualization ----
; Phase 1 (normal call, returns with rax = original guest rax): saves all
; registers/rflags and the resume point into GuestRegs at [rcx].
; Phase 2 (vmrun starts at if_load_regs with rax = &GuestRegs): restores
; everything and jumps to the saved resume point; the thread continues
; transparently as guest.
_svmb_save_or_load_regs Proc
	BACKUP_REGISTERS rcx

	pushfq
	mov rax, [rsp]
	mov [rcx + GREG_RFLAGS], rax
	popfq

	mov rax, offset if_load_regs
	mov [rcx + GREG_RIP], rax

	mov rax, [rsp]              ; phase-1 return address = resume point
	mov [rcx + GREG_EXTRA1], rax

	mov rax, rsp
	add rax, 8                  ; caller rsp (pop the return address)
	mov [rcx + GREG_RSP], rax

	mov rax, 0                  ; phase 1 marker: skip restore

if_load_regs:
	test rax, rax
	jz phase1_return

	RESTORE_REGISTERS rax       ; rcx = &GuestRegs again (phase-1 param)
	push qword ptr [rax + GREG_RFLAGS]
	popfq
	mov rsp, [rax + GREG_RSP]
	mov rax, [rax + GREG_RAX]
	jmp qword ptr [rcx + GREG_EXTRA1]

phase1_return:
	mov rax, [rcx + GREG_RAX]
	ret
_svmb_save_or_load_regs Endp

; ---- main VMRUN loop ----
; void _svmb_vmm_loop(VcpuContext* vcpu /*rcx*/, u64 guestVmcbPa /*rdx*/,
;                     u64 hostVmcbPa /*r8*/, void* vmmStackTop /*r9*/)
; Never returns on success. Returns only when VMRUN itself failed
; (exit code -1/-2/-3); the caller must treat that as fatal.
_svmb_vmm_loop Proc Frame
	mov rax, rsp
	mov rsp, r9

	sub rsp, 28h           ; MACHINE_FRAME-sized scratch for WinDbg walks
	sub rsp, 8h            ; 16-byte alignment pad
	push rax
	push rax
	push r9
	push r8
	push rdx
	push rcx

	.pushframe
	.allocstack 38h
	.endprolog

enter_guest:
	mov rax, [rsp + 8h]
	vmload rax             ; load guest segment + MSR state

	mov rax, [rsp + 8h]
	vmrun rax              ; enter guest (sets GIF)

	; VMRUN failure? (invalid -1 / busy -2 / illegal -3)
	; r27-exp3: GuestVmcb is a POINTER at +0 (independent contiguous page),
	; so chase it before reading Ctrl.ExitCode.
	mov rax, [rsp]
	mov rax, [rax + VCPU_GUEST_VMCB]     ; vcpu->GuestVmcb (VMCB*)
	mov rax, [rax + VMCB_EXIT_CODE]
	cmp rax, -1
	je vmrun_failed
	cmp rax, -2
	je vmrun_failed
	cmp rax, -3
	je vmrun_failed

	mov rax, [rsp + 8h]
	vmsave rax             ; save guest state into guest VMCB

	mov rax, [rsp + 10h]
	vmload rax             ; load host state (host = guest snapshot)

	; capture guest GPRs into vcpu->Regs (rax/rsp/rip/rflags fixed up by the
	; C++ shim from the VMCB save area)
	mov rax, [rsp]
	add rax, VCPU_REGS
	BACKUP_REGISTERS rax

	; SvmbFillMachineFrame(machineFrame = rsp+38h, vcpu)
	mov rcx, rsp
	add rcx, 38h
	mov rdx, [rsp]
	CALL_WITH_SHADOW SvmbFillMachineFrame

	; SvmbVmExitEntry(vcpu, regs)
	mov rcx, [rsp]
	mov rdx, rcx
	add rdx, VCPU_REGS
	CALL_WITH_SHADOW SvmbVmExitEntry

	mov rax, [rsp]
	add rax, VCPU_REGS
	RESTORE_REGISTERS rax

	; devirtualize request? Extra1 = &Regs (nonzero = request)
	mov rax, [rax + GREG_EXTRA1]
	test rax, rax
	jnz exit_virtualization

	; self-exit check: State (VCPU_INFO+0) == Leaving (3) means Stop() asked
	; THIS core to leave the VMM. Do it right here - no cross-core VMMCALL,
	; no affinity dance. We are in the VMM on our own stack, host state is
	; loaded, so simply shut SVME off and return to EnterCore's caller.
	; VcpuInfo is EMBEDDED at +0x3000 (vcpu.h static_assert), State is the
	; first LONG - load the dword directly, do NOT follow a pointer (the old
	; pointer-chase dereferenced {State|ExitTraceLeft<<32} as an address and
	; #PF'd on every exit; the fault dispatched on VmmStack -> 139/4).
	mov rax, [rsp]                    ; vcpu
	mov eax, [rax + VCPU_INFO]        ; Info.State (dword, zero-extended)
	cmp eax, 3                        ; VcpuState::Leaving
	jne no_self_exit

	; r28 fix: the old code shut SVME off and returned to EnterCore here,
	; DISCARDING the interrupted thread's context (its GPRs were already
	; back in the CPU via RESTORE_REGISTERS; rax/rcx/rdx then got clobbered
	; by the EFER dance). Any live thread hit by the devirt exit was
	; abandoned mid-execution and later crashed (139/4, RIP=0 - see
	; CRASH_DEBUG_LOG round 27). Devirt now happens IN PLACE: the
	; interrupted thread resumes as if the exit never happened, with SVM
	; off; the core's enter thread just continues as a plain kernel thread
	; whenever the scheduler runs it.
	mov rax, [rsp]                        ; vcpu (rax already dead: State)
	mov rcx, [rax + VCPU_GUEST_VMCB]      ; guest VMCB VA (pointer, r27-exp3)
	; r28b gate: only resume in place when the interrupted context is a
	; KERNEL-mode one (Save.Rip top byte 0xFF = canonical kernel VA). A
	; user-mode context needs VMRUN's full state reload (CPL/segments);
	; resuming it host-side executes user code at CPL0 -> 0x0A bugcheck
	; (r28v2 crash). Save.Cpl is NOT usable: FillGuestState writes 0 and
	; every exit trace shows cpl=0 even for user rips.
	mov al, [rcx + 578h + 7]              ; Save.Rip byte 7
	cmp al, 0FFh
	jne no_self_exit                      ; user exit: service it and re-enter;
	                                      ; this core devirts on its next
	                                      ; kernel-mode exit
	mov rdx, [rcx + 550h]                 ; interrupted thread's CR3 (Save.Cr3)
	mov [rsp + 30h], rdx                  ; stash in frame scratch
	mov rax, [rsp + 8]                    ; guestVmcbPa
	vmload rax                            ; thread's segments/MSRs - MUST run
	                                      ; while SVME is still on (APM #UDs
	                                      ; VMLOAD when SVM is disabled)
	stgi                                  ; re-enable GIF. MUST run while SVME=1
	                                      ; (stgi #UDs once SVM is disabled -
	                                      ; r28c learned this the hard way).
	                                      ; GIF was cleared by #VMEXIT and only
	                                      ; VMRUN re-enables it on the normal
	                                      ; path; without stgi here a resumed
	                                      ; host context masks all interrupts,
	                                      ; NMI included, forever (IPI hang).
	mov ecx, 0C0000080h                   ; MSR_EFER (rcx dead from here)
	rdmsr
	btr eax, 12                           ; clear SVME: core is devirtualized
	wrmsr
	mov rdx, [rsp + 30h]                  ; rdx is dead (EFER high bits)
	mov cr3, rdx                          ; thread's CR3 (kernel VAs are global,
	                                      ; so the rest of the unwind is safe)
	mov rax, [rsp]                        ; vcpu
	mov dword ptr [rax + VCPU_INFO], 0    ; State = Off (Stop's poll unblocks)
	mov rcx, [rax + VCPU_GUEST_VMCB]      ; VMCB VA again (rcx was EFER index)
	movzx edx, byte ptr [rcx + 60h]       ; V_TPR = guest CR8 at exit time;
	mov cr8, rdx                          ; without this an IPI_LEVEL context
	                                      ; (cr8=15) resumes at cr8=0 and
	                                      ; interrupt delivery desyncs (0x80)
	add rax, VCPU_REGS                    ; Regs base (rax repurposed as pointer)
	push qword ptr [rax + GREG_RFLAGS]
	popfq                                 ; thread's rflags (IF back on - but
	                                      ; GIF is still OFF: nothing delivers)
	mov rcx, [rax + GREG_RCX]
	mov rdx, [rax + GREG_RDX]
	mov rsp, [rax + GREG_RSP]             ; thread's kernel stack
	push qword ptr [rax + GREG_RIP]       ; transient write below rsp - safe,
	                                      ; Windows x64 ABI has no red zone
	mov rax, [rax + GREG_RAX]             ; context rax
	ret                                   ; resume the interrupted thread

no_self_exit:
	mov rax, [rsp + 10h]
	vmsave rax             ; capture host MSR/segment changes made by handlers
	jmp enter_guest

vmrun_failed:
	mov rax, [rsp + 20h]   ; original rsp
	mov rsp, rax
	ret

exit_virtualization:
	; rax = GuestRegs base; switch to the interrupted context's stack and
	; resume at Extra2 as if nothing ever happened
	stgi                                  ; r28: resume as HOST - GIF was
	                                      ; cleared by the #VMEXIT and nothing
	                                      ; re-enables it on a host-side resume
	mov rsp, [rax + GREG_RSP]
	push qword ptr [rax + GREG_EXTRA2]   ; resume rip
	push qword ptr [rax + GREG_RAX]      ; context rax
	push qword ptr [rax + GREG_RFLAGS]   ; context rflags
	popfq
	pop rax
	ret

_svmb_vmm_loop Endp

; ---- stage-3 nthook detour: FRAME-FAITHFUL relay for real-kernel hooks ----
; Entered by the hidden-copy patch jump at the hooked function's entry, with
; the ORIGINAL caller's stack (rsp = original entry rsp). A C detour here
; would push its own frame and shift every rsp-relative access of the
; hooked body (r33: NtCreateFile returned c0000010 24/24 that way). This
; stub counts the hit and tail-jumps to the trampoline WITHOUT touching
; rsp, non-volatile GPRs, or anything but rax (dead at entry).
; r35: the hand-written relay (_svmb_nthook_detour) and its globals are
; RETIRED - InstallCallback now emits a per-hook stub on each hook's
; trampoline page (saves volatile GPRs, calls the managed C callback,
; restores, register-free jumps to the stolen prefix; override returns
; the callback's value). See npt_hook_mgr.cpp EmitCallbackStub.

End
