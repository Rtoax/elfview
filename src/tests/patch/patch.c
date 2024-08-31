// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2022-2024 Rong Tao */
#include <errno.h>

#include <utils/log.h>
#include <utils/list.h>
#include <utils/util.h>
#include <utils/task.h>
#include <utils/disasm.h>
#include <elf/elf-api.h>
#include <patch/patch.h>

#include <tests/test-api.h>


struct patch_test_arg {
	void (*custom_mcount)(void);
	enum {
		REPLACE_MCOUNT,
		REPLACE_NOP,
	} replace;
};

extern void mcount(void);
extern void _mcount(void);

static int ret_TTWU = 0;

#define TTWU_FTRACE_RETURN	1

/* when mcount() be called at the first time, mcount's address will be parse
 * so that, if you don't access mcount, sym.st_value will be '0'
 */
#if defined(__x86_64__)
char const *mcount_str = "mcount";
const unsigned long mcount_addr = (unsigned long)mcount;
#elif defined(__aarch64__)
char const *mcount_str = "_mcount";
const unsigned long mcount_addr = (unsigned long)_mcount;
#endif

static void my_direct_func(void)
{
	linfo(">>>>> REPLACE mcount() <<<<<\n");
	ret_TTWU = TTWU_FTRACE_RETURN;
}

__opt_O0 int try_to_wake_up(struct task_struct *task, int mode, int wake_flags)
{
	linfo("TTWU emulate.\n");
	int ret = ret_TTWU;
	ret_TTWU = 0;
	return ret;
}

__opt_O0 int ulpatch_try_to_wake_up(struct task_struct *task, int mode,
				    int wake_flags)
{
#define ULPATCH_TTWU_RET	0xdead1234
	linfo("TTWU emulate, patched.\n");
	return ULPATCH_TTWU_RET;
}

