#include <inc/mmu.h>
#include <inc/memlayout.h>
#include <inc/trap.h>
#include <kern/graphics.h>



#define	RELOC(x) ((x) - KERNBASE)

.intel_syntax noprefix



# use this macro to switch to real mode
# the 'code_start' parameter must be a label pointing to the beginning of the
# code that's switching modes
.macro switch_to_real_mode code_start

	pushad
	
	# save the stack pointer for later
	mov eax, 0x7000
	mov [eax], esp

	# first we need to switch to real mode to use int 0x10.
	#     Disable the interrupts:
	#         Turn off maskable interrupts using CLI.
	#         Disable NMI (optional).
	cli

	#     Turn off paging:
	#         Transfer control to a 1:1 page.
	# - we already did this from C code
	#         Ensure that the GDT and IDT are in a 1:1 page.
	# - this should already be the case; we're using a bootstrap GDT, and the IDT
	#   isn't even loaded
	#         Clear the PG-flag in the zeroth control register.
	mov eax, cr0
	and eax, ~0x80000000
	mov cr0, eax

	#         Set the third control register to 0.
	xor eax, eax
	mov cr3, eax

	# Use GDT with 16-bit tables (skip this step if one is already available):
	#     Create a new GDT with a 16-bit data and code segment:
	#         Limit: 0xFFFFF
	#         Base: 0x0
	#         16-bit
	#         Privilege level: 0
	#         Granularity: 0
	#         Read and Write: 1
	#     Load new GDT ensuring that the currently used selectors will remain
	#     the same (index in cs/ds/ss will be copy of original segment in new
	#     GDT)
	mov eax, 0x9000 # the gdt descriptor must be there
	lgdt [eax]

	# Far jump to 16-bit protected mode:
	#     Far jump to 16-bit protected mode with a 16-bit segment index.
	ljmp 0x8:(0x8000 + 1f - \code_start)

1:

.code16
	
	#     Load data segment selectors with 16-bit indexes:
	#         Load ds, es, fs, gs, ss with a 16-bit data segment.
	mov ax, 0x10
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax

	#     Load real mode IDT:
	#         Limit: 0x3FF
	#         Base 0x0
	#         Use lidt
	mov ax, (0x9000 + realmode_idt - realmode_gdt)
	lidt [eax]

	#     Disable protected mode:
	#         Set PE bit in CR0 to false.
	mov eax, cr0
	and eax, 0x7FFFFFFe
	mov cr0, eax

	#     Far jump to real mode:
	#         Far jump to real mode with real mode segment selector (usually 0).
	ljmp 0x0:(0x8000 + 1f - \code_start)

1:

	#     Reload data segment registers with real mode values:
	#         Load ds, es, fs, gs, ss with appropriate real mode values (usually 0).
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax

	#     Set stack pointer to appropriate value:
	#         Set sp to stack value that will not interfere with real mode program.
	mov sp, 0xa000

	#     Enable interrupts:
	#         Enable maskable interrupts with STI.
	sti

	#     Continue on in real mode with all bios interrupts.

.endm # end of macro


.macro switch_to_protected_mode code_start

	# use a proper 32-bit GDT
	mov eax, (0x9000 + protmode_gdtdesc - realmode_gdt)
	lgdt [eax]

	# real mode -> protected mode
	mov eax, cr0
	or eax, 1
	mov cr0, eax

	# go to 32-bit mode
	ljmp 0x8:(0x8000 + 1f - \code_start)

.code32
1:
	
	# fix up the other segment selectors
	mov eax, 0x10
	mov ds, ax
	mov es, ax
	mov fs, ax
	mov gs, ax
	mov ss, ax

	# we need to re-enable paging
	lea eax, (RELOC(entry_pgdir))
	mov cr3, eax
	mov eax, cr0
	or eax, (CR0_PG | CR0_WP)
	mov cr0, eax

	# restore the trashed esp
	mov eax, 0x7000
	mov esp, [eax]

	# put return values temporarily at 0x7000
	mov [eax], ebx

	# restore all registers
	popad

	mov eax, 0x7000
	mov eax, [eax]

	# we should clear interrupts too, we don't allow those in kernel mode..
	cli

