/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/monitor.h>
#include <kern/sched.h>
#include <kern/cpu.h>
#include <kern/spinlock.h>

// an array of all the environments
struct Env *envs = NULL;		

// the currently executing environment
// this is replaced by a macro instead (?)
// struct Env *curenv = NULL;		

static struct Env *env_free_list;	// Free environment list
					// (linked by Env->env_link)

#define ENVGENSHIFT	12		// >= LOGNENV

// Global descriptor table.
//
// Set up global descriptor table (GDT) with separate segments for
// kernel mode and user mode.  Segments serve many purposes on the x86.
// We don't use any of their memory-mapping capabilities, but we need
// them to switch privilege levels. 
//
// The kernel and user segments are identical except for the DPL.
// To load the SS register, the CPL must equal the DPL.  Thus,
// we must duplicate the segments for the user and the kernel.
//
// In particular, the last argument to the SEG macro used in the
// definition of gdt specifies the Descriptor Privilege Level (DPL)
// of that descriptor: 0 for kernel and 3 for user.
//
struct Segdesc gdt[NCPU + 5] =
{
	// 0x0 - unused (always faults -- for trapping NULL far pointers)
	SEG_NULL,

	// 0x8 - kernel code segment
	[GD_KT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 0),

	// 0x10 - kernel data segment
	[GD_KD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 0),

	// 0x18 - user code segment
	[GD_UT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 3),

	// 0x20 - user data segment
	[GD_UD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 3),

	// Per-CPU TSS descriptors (starting from GD_TSS0) are initialized
	// in trap_init_percpu()
	[GD_TSS0 >> 3] = SEG_NULL
};

struct Pseudodesc gdt_pd = {
	sizeof(gdt) - 1, (unsigned long) gdt
};

//
// Converts an envid to an env pointer.
// If checkperm is set, the specified environment must be either the
// current environment or an immediate child of the current environment.
//
// RETURNS
//   0 on success, -E_BAD_ENV on error.
//   On success, sets *env_store to the environment.
//   On error, sets *env_store to NULL.
//
int
envid2env(envid_t envid, struct Env **env_store, bool checkperm)
{
	struct Env *e;

	// If envid is zero, return the current environment.
	if (envid == 0) {
		*env_store = curenv;
		return 0;
	}

	// Look up the Env structure via the index part of the envid,
	// then check the env_id field in that struct Env
	// to ensure that the envid is not stale
	// (i.e., does not refer to a _previous_ environment
	// that used the same slot in the envs[] array).
	e = &envs[ENVX(envid)];
	if (e->env_status == ENV_FREE || e->env_id != envid) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	// Check that the calling environment has legitimate permission
	// to manipulate the specified environment.
	// If checkperm is set, the specified environment
	// must be either the current environment
	// or an immediate child of the current environment.
	if (checkperm && e != curenv && e->env_parent_id != curenv->env_id) {
		*env_store = 0;
		return -E_BAD_ENV;
	}

	*env_store = e;
	return 0;
}

// Mark all environments in 'envs' as free, set their env_ids to 0,
// and insert them into the env_free_list.
// Make sure the environments are in the free list in the same order
// they are in the envs array (i.e., so that the first call to
// env_alloc() returns envs[0]).
//
void
init_environments(void)
{
	struct Env *cur;
	for (cur = envs + NENV - 1; cur >= envs; cur--) {
		cur->env_status = ENV_FREE;
		cur->env_id = 0;
		cur->env_link = env_free_list;
		env_free_list = cur;
	}

	// Per-CPU part of the initialization
	env_init_percpu();
}

// Load GDT and segment descriptors.
void
env_init_percpu(void)
{
	lgdt(&gdt_pd);
	// The kernel never uses GS or FS, so we leave those set to
	// the user data segment.
	asm volatile("movw %%ax,%%gs" : : "a" (GD_UD|3));
	asm volatile("movw %%ax,%%fs" : : "a" (GD_UD|3));
	// The kernel does use ES, DS, and SS.  We'll change between
	// the kernel and user data segments as needed.
	asm volatile("movw %%ax,%%es" : : "a" (GD_KD));
	asm volatile("movw %%ax,%%ds" : : "a" (GD_KD));
	asm volatile("movw %%ax,%%ss" : : "a" (GD_KD));
	// Load the kernel text segment into CS.
	asm volatile("ljmp %0,$1f\n 1:\n" : : "i" (GD_KT));
	// For good measure, clear the local descriptor table (LDT),
	// since we don't use it.
	lldt(0);
}

