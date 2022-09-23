// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2022 Rong Tao */
#include <stdio.h>
#include <errno.h>
#include <malloc.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <limits.h>
#include <stdlib.h>
#include <elf.h>

#include <elf/elf_api.h>

#include "log.h"
#include "task.h"

#if defined(__x86_64__)
#include "arch/x86_64/regs.h"
#include "arch/x86_64/instruments.h"
#elif defined(__aarch64__)
#include "arch/aarch64/regs.h"
#include "arch/aarch64/instruments.h"
#endif

LIST_HEAD(tasks_list);

int open_pid_maps(pid_t pid)
{
	int ret;
	char maps[] = "/proc/1234567890/maps";

	snprintf(maps, sizeof(maps), "/proc/%d/maps", pid);
	ret = open(maps, O_RDONLY);
	if (ret <= 0) {
		lerror("open %s failed. %s\n", maps, strerror(errno));
		ret = -errno;
	}
	return ret;
}

int open_pid_mem(pid_t pid)
{
	char mem[] = "/proc/1234567890/mem";
	snprintf(mem, sizeof(mem), "/proc/%d/mem", pid);
	int memfd = open(mem, O_RDWR);
	if (memfd <= 0) {
		lerror("open %s failed. %s\n", mem, strerror(errno));
	}
	return memfd;
}

struct vma_struct *alloc_vma(struct task *task)
{
	struct vma_struct *vma = malloc(sizeof(struct vma_struct));
	assert(vma && "alloc vma failed.");
	memset(vma, 0x00, sizeof(struct vma_struct));

	vma->task = task;
	vma->type = VMA_NONE;

	list_init(&vma->node);

	vma->leader = NULL;
	list_init(&vma->siblings);

	return vma;
}

static inline int __vma_rb_cmp(struct rb_node *node, unsigned long key)
{
	struct vma_struct *vma = rb_entry(node, struct vma_struct, node_rb);
	struct vma_struct *new = (struct vma_struct *)key;

	if (new->end <= vma->start)
		return -1;
	else if (vma->start < new->end && vma->end > new->start)
		return 0;
	else if (vma->end <= new->start)
		return 1;

	assert(0 && "Try insert illegal vma.");
	return 0;
}

int insert_vma(struct task *task, struct vma_struct *vma,
	struct vma_struct *prev)
{
	if (prev && strcmp(prev->name_, vma->name_) == 0) {
		struct vma_struct *leader = prev->leader;

		vma->leader = leader;

		list_add(&vma->siblings, &leader->siblings);
	}

	list_add(&vma->node, &task->vmas);
	rb_insert_node(&task->vmas_rb, &vma->node_rb,
		__vma_rb_cmp, (unsigned long)vma);
	return 0;
}

int unlink_vma(struct task *task, struct vma_struct *vma)
{
	list_del(&vma->node);
	rb_erase(&vma->node_rb, &task->vmas_rb);

	list_del(&vma->siblings);

	return 0;
}

int free_vma(struct vma_struct *vma)
{
	if (!vma)
		return -1;

	free(vma);
	return 0;
}

static inline int __find_vma_cmp(struct rb_node *node, unsigned long vaddr)
{
	struct vma_struct *vma = rb_entry(node, struct vma_struct, node_rb);

	if (vma->start > vaddr)
		return -1;
	else if (vma->start <= vaddr && vma->end > vaddr)
		return 0;
	else
		return 1;
}

struct vma_struct *find_vma(struct task *task, unsigned long vaddr)
{
	struct rb_node * rnode =
		rb_search_node(&task->vmas_rb, __find_vma_cmp, vaddr);
	if (rnode) {
		return rb_entry(rnode, struct vma_struct, node_rb);
	}
	return NULL;
}

struct vma_struct *next_vma(struct task *task, struct vma_struct *prev)
{
	struct rb_node *next;

	next = prev?rb_next(&prev->node_rb):rb_first(&task->vmas_rb);

	return  next?rb_entry(next, struct vma_struct, node_rb):NULL;
}

unsigned long find_vma_span_area(struct task *task, size_t size)
{
	struct vma_struct *ivma;
	struct rb_node * rnode;

	for (rnode = rb_first(&task->vmas_rb); rnode; rnode = rb_next(rnode)) {
		ivma = rb_entry(rnode, struct vma_struct, node_rb);
		struct rb_node *next_node = rb_next(rnode);
		struct vma_struct *next_vma;
		if (!next_node) {
			return 0;
		}
		next_vma = rb_entry(next_node, struct vma_struct, node_rb);
		if (next_vma->start - ivma->end >= size) {
			return ivma->end;
		}
	}
	lerror("No space fatal in target process, pid %d\n", task->pid);
	return 0;
}

static unsigned int __perms2prot(char *perms)
{
	unsigned int prot = 0;

	if (perms[0] == 'r')
		prot |= PROT_READ;
	if (perms[1] == 'w')
		prot |= PROT_WRITE;
	if (perms[2] == 'x')
		prot |= PROT_EXEC;
	/* Ignore 'p'/'s' flag, we don't need it */
	return prot;
}

int free_task_vmas(struct task *task);

enum vma_type get_vma_type(const char *exe, const char *name)
{
	enum vma_type type = VMA_NONE;

	if (!strcmp(name, exe)) {
		type = VMA_SELF;
	} else if (!strncmp(basename((char*)name), "libc", 4)
		|| !strncmp(basename((char*)name), "libssp", 6)) {
		type = VMA_LIBC;
	} else if (!strncmp(basename((char*)name), "libelf", 6)) {
		type = VMA_LIBELF;
	} else if (!strcmp(name, "[heap]")) {
		type = VMA_HEAP;
	} else if (!strncmp(basename((char*)name), "ld-linux", 8)) {
		type = VMA_LD;
	} else if (!strcmp(name, "[stack]")) {
		type = VMA_STACK;
	} else if (!strcmp(name, "[vvar]")) {
		type = VMA_VVAR;
	} else if (!strcmp(name, "[vdso]")) {
		type = VMA_VDSO;
	} else if (!strcmp(name, "[vsyscall]")) {
		type = VMA_VSYSCALL;
	} else if (!strncmp(basename((char*)name), "lib", 3)) {
		type = VMA_LIB_DONT_KNOWN;
	} else if (strlen(name) == 0) {
		type = VMA_ANON;
	} else {
		type = VMA_NONE;
	}

