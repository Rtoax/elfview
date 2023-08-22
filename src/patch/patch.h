// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2022-2023 CESTC, Co. Rong Tao <rongtao@cestc.cn> */
#ifndef __ELF_UPATCH_H
#define __ELF_UPATCH_H 1

#include <stdbool.h>
#include <gelf.h>

#include <utils/util.h>
#include <utils/compiler.h>

#include <patch/meta.h>


#if defined(__x86_64__)
#include <arch/x86_64/instruments.h>
#include <arch/x86_64/mcount.h>
#elif defined(__aarch64__)
#include <arch/aarch64/instruments.h>
#include <arch/aarch64/mcount.h>
#include <arch/aarch64/ftrace.h>
#endif


/* see linux:kernel/module-internal.h */
struct load_info {
	const char *name;

	GElf_Ehdr *hdr;
	unsigned long len;

	/* the VMA start address in target task/process address space */
	unsigned long target_hdr;
	struct task *target_task;
	/* Create ROOT_DIR/PID/TASK_PROC_MAP_FILES/patch-XXXXXX */
	char *patch_path;
	struct mmap_struct *patch_mmap;

	GElf_Shdr *sechdrs;
	char *secstrings, *strtab;
	unsigned long symoffs, stroffs, init_typeoffs, core_typeoffs;

	struct upatch_info *info;
	enum patch_type type;
	struct upatch_strtab upatch_strtab;

	struct {
		unsigned int
			sym,
			str,
			vers,
			upatch_strtab,
			info;
	} index;
};


/* ftrace */
#if defined(__x86_64__)
# define MCOUNT_INSN_SIZE	CALL_INSN_SIZE
#elif defined(__aarch64__)
/* A64 instructions are always 32 bits. */
# define MCOUNT_INSN_SIZE	BL_INSN_SIZE
#endif


#define PATCH_VMA_TEMP_PREFIX	"patch-"


bool is_ftrace_entry(char *func);

extern void _ftrace_mcount(void);
extern void _ftrace_mcount_return(void);

int mcount_entry(unsigned long *parent_loc, unsigned long child,
			struct mcount_regs *regs);
unsigned long mcount_exit(long *retval);

struct task;

int parse_load_info(const char *obj_from, const char *obj_to,
	struct load_info *info);
int setup_load_info(struct load_info *info);

int init_patch(struct task *task, const char *obj_file);
int delete_patch(struct task *task);

int apply_relocate_add(const struct load_info *info, GElf_Shdr *sechdrs,
	const char *strtab,	unsigned int symindex, unsigned int relsec);

#endif /* __ELF_UPATCH_H */