//
// Given an environment, allocate its page directory, then set up the kernel
// space part of it.
//
// Returns 0 on success, < 0 on error.  Errors include:
//	-E_NO_MEM if page directory or table could not be allocated.
//
static int
env_setup_vm(struct Env *env)
{
	struct PageInfo *user_pgdir, *p;
	int i;

	// Allocate a page for the page directory
	if (!(user_pgdir = page_alloc(ALLOC_ZERO)))
		return -E_NO_MEM;
	
	// Now, set env->env_pgdir and initialize the user page directory.
	env->env_pgdir = page2kva(user_pgdir);

	// incref it so that env_free will properly free it
	page_incref(user_pgdir);

	// initialize the user pgdir to be identical to the kernel pgdir for now.
	// This is necessary for several reasons:
	// 
	// (1) when we switch cr3 we are still running kernel-land code, so if the
	// code isn't mapped in the user pgdir, it gets "pulled out from
	// underneath us" resulting in a segfault, and
	// (2) when an exception occurs, the interrupt handler needs use the
	// kernel stack, which hence needs to be mapped.
	// 
	// This means that userland programs can access kernel space. We need a
	// way to prevent this, perhaps with segmentation (GD_UD etc.)? TODO.
	// 
	// Actually we must be careful only to copy the part above UTOP. Otherwise
	// we'll accidentally map some pages into our address space which
	// shouldn't be there. They would then wrongly get freed once a process
	// exits.
	for (i = PDX(UTOP); i < NPDENTRIES; i++) {
		pde_t pde  = kern_pgdir[i];
		if (!(pde & PTE_P))
			continue;

		// allocate a separate second-level page in the page table; a process
		// can *NOT* share the second level of its page table with the kernel,
		// because then all processes would share the second level, and that's
		// nonsense.
		if (!(p = page_alloc(ALLOC_ZERO)))
			return -E_NO_MEM;

		// copy the second-level page
		memcpy(page2kva(p), KADDR(PTE_ADDR(pde)), PGSIZE);

		page_incref(p);
		env->env_pgdir[i] = PTE_FLAGS(pde) | page2pa(p);
	}

	// these should be initialized via the kernel page table
	assert (env->env_pgdir[PDX(UPAGES)] & PTE_P);
	assert (env->env_pgdir[PDX(UENVS)] & PTE_P);

	// this is also needed, otherwise we'll segfault when we switch cr3
	// because the code gets "pulled out under us".
	assert (env->env_pgdir[PDX(KERNBASE)] & PTE_P);

	// UVPT maps the env's own page table read-only.
	// Permissions: kernel R, user R
	env->env_pgdir[PDX(UVPT)] = PADDR(env->env_pgdir) | PTE_P | PTE_U;

	return 0;
}

void init_trapframe(struct Trapframe *tf) {
	// Set up appropriate initial values for the segment registers.
	// GD_UD is the user data segment selector in the GDT, and
	// GD_UT is the user text segment selector (see inc/memlayout.h).
	// The low 2 bits of each segment register contains the
	// Requestor Privilege Level (RPL); 3 means user mode.  When
	// we switch privilege levels, the hardware does various
	// checks involving the RPL and the Descriptor Privilege Level
	// (DPL) stored in the descriptors themselves.
	tf->tf_ds = GD_UD | 3;
	tf->tf_es = GD_UD | 3;
	tf->tf_ss = GD_UD | 3;
	tf->tf_cs = GD_UT | 3;

	// Enable interrupts while in user mode.
	tf->tf_eflags |= FL_IF;
}