	return type;
}

static bool elf_vma_is_interp_exception(struct vma_struct *vma)
{
	char *name = vma->name_;

	/* libc */
	if (!strncmp(name, "libc", 4) &&
	    !strncmp(name + strlen(name) - 3, ".so", 3))
		return true;

	/* some times, libc-xxx.so(like libssp.so.0) is linked to libssp.so.xx
	 */
	if (!strncmp(name, "libssp", 6)) {
		return true;
	}

	/* libpthread */
	if (!strncmp(name, "libpthread", 10) &&
	    !strncmp(name + strlen(name) - 3, ".so", 3))
		return true;

	/* libdl */
	if (!strncmp(name, "libdl", 5) &&
	    !strncmp(name + strlen(name) - 3, ".so", 3))
		return true;

	return false;
}

/* Only FTO_VMA_ELF flag will load VMA ELF
 */
int __unused vma_peek_phdr(struct vma_struct *vma)
{
	GElf_Ehdr ehdr = {};
	struct task *task = vma->task;
	unsigned long phaddr;
	unsigned int phsz = 0;
	int i;
	bool is_share_lib = true;
	unsigned long lowest_vaddr = ULONG_MAX;

	/* Check VMA type, and skip it */
	switch (vma->type) {
	case VMA_VVAR:
	case VMA_STACK:
	case VMA_VSYSCALL:
		lwarning("not support %s\n", VMA_TYPE_NAME(vma->type));
		return 0;
	default:
		break;
	}

	/* is ELF or already peek */
	if (vma->elf != NULL || vma->is_elf) {
		return 0;
	}

	/* Is not ELF? */
	if (memcpy_from_task(task, &ehdr, vma->start, sizeof(ehdr)) < sizeof(ehdr)) {
		lerror("Failed read from %lx:%s\n", vma->start, vma->name_);
		return -1;
	}

	/* If not ELF, return success */
	if (!check_ehdr_magic_is_ok(&ehdr)) {
		return 0;
	}

	ldebug("%lx %s is ELF\n", vma->start, vma->name_);

	/* VMA is ELF, handle it */
	vma->elf = malloc(sizeof(struct vma_elf));
	memset(vma->elf, 0x0, sizeof(struct vma_elf));

	/* Copy ehdr from load var */
	memcpy(&vma->elf->ehdr, &ehdr, sizeof(ehdr));

	phaddr = vma->start + vma->elf->ehdr.e_phoff;
	phsz = vma->elf->ehdr.e_phnum * sizeof(GElf_Phdr);

	// memshow(&vma->elf->ehdr, sizeof(ehdr));

	/* if no program headers, just return. we don't need it, such as:
	 * /usr/lib64/ld-linux-x86-64.so.2 has '.ELF' magic, but it's no phdr
	 * memshow() like:
	 *
	 * |.ELF............|
	 * |/lib64/./usr/lib|
	 * |64/.............|
	 * |................|
	 */
	if (phsz == 0) {
		lwarning("%s: no phdr, e_phoff %lx, skip it.\n",
			vma->name_, vma->elf->ehdr.e_phoff);
		free(vma->elf);
		return 0;
	}

	vma->elf->phdrs = malloc(phsz);
	if (!vma->elf->phdrs) {
		free(vma->elf);
		return -1;
	}

	if (memcpy_from_task(task, vma->elf->phdrs, phaddr, phsz) < phsz) {
		free(vma->elf->phdrs);
		free(vma->elf);
		lerror("Failed to read %s program header.\n", vma->name_);
		return -1;
	}

	vma->is_elf = true;

	/* If type of the ELF is not ET_DYN, this is definitely not a shared
	 * library.
	 */
	if (vma->elf->ehdr.e_type != ET_DYN) {
		is_share_lib = false;
		goto share_lib;
	}

	/*
	 * Now there are possibilities:
	 *   - either this is really a shared library
	 *   - or this is a position-independent executable
	 * To distinguish between them look for INTERP
	 * program header that mush be present in any valid
	 * executable or usually don't in shared libraries
	 * (notable exception - libc)
	 */
	for (i = 0; i < vma->elf->ehdr.e_phnum; i++) {
		/* Ok, looks like this is an executable */
		if (vma->elf->phdrs[i].p_type == PT_INTERP &&
			!elf_vma_is_interp_exception(vma)) {
			is_share_lib = false;
			goto share_lib;
		}
	}

share_lib:

	is_share_lib |= vma->type == VMA_LIBC;
	is_share_lib |= vma->type == VMA_LIB_DONT_KNOWN;

	vma->is_share_lib = is_share_lib;

	for (i = 0; i < vma->elf->ehdr.e_phnum; i++) {
		GElf_Phdr *phdr = &vma->elf->phdrs[i];
		unsigned long off;
		struct vma_struct *sibling, *tmpvma;

		switch (phdr->p_type) {
		case PT_LOAD:
			lowest_vaddr = lowest_vaddr <= phdr->p_vaddr
					? lowest_vaddr : phdr->p_vaddr;

			off = ALIGN_DOWN(phdr->p_vaddr, phdr->p_align);

			list_for_each_entry_safe(sibling, tmpvma,
				&vma->siblings, siblings) {

				if (sibling->offset == off)
					sibling->voffset = phdr->p_vaddr;
			}

			FALLTHROUGH;
		case PT_GNU_RELRO:
			break;
		}
	}

	if (lowest_vaddr == ULONG_MAX) {
		lerror("%s: unable to find lowest load address.\n", vma->name_);
		free(vma->elf->phdrs);
		free(vma->elf);
		vma->elf = NULL;
		vma->is_elf = false;
		vma->is_share_lib = false;
		return -1;
	}

	vma->elf->load_offset = vma->start - lowest_vaddr;

	linfo("%s vma start %lx, load_offset %lx\n",
		vma->name_, vma->start, vma->elf->load_offset);

	return 0;
}