.endm # end of macro



.globl _get_video_mode
.type _get_video_mode, @function

_get_video_mode:
	
	# grab the argument (the mode offset)
	push ebx
	mov ebx, [esp + 8]

	switch_to_real_mode _get_video_mode

	push bx

	mov di, 0xc000
	mov eax, 0x32454256 	# "VBE2"
	mov dword [di], eax
	mov ax, 0x4F00				# get VBE BIOS info
	int 0x10

	cmp ax, 0x4F				# BIOS doesn't support VBE?
	jne .error

	mov bx, (0xc000 + 16) # get video_modes (.segment)
	mov fs, [bx]
	mov bx, (0xc000 + 14) # get video_modes (.offset)
	mov si, [bx]

	pop bx
	add si, bx # add the offset argument

	mov dx, fs:[si] # grab the mode

	cmp dx, 0xFFFF			# end of list?
	je .error

	mov ax, 0x4F01				# get VBE mode info
	mov cx, dx
	mov di, 0xd000
	int 0x10

	cmp ax, 0x4F
	je .success

.error:
	xor ebx, ebx
	jmp .finish

.success:
	mov ebx, 1

.finish:
	
	# we've got the video mode in 0xd000, so return.
	switch_to_protected_mode _get_video_mode

	pop ebx
	ret



.globl _set_video_mode
.type _set_video_mode, @function

_set_video_mode:

	# grab the argument (the mode offset)
	push ebx
	mov ebx, [esp + 8]

	switch_to_real_mode _set_video_mode

	push bx

	mov di, 0xc000
	mov eax, 0x32454256 	# "VBE2"
	mov dword [di], eax
	mov ax, 0x4F00				# get VBE BIOS info
	int 0x10

	mov bx, (0xc000 + 16) # get video_modes (.segment)
	mov fs, [bx]
	mov bx, (0xc000 + 14) # get video_modes (.offset)
	mov si, [bx]

	pop bx
	add si, bx

	mov ax, 0x4F02
	mov bx, fs:[si] # mode
	or bx, 0x4000			# enable LFB
	mov di, 0
	int 0x10

	cmp ax, 0x4F
	jne .error2

	jmp .success2

.error2:
	xor ebx, ebx
	jmp .finish2

.success2:
	mov ebx, 1

.finish2:

	switch_to_protected_mode _set_video_mode

	pop ebx
	ret



.globl realmode_gdt

realmode_gdt:

# table descriptor
.word   (3 * 8) - 1                 # GDT table limit
.long   0x9008            			# GDT table location
.word 0 # padding

# null descriptor
.long   0x0
.long   0x0
                
# cs descriptor
.word   0xFFFF                      # Limit 15:0
.word   0x0                         # Base 15:0
.byte   0x0                         # Base 23:16
.byte   0x9A                        # Access: Present, Ring 0, Code Segment
.byte   0x8F                        # Granularity: 1Kb, 16-bit mode
.byte   0x00                        # Base 31:24

# ds descriptor
.word   0xFFFF                      # Limit 15:0
.word   0x0                         # Base 15:0
.byte   0x0                         # Base 23:16
.byte   0x92                        # Access: Present, Ring 0, Data Segment
.byte   0x8F                        # Granularity: 1kB, 16-bit mode
.byte   0x00                        # Base 31:24


realmode_idt:
.word   0x3FF                       # IDT Table Limit
.long   0x0                         # IDT Table base



.p2align 2                                # force 4 byte alignment
protmode_gdt:
  SEG_NULL				# null seg
  SEG(STA_X|STA_R, 0x0, 0xffffffff)	# code seg
  SEG(STA_W, 0x0, 0xffffffff)	        # data seg

protmode_gdtdesc:
  .word   0x17                            # sizeof(gdt) - 1
  .long   (0x9000 + protmode_gdt - realmode_gdt)                             # address gdt




.att_syntax prefix