//
// Allocates and initializes a new environment.
// On success, the new environment is stored in *newenv_store.
//
// Returns 0 on success, < 0 on failure.  Errors include:
//	-E_NO_FREE_ENV if all NENVS environments are allocated
//	-E_NO_MEM on memory exhaustion
//
int
env_alloc(struct Env **newenv_store, envid_t parent_id)
{
	int32_t generation;
	int r;
	struct Env *e;

	if (!(e = env_free_list))
		return -E_NO_FREE_ENV;

	// Allocate and set up the page directory for this environment.
	if ((r = env_setup_vm(e)) < 0)
		return r;

	// Generate an env_id for this environment.
	generation = (e->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
	if (generation <= 0)	// Don't create a negative env_id.
		generation = 1 << ENVGENSHIFT;
	e->env_id = generation | (e - envs);

	// Set the basic status variables.
	e->env_parent_id = parent_id;
	e->env_type = ENV_TYPE_USER;
	e->env_status = ENV_RUNNABLE;
	e->env_runs = 0;

	// Clear out all the saved register state,
	// to prevent the register values
	// of a prior environment inhabiting this Env structure
	// from "leaking" into our new environment.
	memset(&e->env_tf, 0, sizeof(e->env_tf));

	init_trapframe(&e->env_tf);

	e->env_tf.tf_esp = USTACKTOP;
	// we will set e->env_tf.tf_eip later.

	// Clear the page fault handler until user installs one.
	e->env_pgfault_upcall = 0;

	// Also clear the IPC receiving flag.
	e->env_ipc_recving = 0;

	e->in_v86_mode = false;

	// commit the allocation
	env_free_list = e->env_link;
	*newenv_store = e;

	// cprintf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, e->env_id);
	return 0;
}

//
// Allocate len bytes of physical memory for environment env,
// and map it at virtual address va in the environment's address space.
// Does not zero or otherwise initialize the mapped pages in any way.
// Pages will be made writable by both user and kernel.
// Panic if any allocation attempt fails.
//
static void
region_alloc(pde_t *pgdir, uintptr_t va, size_t len)
{
	uintptr_t cur, end = ROUNDUP(va + len, PGSIZE);

	// check whether the required pages are already mapped, and if not, add
	// them to the pgdir.
	for (cur = va; cur < end; cur += PGSIZE) {
		struct PageInfo *pp = page_lookup(pgdir, (void *) cur, NULL);

		// is the page already allocated?
		if (pp)
			continue;

		if (!(pp = page_alloc(0)))
			goto bad;

		if (page_insert(pgdir, pp, (void *) cur, PTE_U | PTE_W | PTE_P))
			goto bad;
	}

	return;

bad:
	panic("failed allocation during region_alloc");
}

// This function is given a single program header from an ELF file.
// The program header specifies a segment which should be loaded.
// This function does so, adding the segment to the given page directory.
static void 
load_segment(pde_t *pgdir, uint8_t *binary, struct Proghdr *ph) {
	size_t memsize = ROUNDUP(ph->p_memsz, PGSIZE);
	size_t filesize = ph->p_filesz;
	uint8_t *src = binary + ph->p_offset;
	uintptr_t va = ph->p_va;

	assert (filesize <= memsize);

	// allocate the segment
	region_alloc(pgdir, va, memsize);

	// zero out the memory
	memset((void *) va, '\0', memsize);

	// initialize (some of) the memory
	memcpy((uint8_t *) va, src, filesize);
}

//
// All in all: loads an ELF file into a user address space, updating 'env'
// accordingly.
//
// Set up the initial program binary, stack, and processor flags
// for a user process.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
//
// This function loads all loadable segments from the ELF binary image
// into the environment's user memory, starting at the appropriate
// virtual addresses indicated in the ELF program header.
// At the same time it clears to zero any portions of these segments
// that are marked in the program header as being mapped
// but not actually present in the ELF file - e.g., the program's bss section.
//
// Finally, this function maps one page for the program's initial stack.
//
// load_icode panics if it encounters problems.
//
static void
load_icode(struct Env *env, uint8_t *binary)
{
	pde_t *user_pgdir = env->env_pgdir;

	struct Proghdr *ph, *ph_end;
	struct Elf *elf = (struct Elf *) binary;

	if (elf->e_magic != ELF_MAGIC)
		panic("not a valid ELF file");
	
	// parse the ELF file, loading one segment at a time

	// switch to the userland page directory first, so that we can copy data
	// directly with memcpy during load_segment
	lcr3(PADDR(user_pgdir));

	// perform the actual loading of each segment
	// TODO: add size checks so we don't go oob
	ph = (struct Proghdr *) (binary + elf->e_phoff);
	ph_end = ph + elf->e_phnum;
	for (; ph < ph_end; ph++) {
		if (ph->p_type == ELF_PROG_LOAD)
			load_segment(user_pgdir, binary, ph);
	}

	// switch back to the kernel page directory
	lcr3(PADDR(kern_pgdir));

	// initialize the trap frame according to the ELF entry point
	// so that the environment starts executing at the right place
	assert(elf->e_entry < UTOP);
	env->env_tf.tf_eip = elf->e_entry;

	// Now map one page for the program's initial stack
	// at virtual address USTACKTOP - PGSIZE.
	struct PageInfo *pp = page_alloc(ALLOC_ZERO);
	if (!pp) 
		goto bad;
	if (page_insert(user_pgdir, pp, (void *) (USTACKTOP - PGSIZE), 
					PTE_P | PTE_U | PTE_W)) 
		goto bad;
	
	// set up the trap frame with the new stack
	env->env_tf.tf_esp = USTACKTOP;

	// fields like 'ss' and 'ds' were already set in env_alloc.

	return;

bad:
	panic("allocation failed during load_icode");
}

//
// Allocates a new env with env_alloc, loads the named elf
// binary into it with load_icode, and sets its env_type.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
// The new env's parent ID is set to 0.
//
void
env_create(uint8_t *binary, enum EnvType type)
{
	struct Env *env;

	envid_t parent_id = 0;

	int res = env_alloc(&env, parent_id);
	if (res)
		panic("env_alloc failed: %e", res);
	
	load_icode(env, binary);

	env->env_type = type;

	// If this is the file server, give it I/O privileges.
	if (type == ENV_TYPE_FS) {
		env->env_tf.tf_eflags |= FL_IOPL_3;
	}

	if (type == ENV_TYPE_V86) {
		env->env_tf.tf_eflags |= FL_IOPL_3;
	}

}

//
// Frees env e and all memory it uses.
//
void
env_free(struct Env *e)
{
	pte_t *pt;
	uint32_t pdeno, pteno;
	physaddr_t pa;

	// If freeing the current environment, switch to kern_pgdir
	// before freeing the page directory, just in case the page
	// gets reused.
	if (e == curenv)
		lcr3(PADDR(kern_pgdir));

	// Flush all mapped pages in the user portion of the address space
	static_assert(UTOP % PTSIZE == 0);
	for (pdeno = 0; pdeno < PDX(UTOP); pdeno++) {

		// only look at mapped page tables
		if (!(e->env_pgdir[pdeno] & PTE_P))
			continue;

		// find the pa and va of the page table
		pa = PTE_ADDR(e->env_pgdir[pdeno]);
		pt = (pte_t*) KADDR(pa);

		// unmap all PTEs in this page table
		for (pteno = 0; pteno <= PTX(~0); pteno++) {
			if (pt[pteno] & PTE_P)
				page_remove(e->env_pgdir, PGADDR(pdeno, pteno, 0));
		}

		// free the page table itself
		e->env_pgdir[pdeno] = 0;
		page_decref(pa2page(pa));
	}

	// free the page directory
	pa = PADDR(e->env_pgdir);
	e->env_pgdir = 0;
	page_decref(pa2page(pa));

	// return the environment to the free list
	e->env_status = ENV_FREE;
	e->env_link = env_free_list;
	env_free_list = e;
}

//
// Frees environment e.
// If e was the current env, then runs a new environment (and does not return
// to the caller).
//
void
env_destroy(struct Env *e)
{
	// If e is currently running on other CPUs, we change its state to
	// ENV_DYING. A zombie environment will be freed the next time
	// it traps to the kernel.
	if (e->env_status == ENV_RUNNING && curenv != e) {
		e->env_status = ENV_DYING;
		return;
	}

	env_free(e);

	if (curenv == e) {
		curenv = NULL;
		sched_yield();
	}
}


//
// Restores the register values in the Trapframe with the 'iret' instruction.
// This exits the kernel and starts executing some environment's code.
//
// This function does not return.
//
void
env_pop_tf(struct Trapframe *tf)
{
	// Record the CPU we are running on for user-space debugging
	curenv->env_cpunum = cpunum();

	asm volatile(
		"\tmovl %0,%%esp\n"
		"\tpopal\n"
		"\tpopl %%es\n"
		"\tpopl %%ds\n"
		"\taddl $0x8,%%esp\n" /* skip tf_trapno and tf_errcode */
		"\tiret\n"
		: : "g" (tf) : "memory");
	panic("iret failed");  /* mostly to placate the compiler */
}


#define ENABLE_SLOW_CHECKS 0

static void sanity_check_env(struct Env *e) {
	int i;

	if (!ENABLE_SLOW_CHECKS)
		return;
	
	// a process should not use the kernel pgdir.
	assert (e->env_pgdir != kern_pgdir);

	// a process should not share a pgdir with another process
	for (i = 0; i < NENV; i++) {
		struct Env *other = &envs[i];
		if (other == e)
			continue;
		assert (other->env_pgdir != e->env_pgdir);
	}
}

//
// Context switch from curenv to env.
//
// This function does not return.
//
void
env_run(struct Env *new)
{
	struct Env *old = curenv;
	// cprintf("switching to 0x%x\n", new->env_id);

	// make sure interrupts are enabled in user mode
	assert (!old || old->env_tf.tf_eflags & FL_IF);
	assert (new);
	assert (new->env_tf.tf_eflags & FL_IF);

	// update the old environment's status
	if (old && old->env_status == ENV_RUNNING)
		old->env_status = ENV_RUNNABLE;
	else
		; // nothing; it's ENV_DYING or such.
	
	// set the new environment as the current one and update its fields
	curenv = new;
	new->env_status = ENV_RUNNING;
	new->env_runs++;

	sanity_check_env(new);

	// unlock the big kernel lock before context switching to userland
	unlock_kernel();

	// switch to the new address space
	lcr3(PADDR(new->env_pgdir));

	// context switch to user mode
	env_pop_tf(&new->env_tf);
}