void __unused vma_free_elf(struct vma_struct *vma)
{
	if (!vma->is_elf)
		return;

	free(vma->elf->phdrs);
	free(vma->elf);
}

unsigned long task_vma_symbol_value(struct symbol *sym)
{
	unsigned long addr = 0;
	struct vma_struct *vma_leader = sym->vma;

	if (vma_leader != vma_leader->leader) {
		lerror("Symbol vma must be leader.\n");
		return 0;
	}

	/* After get symbol's st_value from target process's memory, we need to
	 * handle shared library manually, for example, libc.so LOAD headers:
	 *
	 *  $ readelf -l /usr/lib64/libc.so.6
	 *  ...
	 *  LOAD           0x0000000000000000 0x0000000000000000 0x0000000000000000
	 *                 0x0000000000027ed8 0x0000000000027ed8  R      0x1000
	 *  LOAD           0x0000000000028000 0x0000000000028000 0x0000000000028000
	 *                 0x00000000001742fc 0x00000000001742fc  R E    0x1000
	 *  LOAD           0x000000000019d000 0x000000000019d000 0x000000000019d000
	 *                 0x0000000000057df8 0x0000000000057df8  R      0x1000
	 *  LOAD           0x00000000001f58d0 0x00000000001f68d0 0x00000000001f68d0
	 *                 0x0000000000004fb8 0x00000000000126e0  RW     0x1000
	 *
	 * That is to say, we have vma start address, LOAD offset and symbol value.
	 *
	 *   00007fd4c72f6000 vma start
	 *   0000000000028000 LOAD offset
	 *   00007fd4c733d3d0 gdb> p printf
	 *   000000000006f3d0 symbol 'printf' st_value
	 *
	 * How should we get 'printf' function virtual address? That is easy:
	 *
	 *   00007fd4c733d3d0 gdb> p printf
	 * - 00007fd4c72f6000 vma start
	 * = 00000000000473d0
	 * + 0000000000028000 LOAD offset
	 * = 000000000006f3d0 symbol 'printf' st_value
	 */
	if (vma_leader->is_share_lib) {
		unsigned long off = sym->sym.st_value;
		struct vma_struct *vma, *tmpvma;

		list_for_each_entry_safe(vma, tmpvma,
			&vma_leader->siblings, siblings) {

			if (off < vma->offset)
				break;
		}

		addr = vma->start + (off - vma->offset);

		ldebug("SYMBOL %s addr %lx\n", sym->name, addr);

	} else {
		addr = sym->sym.st_value;
	}

	return addr;
}

struct symbol *task_vma_find_symbol(struct task *task, const char *name)
{
	struct symbol tmp = {
		.name = (char *)name,
	};
	struct rb_node *node = rb_search_node(&task->vma_symbols,
						cmp_symbol_name, (unsigned long)&tmp);

	return node?rb_entry(node, struct symbol, node):NULL;
}

/* Insert OK, return 0, else return -1 */
int task_vma_link_symbol(struct task *task, struct symbol *s)
{
	struct rb_node *node = rb_insert_node(&task->vma_symbols, &s->node,
						cmp_symbol_name, (unsigned long)s);

	if (unlikely(node)) {
		lwarning("%s: symbol %s already exist\n", task->comm, s->name);
	} else {
		ldebug("%s: add symbol %s success.\n", task->comm, s->name);
	}

	return node?-1:0;
}

/**
 * load_self_vma_symbols - load self symbols from ELF file
 *
 * @vma - self vma
 */
static int load_self_vma_symbols(struct vma_struct *vma)
{
	int err = 0;
	struct task *task = vma->task;

	struct symbol *sym, *tmp;

	rbtree_postorder_for_each_entry_safe(sym, tmp,
		&task->exe_elf->symbols, node) {

		struct symbol *new;

		/* skip undefined symbols */
		if (is_undef_symbol(&sym->sym))
			continue;

		/* allocate a symbol, and add it to task struct */
		new = alloc_symbol(sym->name, &sym->sym);
		if (!new) {
			lerror("Alloc symbol failed, %s\n", sym->name);
			continue;
		}

		ldebug("SELF %s %lx\n", new->name, new->sym.st_value);

		new->vma = vma;

		err = task_vma_link_symbol(task, new);
		if (err) {
			free_symbol(new);
		}
	}

	return err;
}