static int direct_patch_ftrace_test(struct patch_test_arg *arg, int expect_ret)
{
	int ret = 0, test_ret;
	int flags;
	struct task_struct *task;
	struct symbol *rel_s = NULL;
	struct symbol *libc_s = NULL;
	unsigned long restore_addr, disasm_addr;
	size_t restore_size, disasm_size;


	test_ret = expect_ret;

	flags = FTO_VMA_ELF_FILE | FTO_RDWR;
	task = open_task(getpid(), flags);

	/**
	 * Try to find mcount symbol in target task address space, you need to
	 * access mcount before find_symbol("mcount"), otherwise, st_value will
	 * be zero.
	 *
	 * AArch64: bl <_mcount> is 0x94000000 before relocation
	 */
	rel_s = find_symbol(task->exe_elf, mcount_str, STT_FUNC);
	if (!rel_s) {
		lwarning("Not found %s symbol in %s\n", mcount_str, task->exe);
		/**
		 * Symbol mcount() in SELF elf is undef
		 */
		rel_s = find_undef_symbol(task->exe_elf, mcount_str, STT_FUNC);
		if (!rel_s) {
			lerror("Not found %s symbol in %s\n", mcount_str, task->exe);
			return -1;
		}
	}

	/**
	 * Try to find mcount in libc.so, some time, libc.so's symbols is very
	 * useful when you try to patch a running process or ftrace it. thus,
	 * this is a test.
	 */
	libc_s = find_symbol(task->libc_elf, mcount_str, STT_FUNC);
	if (!libc_s) {
		lerror("Not found mcount in %s\n", task->libc_elf->filepath);
		return -1;
	}

	dump_task(task, true);
	linfo("SELF: _mcount: st_value: %lx %lx\n", rel_s->sym.st_value,
		mcount_addr);
	linfo("LIBC: _mcount: st_value: %lx %lx\n", libc_s->sym.st_value,
		mcount_addr);

	try_to_wake_up(task, 0, 0);

	char orig_code[MCOUNT_INSN_SIZE];
	unsigned long addr = (unsigned long)arg->custom_mcount;
	/**
	 * Skip symbols whose symbol address length is longer than 4 bytes.
	 * After all, this method is designed to test 4-byte addresses.
	 */
	if ((addr & 0xFFFFFFFFUL) != addr) {
		lwarning("Not support address overflow 4 bytes length.\n");
		/* Skip, return expected value */
		return expect_ret;
	}

	disasm_addr = (unsigned long)try_to_wake_up;

#if defined(__x86_64__)

	unsigned long ip = (unsigned long)try_to_wake_up +
		x86_64_func_callq_offset(try_to_wake_up);

	union text_poke_insn insn;
	const char *new = NULL;

	restore_addr = ip;
	restore_size = MCOUNT_INSN_SIZE;
	disasm_size = ip - disasm_addr + MCOUNT_INSN_SIZE;

	switch (arg->replace) {
	case REPLACE_MCOUNT:
		new = ftrace_call_replace(&insn, ip, addr);
		break;
	case REPLACE_NOP:
		new = ftrace_nop_replace();
		break;
	}

	linfo("addr:%#0lx call:%#0lx\n", addr, ip);

	/* Store original code */
	ret = memcpy_from_task(task, orig_code, ip, MCOUNT_INSN_SIZE);
	if (ret == -1 || ret < MCOUNT_INSN_SIZE) {
		lerror("failed to memcpy, ret = %d.\n", ret);
	}

	fdisasm_arch(stdout, (void *)disasm_addr, disasm_size);

	ret = memcpy_to_task(task, ip, (void*)new, MCOUNT_INSN_SIZE);
	if (ret == -1 || ret != MCOUNT_INSN_SIZE) {
		lerror("failed to memcpy.\n");
	}

	fdisasm_arch(stdout, (void *)disasm_addr, disasm_size);

#elif defined(__aarch64__)

	/* TODO: how to get bl <_mcount> address (24) */
	unsigned long pc =
		(unsigned long)try_to_wake_up + aarch64_func_bl_offset(try_to_wake_up);
	uint32_t new = aarch64_insn_gen_branch_imm(pc,
						(unsigned long)arg->custom_mcount,
						AARCH64_INSN_BRANCH_LINK);

	restore_addr = pc;
	restore_size = MCOUNT_INSN_SIZE;
	disasm_size = pc - disasm_addr + MCOUNT_INSN_SIZE;

	linfo("pc:%#0lx new addr:%x, mcount_offset %x\n", pc, new,
		aarch64_func_bl_offset(try_to_wake_up));

	/* Store original code */
	ret = memcpy_from_task(task, orig_code, pc, MCOUNT_INSN_SIZE);
	if (ret == -1 || ret < MCOUNT_INSN_SIZE) {
		lerror("failed to memcpy, ret = %d.\n", ret);
	}

	fdisasm_arch(stdout, (void *)disasm_addr, disasm_size);

	/* application the patch */
	ftrace_modify_code(task, pc, 0, new, false);

	fdisasm_arch(stdout, (void *)disasm_addr, disasm_size);
#endif

	/**
	 * call again, custom_mcount() will be called. see macro ULPATCH_TEST
	 * code branch
	 */
	test_ret = try_to_wake_up(task, 1, 2);

	/* Restore original code */
	ret = memcpy_to_task(task, restore_addr, orig_code, restore_size);
	if (ret == -1 || ret < restore_size) {
		lerror("failed to memcpy, ret = %d.\n", ret);
	}

	fdisasm_arch(stdout, (void *)disasm_addr, disasm_size);

	close_task(task);

	return test_ret;
}

TEST(Patch, ftrace_direct, TTWU_FTRACE_RETURN)
{
	struct patch_test_arg arg = {
		.custom_mcount = my_direct_func,
		.replace = REPLACE_MCOUNT,
	};

	return direct_patch_ftrace_test(&arg, TTWU_FTRACE_RETURN);
}

TEST(Patch, ftrace_object, 0)
{
	struct patch_test_arg arg = {
		.custom_mcount = _ftrace_mcount,
		.replace = REPLACE_MCOUNT,
	};

	return direct_patch_ftrace_test(&arg, 0);
}

#if defined(__x86_64__)
TEST(Patch, ftrace_nop, 0)
{
	struct patch_test_arg arg = {
		.custom_mcount = NULL,
		.replace = REPLACE_NOP,
	};

	return direct_patch_ftrace_test(&arg, 0);
}
#endif

