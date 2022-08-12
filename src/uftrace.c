// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2022 Rong Tao */
#include <stdlib.h>
#include <getopt.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>

#include <elf/elf_api.h>
#include <cli/cli_api.h>

#include <utils/log.h>
#include <utils/list.h>
#include <utils/compiler.h>
#include <utils/task.h>


struct config config = {
	.log_level = -1,
};

static pid_t target_pid = -1;
static struct task *target_task = NULL;

static const char *patch_object_file = NULL;

/* This is ftrace object file path, during 'make install' install to
 * /usr/share/elftools/, this macro is a absolute path of LSB relocatable file.
 *
 * see top level of CMakeLists.txt
 */
#if !defined(ELFTOOLS_FTRACE_OBJ_PATH)
# error "Need ELFTOOLS_FTRACE_OBJ_PATH"
#endif


static void print_help(void)
{
	printf(
	"\n"
	" Usage: uftrace [OPTION]... [FILE]...\n"
	"\n"
	" User space ftrace\n"
	"\n"
	" Mandatory arguments to long options are mandatory for short options too.\n"
	"\n"
	" Base argument:\n"
	"\n"
	"  -p, --pid           specify a process identifier(pid_t)\n"
	"\n"
	"\n"
	" Ftrace argument:\n"
	"\n"
	"  -j, --patch-obj     input a ELF 64-bit LSB relocatable object file.\n"
	"                      default: %s\n"
	"\n"
	"\n"
	" Common argument:\n"
	"\n"
	"  -l, --log-level     set log level, default(%d)\n"
	"                      EMERG(%d),ALERT(%d),CRIT(%d),ERR(%d),WARN(%d)\n"
	"\n"
	"                      NOTICE(%d),INFO(%d),DEBUG(%d)\n"
	"  -h, --help          display this help and exit\n"
	"  -v, --version       output version information and exit\n"
	"\n"
	" uftrace %s\n",
	ELFTOOLS_FTRACE_OBJ_PATH,
	config.log_level,
	LOG_EMERG, LOG_ALERT, LOG_CRIT, LOG_ERR, LOG_WARNING, LOG_NOTICE, LOG_INFO,
	LOG_DEBUG,
	elftools_version()
	);
	exit(0);
}

static int parse_config(int argc, char *argv[])
{
	struct option options[] = {
		{"pid",		required_argument,	0,	'p'},
		{"patch-obj",		required_argument,	0,	'j'},
		{"version",	no_argument,	0,	'v'},
		{"help",	no_argument,	0,	'h'},
		{"log-level",		required_argument,	0,	'l'},
	};

	while (1) {
		int c;
		int option_index = 0;
		c = getopt_long(argc, argv, "p:j:vhl:", options, &option_index);
		if (c < 0) {
			break;
		}
		switch (c) {
		case 'p':
			target_pid = atoi(optarg);
			break;
		case 'j':
			patch_object_file = optarg;
			break;
		case 'v':
			printf("version %s\n", elftools_version());
			exit(0);
		case 'h':
			print_help();
		case 'l':
			config.log_level = atoi(optarg);
			break;
		default:
			print_help();
		}
	}

	if (target_pid == -1) {
		fprintf(stderr, "Specify pid with -p, --pid.\n");
		exit(1);
	}

	if (!proc_pid_exist(target_pid)) {
		fprintf(stderr, "pid %d not exist.\n", target_pid);
		exit(1);
	}

	if (!patch_object_file) {
		fprintf(stderr, "Specify object -j, --patch-obj.\n");
		exit(1);
	}

	if (!fexist(patch_object_file) ||
		(ftype(patch_object_file) & FILE_ELF_RELO) != FILE_ELF_RELO) {
		fprintf(stderr, "%s is not ELF or ELF LSB relocatable.\n",
			patch_object_file);
		exit(1);
	}

	return 0;
}

static unsigned long obj_target_task_map_addr = 0;
static size_t obj_target_tasp_map_size = 0;


static __unused int mmap_object(struct task *task)
{
	int ret = 0;
	ssize_t obj_target_tasp_map_size = fsize(patch_object_file);
	int __unused map_fd;

	ret = task_attach(task->pid);
	if (ret != 0) {
		fprintf(stderr, "attach %d failed.\n", task->pid);
		return -1;
	}
	map_fd = task_open(task,
				(char *)patch_object_file,
				O_RDWR,
				0644);
	if (map_fd <= 0) {
		fprintf(stderr, "remote open failed.\n");
		return -1;
	}
	ldebug("New open. %d\n", map_fd);
	ret = task_ftruncate(task, map_fd, obj_target_tasp_map_size);
	if (ret != 0) {
		fprintf(stderr, "remote ftruncate failed.\n");
		goto close_ret;
	}
	obj_target_task_map_addr =
		task_mmap(task,
				0UL, obj_target_tasp_map_size,
				PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE, map_fd, 0);
	if (!obj_target_task_map_addr) {
		fprintf(stderr, "remote mmap failed.\n");
		goto close_ret;
	}

	update_task_vmas(task);
	dump_task_vmas(task);

close_ret:
	task_close(task, map_fd);
	task_detach(task->pid);

	return ret;
}

static __unused int munmap_object(struct task *task)
{
	if (obj_target_task_map_addr) {
		ldebug("unmmap. %lx\n", obj_target_task_map_addr);
		task_attach(task->pid);
		task_munmap(target_task,
			obj_target_task_map_addr, obj_target_tasp_map_size);
		task_detach(task->pid);
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int __unused ret = 0;

	elftools_init();

	parse_config(argc, argv);

	set_log_level(config.log_level);

	target_task = open_task(target_pid, FTO_ALL);

	if (!target_task) {
		fprintf(stderr, "open %d failed. %s\n", target_pid, strerror(errno));
		return 1;
	}

#if 1
	/* mmap relocatable object ELF file to target process */
	ret = mmap_object(target_task);
	if (ret != 0) {
		fprintf(stderr, "mmap %s failed.\n", patch_object_file);
		goto free_and_ret;
	}

free_and_ret:
	munmap_object(target_task);
	free_task(target_task);
#else
	dump_task_vmas(target_task);
#endif

	return 0;
}