int vma_load_all_symbols(struct vma_struct *vma)
{
	if (!vma->is_elf || !vma->elf)
		return 0;

	struct task *task = vma->task;
	struct rb_root __unused *root = &task->vma_symbols;

	int err = 0;
	size_t i;
	GElf_Dyn *dynamics = NULL;
	GElf_Phdr *phdr;
	GElf_Sym *syms = NULL;
	char *buffer = NULL;

	unsigned long __unused symtab_addr, strtab_addr;
	unsigned long __unused symtab_sz, strtab_sz;

	symtab_addr = strtab_addr = 0;
	symtab_sz = strtab_sz = 0;


	/* load all self symbols */
	if (vma->type == VMA_SELF) {
		return load_self_vma_symbols(vma);
	}

	for (i = 0; i < vma->elf->ehdr.e_phnum; i++) {
		if (vma->elf->phdrs[i].p_type == PT_DYNAMIC) {
			phdr = &vma->elf->phdrs[i];
			break;
		}
	}

	if (i == vma->elf->ehdr.e_phnum) {
		lerror("No PT_DYNAMIC in %s\n", vma->name_);
		return -1;
	}

	dynamics = malloc(phdr->p_memsz);
	assert(dynamics && "Malloc fatal.");

	err = memcpy_from_task(task, dynamics,
			vma->elf->load_offset + phdr->p_vaddr, phdr->p_memsz);
	if (err < phdr->p_memsz) {
		lerror("Task read mem failed, %lx.\n", vma->start + phdr->p_vaddr);
		goto out_free;
	}

	/* For each Dyn */
	for (i = 0; i < phdr->p_memsz / sizeof(GElf_Dyn); i++) {

		GElf_Dyn *curdyn = dynamics + i;

		switch (curdyn->d_tag) {

		case DT_SYMTAB:
			symtab_addr = curdyn->d_un.d_ptr;
			break;

		case DT_STRTAB:
			strtab_addr = curdyn->d_un.d_ptr;
			break;

		case DT_STRSZ:
			strtab_sz = curdyn->d_un.d_val;
			break;

		case DT_SYMENT:
			if (curdyn->d_un.d_val != sizeof(GElf_Sym)) {
				lerror("Dynsym entry size is %ld expected %ld\n",
					curdyn->d_un.d_val, sizeof(GElf_Sym));
				goto out_free;
			}
			break;
		default:
			break;
		}
	}

	symtab_sz = (strtab_addr - symtab_addr);

	if (strtab_sz == 0 || symtab_sz == 0) {
		memshow(dynamics, phdr->p_memsz);
		lwarning(
			"No strtab, p_memsz %ld, p_vaddr %lx. "
			"strtab(%lx) symtab(%lx) %s %lx\n",
			phdr->p_memsz, phdr->p_vaddr,
			strtab_addr, symtab_addr, vma->name_, vma->start);
	}

	buffer = malloc(symtab_sz + strtab_sz);
	assert(buffer && "Malloc fatal.");
	memset(buffer, 0x0, symtab_sz + strtab_sz);

	ldebug("%s: symtab_addr %lx, load_offset: %lx, vma_start %lx\n",
		vma->name_,
		symtab_addr,
		vma->elf->load_offset,
		vma->start);

	/* [vdso] need add load_offset or vma start address.
	 *
	 * $ readelf -S vdso.so
	 * There are 16 section headers, starting at offset 0xe98:
	 * Section Headers:
	 *  [Nr] Name              Type             Address           Offset
	 *       Size              EntSize          Flags  Link  Info  Align
	 *  [ 3] .dynsym           DYNSYM           00000000000001c8  000001c8
	 *       0000000000000138  0000000000000018   A       4     1     8
	 */
	if (vma->type == VMA_VDSO) {
		symtab_addr += vma->elf->load_offset;
	}

	err = memcpy_from_task(task, buffer, symtab_addr, strtab_sz + symtab_sz);
	if (err < strtab_sz + symtab_sz) {
		lerror("load symtab failed.\n");
		goto out_free_buffer;
	}

	ldebug("%s\n", vma->name_);
	// memshow(buffer, strtab_sz + symtab_sz);

	/* For each symbol */
	syms = (GElf_Sym *)buffer;

	for (i = 0; i < symtab_sz / sizeof(GElf_Sym); i++) {

		struct symbol __unused *s;

		GElf_Sym __unused *sym = syms + i;
		const char *symname = buffer + symtab_sz + syms[i].st_name;

		if (is_undef_symbol(sym) || strlen(symname) == 0) {
			continue;
		}

		ldebug("%s: %s\n", vma->name_, symname);

		/* allocate a symbol, and add it to task struct */
		s = alloc_symbol(symname, sym);
		if (!s) {
			lerror("Alloc symbol failed, %s\n", symname);
			continue;
		}

		s->vma = vma;

		err = task_vma_link_symbol(task, s);
		if (err) {
			free_symbol(s);
		}
	}


out_free_buffer:
	free(buffer);
out_free:
	free(dynamics);

	return 0;
}

int read_task_vmas(struct task *task, bool update)
{
	struct vma_struct *vma, *prev = NULL;
	int mapsfd;
	FILE *mapsfp;

	if (update) free_task_vmas(task);

	// open(2) /proc/PID/maps
	mapsfd = open_pid_maps(task->pid);
	if (mapsfd <= 0) {
		return -1;
	}
	lseek(mapsfd, 0, SEEK_SET);

	mapsfp = fdopen(mapsfd, "r");
	fseek(mapsfp, 0, SEEK_SET);
	do {
		unsigned long start, end, offset;
		unsigned int maj, min, inode;
		char perms[5], name_[256];
		int r;
		char line[1024];

		start = end = offset = maj = min = inode = 0;

		memset(perms, 0, sizeof(perms));
		memset(name_, 0, sizeof(name_));
		memset(line, 0, sizeof(line));

		if (!fgets(line, sizeof(line), mapsfp))
			break;

		r = sscanf(line, "%lx-%lx %s %lx %x:%x %d %255s",
				&start, &end, perms, &offset,
				&maj, &min, &inode, name_);
		if (r <= 0) {
			lerror("sscanf failed.\n");
			return -1;
		}

		vma = alloc_vma(task);

		vma->start = start;
		vma->end = end;
		memcpy(vma->perms, perms, sizeof(vma->perms));
		vma->prot = __perms2prot(perms);
		vma->offset = offset;
		vma->maj = maj;
		vma->min = min;
		vma->inode = inode;
		strncpy(vma->name_, name_, sizeof(vma->name_));
		vma->type = get_vma_type(task->exe, name_);

		// Find libc.so
		if (!task->libc_vma
			&& vma->type == VMA_LIBC
			&& vma->prot & PROT_EXEC) {
			ldebug("Get libc:\n");
			task->libc_vma = vma;
		}

		// Find [stack]
		if (!task->stack && vma->type == VMA_STACK) {
			task->stack = vma;
		}

		vma->leader = vma;

		insert_vma(task, vma, prev);

		prev = vma;
	} while (1);

	fclose(mapsfp);
	close(mapsfd);

	return 0;
}