TEST(Patch, direct_jmp, 0)
{
	int ret = 0;
	int flags = FTO_VMA_ELF_FILE | FTO_RDWR;
	struct task_struct *task = open_task(getpid(), flags);

	unsigned long ip_pc = (unsigned long)try_to_wake_up;
	unsigned long addr = (unsigned long)ulpatch_try_to_wake_up;

	/**
	 * Skip symbols whose symbol address length is longer than 4 bytes.
	 * After all, this method is designed to test 4-byte addresses.
	 */
	if ((addr & 0xFFFFFFFFUL) != addr) {
		lwarning("Not support address overflow 4 bytes length.\n");
		return 0;
	}

#if defined(__x86_64__)
	union text_poke_insn insn;
	const char *new = NULL;

	new = ulpatch_jmpq_replace(&insn, ip_pc, addr);

	linfo("addr:%#0lx jmp:%#0lx\n", addr, ip_pc);

	try_to_wake_up(task, 1, 1);

	ret = memcpy_to_task(task, ip_pc, (void*)new, MCOUNT_INSN_SIZE);
	if (ret == -1 || ret != MCOUNT_INSN_SIZE) {
		lerror("failed to memcpy.\n");
	}
#elif defined(__aarch64__)
	uint32_t new = aarch64_insn_gen_branch_imm(ip_pc, addr, AARCH64_INSN_BRANCH_NOLINK);

	linfo("pc:%#0lx new addr:%#0x\n", ip_pc, new);

	try_to_wake_up(task, 1, 1);
	/* application the patch */
	ftrace_modify_code(task, ip_pc, 0, new, false);
#endif

	/* This will called patched function ulpatch_try_to_wake_up() */
	ret = try_to_wake_up(task, 1, 1);
	if (ret != ULPATCH_TTWU_RET)
		ret = -1;
	else
		ret = 0;

	close_task(task);
	return ret;
}

TEST(Patch, direct_jmp_table, 0)
{
	int ret = 0, test_ret = 0;
	int flags = FTO_VMA_ELF_FILE | FTO_RDWR;
	struct task_struct *task = open_task(getpid(), flags);

	unsigned long ip_pc = (unsigned long)try_to_wake_up;
	unsigned long addr = (unsigned long)ulpatch_try_to_wake_up;

	const char *new = NULL;
	char orig_code[sizeof(struct jmp_table_entry)];
	struct jmp_table_entry jmp_entry;

	jmp_entry.jmp = arch_jmp_table_jmp();
	jmp_entry.addr = addr;
	new = (void *)&jmp_entry;

	linfo("addr:%#0lx jmp:%#0lx\n", addr, ip_pc);

	try_to_wake_up(task, 1, 1);
	fdisasm_arch(stdout, (void *)ip_pc, sizeof(jmp_entry));

	/* Store original code */
	ret = memcpy_from_task(task, orig_code, ip_pc, sizeof(jmp_entry));
	if (ret == -1 || ret < sizeof(jmp_entry)) {
		lerror("failed to memcpy, ret = %d.\n", ret);
	}

	ret = memcpy_to_task(task, ip_pc, (void *)new, sizeof(jmp_entry));
	if (ret == -1 || ret < sizeof(jmp_entry)) {
		lerror("failed to memcpy, ret = %d.\n", ret);
	}

	fdisasm_arch(stdout, (void *)ip_pc, sizeof(jmp_entry));

	/* This will called patched function ulpatch_try_to_wake_up() */
	ret = try_to_wake_up(task, 1, 1);
	if (ret != ULPATCH_TTWU_RET)
		test_ret = -1;
	else
		test_ret = 0;

	/* Restore original code */
	ret = memcpy_to_task(task, ip_pc, orig_code, sizeof(jmp_entry));
	if (ret == -1 || ret < sizeof(jmp_entry)) {
		lerror("failed to memcpy, ret = %d.\n", ret);
	}

	fdisasm_arch(stdout, (void *)ip_pc, sizeof(jmp_entry));

	close_task(task);
	return test_ret;
}
