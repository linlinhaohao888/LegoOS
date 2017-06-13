/*
 * Copyright (c) 2016-2017 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <lego/mm.h>
#include <lego/pid.h>
#include <lego/smp.h>
#include <lego/bug.h>
#include <lego/tty.h>
#include <lego/irq.h>
#include <lego/pci.h>
#include <lego/net.h>
#include <lego/init.h>
#include <lego/list.h>
#include <lego/slab.h>
#include <lego/time.h>
#include <lego/wait.h>
#include <lego/delay.h>
#include <lego/sched.h>
#include <lego/string.h>
#include <lego/atomic.h>
#include <lego/kernel.h>
#include <lego/binfmts.h>
#include <lego/cpumask.h>
#include <lego/nodemask.h>
#include <lego/spinlock.h>
#include <lego/irqdomain.h>
#include <lego/completion.h>

#include <lego/comp_processor.h>
#include <lego/comp_memory.h>

#include <asm/io.h>
#include <asm/asm.h>
#include <asm/page.h>
#include <asm/numa.h>
#include <asm/traps.h>
#include <asm/setup.h>

enum system_states system_state __read_mostly;

/* Screen information used by kernel */
struct screen_info screen_info;

/* Builtin command line from kconfig */
#ifdef CONFIG_CMDLINE_BOOL
static char __initdata builtin_cmdline[COMMAND_LINE_SIZE] = CONFIG_CMDLINE;
#endif

/* Untouched command line saved by head, passed from boot loader */
char __initdata boot_command_line[COMMAND_LINE_SIZE];

/* Concatenated command line from boot and builtin */
static char command_line[COMMAND_LINE_SIZE];

/* Setup configured maximum number of CPUs to activate */
unsigned int setup_max_cpus = NR_CPUS;

extern void calibrate_delay(void);

/* Defined in linker scripts */
extern const struct obs_kernel_param __sinitsetup[], __einitsetup[];

static int __init parse_kernel_param(char *param, char *val)
{
	const struct obs_kernel_param *p;

	for (p = __sinitsetup; p < __einitsetup; p++) {
		if (parameq(param, p->str)) {
			if (p->setup_func(val) != 0) {
				pr_warn("Malformed option '%s'\n", param);
				return -ENOENT;
			}
		}
	}
	return 0;
}

static void inline setup_nr_cpu_ids(void)
{
	nr_cpu_ids = find_last_bit(cpumask_bits(cpu_possible_mask), NR_CPUS) + 1;
}

#define MAX_INIT_ARGS	CONFIG_INIT_ENV_ARG_LIMIT
#define MAX_INIT_ENVS	CONFIG_INIT_ENV_ARG_LIMIT

static const char *argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
const char *envp_init[MAX_INIT_ENVS+2] = { "HOME=/", "TERM=linux", NULL, };

static int run_init_process(const char *init_filename)
{
	argv_init[0] = init_filename;
	return do_execve(init_filename,
		(const char *const *)argv_init,
		(const char *const *)envp_init);
}

/*
 * This is our first kernel thread,
 * which will run the first user-level progam init:
 */
static int kernel_init(void *unused)
{
	if (!run_init_process("/sbin/init"))
		/* Return to userspace */
		return 0;

	panic("No working init found!");

	return -EFAULT;
}

static void rest_init(void)
{
	kernel_thread(kernel_init, NULL, CLONE_FS);

#ifndef CONFIG_PREEMPT
	schedule();
#endif
}

asmlinkage void __init start_kernel(void)
{
	local_irq_disable();

	setup_task_stack_end_magic(&init_task);

	boot_cpumask_init();

	/* Prepare output first */
	tty_init();
	pr_info("%s", lego_banner);

#ifdef CONFIG_CMDLINE_BOOL
	if (builtin_cmdline[0]) {
		/* append boot loader cmdline to builtin */
		strlcat(builtin_cmdline, " ", COMMAND_LINE_SIZE);
		strlcat(builtin_cmdline, boot_command_line, COMMAND_LINE_SIZE);
		strlcpy(boot_command_line, builtin_cmdline, COMMAND_LINE_SIZE);
	}
#endif
	strlcpy(command_line, boot_command_line, COMMAND_LINE_SIZE);
	pr_info("Command line: %s\n", command_line);

	/* Things to be done before parsing cmdline */
	early_setup_arch();

	/* Parse setup parameters */
	parse_args(command_line, parse_kernel_param);

	/* Architecture-Specific Initialization */
	setup_arch();
	setup_nr_cpu_ids();
	setup_nr_node_ids();
	setup_per_cpu_areas();

	/*
	 * Init CPU0
	 * cpu_init() will use smp_processor_id(), which
	 * MUST come after setting up per-cpu areas
	 */
	cpu_init();

	/* Allocate pid mapping array */
	pid_init();

	/*
	 * JUST A NOTE:
	 * If we have any large bootmem allocations later (e.g. printk logbuf),
	 * they should go right before memory_init(), because memory_init()
	 * will reserve the reserved memblock, and free the free memblock
	 * to buddy allocator. No more large allocations will be possible.
	 */

	/*
	 * Build all memory managment data structures,
	 * buddy allocator is avaiable afterwards:
	 */
	memory_init();

	irq_init();

	timekeeping_init();
	time_init();
	calibrate_delay();

	/*
	 * Set up the scheduler prior starting any interrupts,
	 * such as timer interrupt. Full topology setup happens
	 * at smp_init(), but meanwhile we still have a functioning
	 * scheduler:
	 */
	sched_init();

	if (WARN(!irqs_disabled(),
		 "Interrupts were enabled *very* early, fixing it\n"))
		local_irq_disable();

	/*
	 * Boot all possible CPUs
	 */
	smp_prepare_cpus(setup_max_cpus);
	local_irq_enable();
	smp_init();

	/* Register binfmt handlers */
	exec_init();

	//ib_cache_setup();
	ib_mad_init();
	pr_info("after ib_mad_init\n");
	pr_info("before pci init\n");
	pci_init();
	pr_info("before lego ib init\n");
	lego_ib_init();
	//init_lwip();

#ifdef CONFIG_COMP_PROCESSOR
	processor_component_init();
#endif
#ifdef CONFIG_COMP_MEMORY
	memory_component_init();
#endif

	/* STOP DBEYOND THIS POINT */
	rest_init();
	cpu_idle();
}