int update_task_vmas(struct task *task)
{
	return read_task_vmas(task, true);
}

void print_vma(struct vma_struct *vma)
{
	if (!vma) {
		lerror("Invalide pointer.\n");
		return;
	}
	printf(
		"%10s: %016lx-%016lx %6s %8lx %8lx %4x:%4x %8d %s %s %s %s\n",
		VMA_TYPE_NAME(vma->type),
		vma->start, vma->end, vma->perms,
		vma->offset,
		vma->voffset,
		vma->maj, vma->min, vma->inode, vma->name_,
		vma->is_elf ? "E" : " ",
		vma->is_share_lib ? "S" : " ",
		vma->leader==vma ? "L" : " "
	);
}

int dump_task(const struct task *task)
{
	printf(
		"COMM: %s\n"
		"PID:  %d\n"
		"EXE:  %s\n",
		task->comm,
		task->pid,
		task->exe
	);

	return 0;
}

void dump_task_vmas(struct task *task)
{
	struct vma_struct *vma;

	list_for_each_entry(vma, &task->vmas, node) {
		print_vma(vma);
	}
	printf(
		"\n"
		"(E)ELF, (S)SharedLib, (L)Leader\n"
	);
}

int free_task_vmas(struct task *task)
{
	struct vma_struct *vma, *tmpvma;

	list_for_each_entry_safe(vma, tmpvma, &task->vmas, node) {
		unlink_vma(task, vma);
		free_vma(vma);
	}

	list_init(&task->vmas);
	rb_init(&task->vmas_rb);

	task->libc_vma = NULL;
	task->stack = NULL;

	return 0;
}

bool proc_pid_exist(pid_t pid)
{
	char path[128];

	snprintf(path, sizeof(path), "/proc/%d", pid);
	return fexist(path);
}

char *get_proc_pid_exe(pid_t pid, char *buf, size_t bufsz)
{
	ssize_t ret = 0;
	char path[128];

	snprintf(path, sizeof(path), "/proc/%d/exe", pid);
	ret = readlink(path, buf, bufsz);
	if (ret < 0) {
		lerror("readlink %s failed, %s\n", path, strerror(errno));
		return NULL;
	}
	return buf;
}

static int __get_comm(struct task *task)
{
	char path[128];
	ssize_t ret;
	FILE *fp = NULL;

	ret = snprintf(path, sizeof(path), "/proc/%d/comm", task->pid);
	if (ret < 0) {
		lerror("readlink %s failed, %s\n", path, strerror(errno));
		return -errno;
	}

	fp = fopen(path, "r");

	fscanf(fp, "%s", task->comm);

	fclose(fp);

	return 0;
}

static int __get_exe(struct task *task)
{
	char path[128], realpath[128];
	ssize_t ret;

	snprintf(path, sizeof(path), "/proc/%d/exe", task->pid);
	ret = readlink(path, realpath, sizeof(realpath));
	if (ret < 0) {
		lerror("readlink %s failed, %s\n", path, strerror(errno));
		return -errno;
	}
	realpath[ret] = '\0';
	task->exe = strdup(realpath);

	return 0;
}

struct task *open_task(pid_t pid, int flag)
{
	struct task *task = NULL;
	int memfd;

	memfd = open_pid_mem(pid);
	if (memfd <= 0) {
		return NULL;
	}


	task = malloc(sizeof(struct task));
	assert(task && "malloc failed");
	memset(task, 0x0, sizeof(struct task));

	list_init(&task->node);
	list_init(&task->vmas);
	rb_init(&task->vmas_rb);

	task->fto_flag = flag;
	task->pid = pid;
	__get_comm(task);
	__get_exe(task);
	task->proc_mem_fd = memfd;

	read_task_vmas(task, false);

	rb_init(&task->vma_symbols);

	if (!task->libc_vma || !task->stack) {
		lerror("No libc or stack founded.\n");
		goto free_task;
	}

	/* Load libc ELF file if needed
	 */
	if (flag & FTO_LIBC) {
		task->libc_elf = elf_file_open(task->libc_vma->name_);
		if (!task->libc_elf) {
			lerror("Open libc failed.\n");
			goto free_task;
		}
	}
	if (flag & FTO_SELF) {
		task->exe_elf = elf_file_open(task->exe);
		if (!task->exe_elf) {
			lerror("Open exe:%s failed.\n", task->exe);
			goto free_task;
		}
	}

	if (flag & FTO_VMA_ELF) {
		struct vma_struct *tmp_vma;
		task_for_each_vma(tmp_vma, task) {
			vma_peek_phdr(tmp_vma);
		}
	}

	if (flag & FTO_VMA_ELF_SYMBOLS) {
		struct vma_struct *tmp_vma;
		task_for_each_vma(tmp_vma, task) {
			vma_load_all_symbols(tmp_vma);
		}
	}

	/* Create a directory under ROOT_DIR */
	if (flag & FTO_PROC) {
		FILE *fp;
		char buffer[BUFFER_SIZE];

		/* ROOT_DIR/PID */
		snprintf(buffer, BUFFER_SIZE - 1, ROOT_DIR "/%d", task->pid);
		if (mkdirat(0, buffer, 0775) != 0 && errno != EEXIST) {
			lerror("mkdirat(2) for %d:%s failed.\n", task->pid, task->exe);
			goto free_task;
		}

		/* ROOT_DIR/PID/TASK_PROC_COMM */
		sprintf(buffer + strlen(buffer), "/" TASK_PROC_COMM);
		fp = fopen(buffer, "w");
		fprintf(fp, "%s", task->comm);
		fclose(fp);

		/* ROOT_DIR/PID/TASK_PROC_MAP_FILES */
		snprintf(buffer, BUFFER_SIZE - 1,
			ROOT_DIR "/%d/" TASK_PROC_MAP_FILES, task->pid);
		if (mkdirat(0, buffer, 0775) != 0 && errno != EEXIST) {
			lerror("mkdirat(2) for %d:%s failed.\n", task->pid, task->exe);
			goto free_task;
		}
	}

