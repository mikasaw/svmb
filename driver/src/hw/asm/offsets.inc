; svmb - single source of truth for offsets shared between MASM and C++.
; C++ side locks these with static_assert (vcpu.h / regs.h). Never hardcode
; magic numbers in svm_entry.asm - include this file.

; ---- GuestRegs (core/regs.h) ----
GREG_RAX     EQU 170h
GREG_RCX     EQU 160h    ; r28: needed by the in-place devirt resume
GREG_RDX     EQU 158h    ; r28
GREG_RFLAGS  EQU 178h
GREG_RIP     EQU 180h
GREG_RSP     EQU 188h
GREG_EXTRA1  EQU 190h    ; devirt flag: &GuestRegs (set by HC_EXIT_VMM handler);
                         ; also the phase-1 resume rip consumed by save_or_load
GREG_EXTRA2  EQU 198h    ; devirt resume rip
GREG_SIZE    EQU 1A0h

; ---- VcpuContext (core/vcpu.h) ----
; round-27 exp3: GuestVmcb/HostVmcb/HSave are now POINTERS to independent
; physically-contiguous pages (svm-base pattern); the blob keeps only
; Info/Regs/VmmStack at their locked offsets.
VCPU_GUEST_VMCB EQU 0000h    ; VMCB* GuestVmcb
VCPU_HOST_VMCB  EQU 0008h    ; VMCB* HostVmcb
VCPU_HSAVE      EQU 0010h    ; u8*   HSave
VCPU_INFO       EQU 3000h
VCPU_REGS       EQU 4000h
VCPU_STACK      EQU 5000h
VCPU_STACK_SIZE EQU 6000h
VCPU_TOTAL      EQU 0B000h

; ---- VMCB control offsets (hw/vmcb.h) ----
VMCB_EXIT_CODE  EQU 070h