	/* All success, add task to global list
	 */
	list_add(&task->node, &tasks_list);

	return task;

free_task:
	free_task(task);
	return NULL;
}

static void rb_free_symbol(struct rb_node *node) {
	struct symbol *s = rb_entry(node, struct symbol, node);
	free_symbol(s);
}

int free_task(struct task *task)
{
	/* free in open_task(), node == NULL */
	if (!list_empty(&task->node))
		list_del(&task->node);

	close(task->proc_mem_fd);

	if (task->fto_flag & FTO_VMA_ELF) {
		struct vma_struct *tmp_vma;
		task_for_each_vma(tmp_vma, task) {
			vma_free_elf(tmp_vma);
		}
	}

	if (task->fto_flag & FTO_SELF)
		elf_file_close(task->exe);

	if (task->fto_flag & FTO_LIBC)
		elf_file_close(task->libc_vma->name_);

	if (task->fto_flag & FTO_PROC) {
		char buffer[BUFFER_SIZE];

		/* ROOT_DIR/PID/TASK_PROC_COMM */
		snprintf(buffer, BUFFER_SIZE - 1,
			ROOT_DIR "/%d/" TASK_PROC_COMM, task->pid);
		if (unlink(buffer) != 0) {
			lerror("unlink(%s) for %d:%s failed, %s.\n",
				buffer, task->pid, task->exe, strerror(errno));
		}

		/* ROOT_DIR/PID/TASK_PROC_MAP_FILES */
		snprintf(buffer, BUFFER_SIZE - 1,
			ROOT_DIR "/%d/" TASK_PROC_MAP_FILES, task->pid);
		if (rmdir(buffer) != 0) {
			lerror("rmdir(%s) for %d:%s failed, %s.\n",
				buffer, task->pid, task->exe, strerror(errno));
		}

		/* ROOT_DIR/PID */
		snprintf(buffer, BUFFER_SIZE - 1, ROOT_DIR "/%d", task->pid);
		if (rmdir(buffer) != 0) {
			lerror("rmdir(%s) for %d:%s failed, %s.\n",
				buffer, task->pid, task->exe, strerror(errno));
		}
	}

	/* Destroy symbols rb tree */
	rb_destroy(&task->vma_symbols, rb_free_symbol);

	free_task_vmas(task);
	free(task->exe);

	free(task);

	return 0;
}

int task_attach(pid_t pid)
{
	int ret;
	int status;

	ret = ptrace(PTRACE_ATTACH, pid, NULL, NULL);
	if (ret != 0) {
		lerror("Attach %d failed. %s\n", pid, strerror(errno));
		return -errno;
	}
	do {
		ret = waitpid(pid, &status, __WALL);
		if (ret < 0) {
			lerror("can't wait for pid %d\n", pid);
			return -errno;
		}
		ret = 0;

		/* We are expecting SIGSTOP */
		if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP)
			break;

		/* If we got SIGTRAP because we just got out of execve, wait
		 * for the SIGSTOP
		 */
		if (WIFSTOPPED(status))
			status = (WSTOPSIG(status) == SIGTRAP) ? 0 : WSTOPSIG(status);
		else if (WIFSIGNALED(status))
			/* Resend signal */
			status = WTERMSIG(status);

		ret = ptrace(PTRACE_CONT, pid, NULL, (void *)(uintptr_t)status);
		if (ret < 0) {
			lerror("can't cont tracee\n");
			return -errno;
		}
	} while (1);

	return ret;
}

int task_detach(pid_t pid)
{
	long rv;
	rv = ptrace(PTRACE_DETACH, pid, NULL, NULL);
	if (rv != 0) {
		lerror("Detach %d failed. %s\n", pid, strerror(errno));
		return -errno;
	}

	return rv;
}

static int __unused pid_write(int pid, void *dest, const void *src, size_t len)
{
	int ret = -1;
	unsigned char *s = (unsigned char *) src;
	unsigned char *d = (unsigned char *) dest;

	while (ROUND_DOWN(len, sizeof(unsigned long))) {
		if (ptrace(PTRACE_POKEDATA, pid, d, *(long *)s) == -1) {
			ret = -errno;
			goto err;
		}
		s += sizeof(unsigned long);
		d += sizeof(unsigned long);
		len -= sizeof(unsigned long);
	}

	if (len) {
		unsigned long tmp;
		tmp = ptrace(PTRACE_PEEKTEXT, pid, d, NULL);
		if (tmp == (unsigned long)-1 && errno)
			return -errno;
		memcpy(&tmp, s, len);

		ret = ptrace(PTRACE_POKEDATA, pid, d, tmp);
	}

	return 0;
err:
	return ret;
}

static int __unused pid_read(int pid, void *dst, const void *src, size_t len)
{
	int sz = len / sizeof(void *);
	unsigned char *s = (unsigned char *)src;
	unsigned char *d = (unsigned char *)dst;
	long word;

	while (sz-- != 0) {
		word = ptrace(PTRACE_PEEKTEXT, pid, s, NULL);
		if (word == -1 && errno) {
			return -errno;
		}

		*(long *)d = word;
		s += sizeof(long);
		d += sizeof(long);
	}

	return len;
}

int memcpy_from_task(struct task *task,
		void *dst, unsigned long task_src, ssize_t size)
{
	int ret = -1;
	ret = pread(task->proc_mem_fd, dst, size, task_src);
	if (ret <= 0) {
		lerror("pread(%d, %p, %ld, 0x%lx)=%d failed, %s\n",
			task->proc_mem_fd, dst, size, task_src, ret, strerror(errno));
		do_backtrace();
		return -errno;
	}
	return ret;
}

int memcpy_to_task(struct task *task,
		unsigned long task_dst, void *src, ssize_t size)
{
	int ret = -1;
	ret = pwrite(task->proc_mem_fd, src, size, task_dst);
	if (ret <= 0) {
		lerror("pwrite(%d, %p, %ld, 0x%lx)=%d failed, %s\n",
			task->proc_mem_fd, src, size, task_dst, ret, strerror(errno));
		do_backtrace();
		return -errno;
	}
	return ret;
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wuninitialized"
#pragma clang diagnostic ignored "-Wmaybe-uninitialized"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
static void
copy_regs(struct user_regs_struct *dst, struct user_regs_struct *src)
{
#define COPY_REG(x) dst->x = src->x
#if defined(__x86_64__)
	COPY_REG(r15);
	COPY_REG(r14);
	COPY_REG(r13);
	COPY_REG(r12);
	COPY_REG(rbp);
	COPY_REG(rbx);
	COPY_REG(r11);
	COPY_REG(r10);
	COPY_REG(r9);
	COPY_REG(r8);
	COPY_REG(rax);
	COPY_REG(rcx);
	COPY_REG(rdx);
	COPY_REG(rsi);
	COPY_REG(rdi);
#elif defined(__aarch64__)
	COPY_REG(regs[0]);
	COPY_REG(regs[1]);
	COPY_REG(regs[2]);
	COPY_REG(regs[3]);
	COPY_REG(regs[4]);
	COPY_REG(regs[5]);
	COPY_REG(regs[8]);
	COPY_REG(regs[29]);
	COPY_REG(regs[9]);
	COPY_REG(regs[10]);
	COPY_REG(regs[11]);
	COPY_REG(regs[12]);
	COPY_REG(regs[13]);
	COPY_REG(regs[14]);
	COPY_REG(regs[15]);
	COPY_REG(regs[16]);
	COPY_REG(regs[17]);
	COPY_REG(regs[18]);
	COPY_REG(regs[19]);
	COPY_REG(regs[20]);
#else
# error "Unsupport architecture"
#endif
#undef COPY_REG
}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

int wait_for_stop(struct task *task)
{
	int ret, status = 0;
	pid_t pid = task->pid;

	while (1) {
		ret = ptrace(PTRACE_CONT, pid, NULL, (void *)(uintptr_t)status);
		if (ret < 0) {
			print_vma(task->libc_vma);
			lerror("ptrace(PTRACE_CONT, %d, ...) %s\n",
				pid, strerror(ESRCH));
			return -1;
		}

		ret = waitpid(pid, &status, __WALL);
		if (ret < 0) {
			lerror("can't wait tracee %d\n", pid);
			return -1;
		}
		if (WIFSTOPPED(status))  {
			if (WSTOPSIG(status) == SIGSTOP ||
				WSTOPSIG(status) == SIGTRAP) {
				break;
			}
			if (WSTOPSIG(status) == SIGSEGV) {
				lerror("Child process %d segment fault.\n", pid);
				return -1;
			}
			status = WSTOPSIG(status);
			continue;
		}

		status = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
	}
	return 0;
}

int task_syscall(struct task *task, int nr,
		unsigned long arg1, unsigned long arg2, unsigned long arg3,
		unsigned long arg4, unsigned long arg5, unsigned long arg6,
		unsigned long *res)
{
	int ret;
	struct user_regs_struct old_regs, regs, __unused syscall_regs;
	unsigned char __syscall[] = {SYSCALL_INSTR};

#if defined(__aarch64__)
	struct iovec orig_regs_iov, regs_iov;

	orig_regs_iov.iov_base = &old_regs;
	orig_regs_iov.iov_len = sizeof(old_regs);
	regs_iov.iov_base = &regs;
	regs_iov.iov_len = sizeof(regs);
#endif

	SYSCALL_REGS_PREPARE(syscall_regs, nr, arg1, arg2, arg3, arg4, arg5, arg6);

	unsigned char orig_code[sizeof(__syscall)];
	unsigned long libc_base = task->libc_vma->start;

#if defined(__x86_64__)
	ret = ptrace(PTRACE_GETREGS, task->pid, NULL, &old_regs);
#elif defined(__aarch64__)
	ret = ptrace(PTRACE_GETREGSET, task->pid, (void*)NT_PRSTATUS,
			(void*)&orig_regs_iov);
#else
# error "Unsupport architecture"
#endif
	if (ret == -1) {
		lerror("ptrace(PTRACE_GETREGS, %d, ...) failed, %s\n",
			task->pid, strerror(errno));
		return -errno;
	}

	memcpy_from_task(task, orig_code, libc_base, sizeof(__syscall));

	memcpy_to_task(task, libc_base, __syscall, sizeof(__syscall));

	regs = old_regs;

	SYSCALL_IP(regs) = libc_base;

	copy_regs(&regs, &syscall_regs);

#if defined(__x86_64__)
	ret = ptrace(PTRACE_SETREGS, task->pid, NULL, &regs);
#elif defined(__aarch64__)
	ret = ptrace(PTRACE_SETREGSET, task->pid, (void*)NT_PRSTATUS,
			(void*)&regs_iov);
#else
# error "Unsupport architecture"
#endif
	if (ret == -1) {
		lerror("ptrace(PTRACE_SETREGS, %d, ...) failed, %s\n",
			task->pid, strerror(errno));
		ret = -errno;
		goto poke_back;
	}

	ret = wait_for_stop(task);
	if (ret < 0) {
		lerror("failed call to func\n");
		goto poke_back;
	}

#if defined(__x86_64__)
	ret = ptrace(PTRACE_GETREGS, task->pid, NULL, &regs);
#elif defined(__aarch64__)
	ret = ptrace(PTRACE_GETREGSET, task->pid, (void*)NT_PRSTATUS,
			(void*)&regs_iov);
#else
# error "Unsupport architecture"
#endif
	if (ret == -1) {
		lerror("ptrace(PTRACE_GETREGS, %d, ...) failed, %s\n",
			task->pid, strerror(errno));
		ret = -errno;
		goto poke_back;
	}

#if defined(__x86_64__)
	ret = ptrace(PTRACE_SETREGS, task->pid, NULL, &old_regs);
#elif defined(__aarch64__)
	ret = ptrace(PTRACE_SETREGSET, task->pid, (void*)NT_PRSTATUS,
			(void*)&orig_regs_iov);
#else
# error "Unsupport architecture"
#endif
	if (ret == -1) {
		lerror("ptrace(PTRACE_SETREGS, %d, ...) failed, %s\n",
			task->pid, strerror(errno));
		ret = -errno;
		goto poke_back;
	}

	syscall_regs = regs;
	*res = SYSCALL_RET(syscall_regs);

	ldebug("result %lx\n", *res);

poke_back:
	memcpy_to_task(task, libc_base, orig_code, sizeof(__syscall));
	return ret;
}

unsigned long task_mmap(struct task *task,
	unsigned long addr, size_t length, int prot, int flags,
	int fd, off_t offset)
{
	int ret;
	unsigned long result;

	ret = task_syscall(task,
			__NR_mmap, addr, length, prot, flags, fd, offset, &result);
	if (ret < 0) {
		return 0;
	}
	return result;
}

int task_munmap(struct task *task, unsigned long addr, size_t size)
{
	int ret;
	unsigned long result;

	ret = task_syscall(task,
			__NR_munmap, addr, size, 0, 0, 0, 0, &result);
	if (ret < 0) {
		return -1;
	}
	return result;
}

int task_msync(struct task *task, unsigned long addr, size_t length, int flags)
{
	int ret;
	unsigned long result;

	ret = task_syscall(task,
			__NR_msync, addr, length, flags, 0, 0, 0, &result);
	if (ret < 0) {
		return -1;
	}
	return result;
}

int task_msync_sync(struct task *task, unsigned long addr, size_t length)
{
	return task_msync(task, addr, length, MS_SYNC);
}
int task_msync_async(struct task *task, unsigned long addr, size_t length)
{
	return task_msync(task, addr, length, MS_ASYNC);
}

unsigned long task_malloc(struct task *task, size_t length)
{
	unsigned long remote_addr;
	remote_addr = task_mmap(task,
				0UL, length,
				PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (remote_addr == (unsigned long)MAP_FAILED) {
		lerror("Remote malloc failed, %d\n", remote_addr);
		return 0UL;
	}
	return remote_addr;
}

int task_free(struct task *task, unsigned long addr, size_t length)
{
	return task_munmap(task, addr, length);
}

int task_open(struct task *task, char *pathname, int flags, mode_t mode)
{
	char maybeislink[MAX_PATH], path[MAX_PATH];
	int ret;
	unsigned long result;

	unsigned long remote_fileaddr;
	ssize_t remote_filename_len = 0;

	if (!(flags|O_CREAT)) {
		ret = readlink(pathname, maybeislink, sizeof(maybeislink));
		if (ret < 0) {
			lwarning("readlink(3) failed.\n");
			return -1;
		}
		maybeislink[ret] = '\0';
		if (!realpath(maybeislink, path)) {
			lwarning("realpath(3) failed.\n");
			return -1;
		}
		ldebug("%s -> %s -> %s\n", pathname, maybeislink, path);
		pathname = path;
	}
	remote_filename_len = strlen(pathname) + 1;

	remote_fileaddr = task_malloc(task, remote_filename_len);

	memcpy_to_task(task, remote_fileaddr, pathname, remote_filename_len);

#if defined(__x86_64__)
	ret = task_syscall(task,
			__NR_open, remote_fileaddr, flags, mode, 0, 0, 0, &result);
#elif defined(__aarch64__)
	ret = task_syscall(task,
			__NR_openat, AT_FDCWD, remote_fileaddr, flags, mode, 0, 0, &result);
#else
# error "Error arch"
#endif

	task_free(task, remote_fileaddr, remote_filename_len);

	return result;
}

int task_close(struct task *task, int remote_fd)
{
	int ret;
	unsigned long result;
	ret = task_syscall(task,
			__NR_close, remote_fd, 0, 0, 0, 0, 0, &result);
	if (ret < 0) {
		return 0;
	}
	return result;
}

int task_ftruncate(struct task *task, int remote_fd, off_t length)
{
	int ret;
	unsigned long result;
	ret = task_syscall(task,
			__NR_ftruncate, remote_fd, length, 0, 0, 0, 0, &result);
	if (ret < 0) {
		return 0;
	}
	return result;
}

int task_fstat(struct task *task, int remote_fd, struct stat *statbuf)
{
	int ret, ret_fstat;
	unsigned long remote_statbuf;
	unsigned long result;

	/* Alloc stat struct from remote */
	remote_statbuf = task_malloc(task, sizeof(struct stat));

	/* Call fstat(2) */
	ret_fstat = task_syscall(task,
			__NR_fstat, remote_fd, remote_statbuf, 0, 0, 0, 0, &result);
	if (ret_fstat < 0) {
		lerror("fstat failed, ret %d, %ld\n", ret_fstat, result);
	}

	ret = memcpy_from_task(task, statbuf, remote_statbuf, sizeof(struct stat));
	if (ret != sizeof(struct stat)) {
		lerror("failed copy struct stat.\n");
	}
	task_free(task, remote_statbuf, sizeof(struct stat));

	return ret_fstat;
}

int task_prctl(struct task *task, int option, unsigned long arg2,
	unsigned long arg3, unsigned long arg4, unsigned long arg5)
{
	int ret;
	unsigned long result;

	ret = task_syscall(task,
		__NR_prctl, option, arg2, arg3, arg4, arg5, 0, &result);
	if (ret < 0) {
		return 0;
	}
	return result;
}

