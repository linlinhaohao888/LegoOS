/*
 * Copyright (c) 2016-2017 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <lego/pid.h>
#include <lego/timer.h>
#include <lego/ktime.h>
#include <lego/sched.h>
#include <lego/kernel.h>
#include <lego/kthread.h>
#include <lego/spinlock.h>
#include <lego/completion.h>
#include <lego/checkpoint.h>

#include <processor/include/fs.h>

#include "internal.h"

static LIST_HEAD(restorer_work_list);
static DEFINE_SPINLOCK(restorer_work_lock);
static struct task_struct *restorer_worker;

struct restorer_work_info {
	/* Info passed to restorer from restore_process_snapshot() */
	struct process_snapshot	*pss;

	/* Results passed back to restore_process_snapshot() from restorer */
	struct task_struct	*result;
	struct completion	*done;

	struct list_head	list;
};

/* It is really just a copy of sys_open() */
static int restore_sys_open(struct ss_files *ss_f)
{
	struct file *f;
	int fd, ret;
	char *f_name = ss_f->f_name;

	fd = alloc_fd(current->files, f_name);
	if (unlikely(fd != ss_f->fd)) {
		pr_err("Unmactched fd: %d:%s\n",
			ss_f->fd, ss_f->f_name);
		return -EBADF;
	}

	f = fdget(fd);
	f->f_flags = ss_f->f_flags;
	f->f_mode = ss_f->f_mode;

	if (unlikely(proc_file(f_name)))
		ret = proc_file_open(f, f_name);
	else if (unlikely(sys_file(f_name)))
		ret = sys_file_open(f, f_name);
	else
		ret = normal_file_open(f, f_name);

	if (ret) {
		free_fd(current->files, fd);
		goto put;
	}

	BUG_ON(!f->f_op->open);
	ret = f->f_op->open(f);
	if (ret)
		free_fd(current->files, fd);

put:
	put_file(f);
	return ret;
}

static int restore_open_files(struct process_snapshot *pss)
{
	unsigned int nr_files = pss->nr_files;
	struct files_struct *files = current->files;
	int fd, ret;
	struct file *f;
	struct ss_files *ss_f;

	for (fd = 0; fd < nr_files; fd++) {
		ss_f = &pss->files[fd];

		/*
		 * TODO
		 * Currently, Lego always open the 3 default
		 * STDIN, STDOUT, STDERR for newly created
		 * processes. But it may close the fd during
		 * runtime.. If so, we need to handle this.
		 */
		if (fd < 3 && test_bit(fd, files->fd_bitmap)) {
			f = files->fd_array[fd];
			BUG_ON(!f);

			if (strncmp(ss_f->f_name, f->f_name,
				FILENAME_LEN_DEFAULT)) {
				WARN(1, "Pacth needed here!");
				ret = -EBADF;
				goto out;
			}
			continue;
		}

		ret = restore_sys_open(ss_f);
		if (ret)
			goto out;
	}

out:
	return ret;
}

static void restore_signals(struct process_snapshot *pss)
{
	struct k_sigaction *k_action = current->sighand->action;
	struct sigaction *src, *dst;
	int i;

	for (i = 0; i < _NSIG; i++) {
		src = &pss->action[i];
		dst = &k_action[i].sa;
		memcpy(dst, src, sizeof(*dst));
	}

	memcpy(&current->blocked, &pss->blocked, sizeof(sigset_t));
}

static int restorer(void *_info)
{
	struct restorer_work_info *info = _info;
	struct process_snapshot *pss = info->pss;
	struct ss_task_struct *ss_task, *ss_tasks = pss->tasks;

#ifdef CONFIG_CHECKPOINT_DEBUG
	dump_task_struct(current, 0);
	dump_process_snapshot(pss, "Restorer", 0);
#endif

	/* Restore thread group shared data */
	memcpy(current->comm, pss->comm, TASK_COMM_LEN);
	restore_open_files(pss);
	restore_signals(pss);

#ifdef CONFIG_CHECKPOINT_DEBUG
	dump_task_struct(current, 0);
#endif

	/* Pass leader back to restore_process_snapshot() */
	info->result = current;
	complete(info->done);

	/* Return to user-space */
	return 0;
}

static void create_restorer(struct restorer_work_info *info)
{
	int pid;

	pid = kernel_thread(restorer, info, 0);
	if (pid < 0) {
		WARN_ON_ONCE(1);
		info->result = ERR_PTR(pid);
		complete(info->done);
	}
}

/*
 * It dequeue work from work_list, and creates a restorer to construct
 * a new process from snapshot. Any error is reported by restorer in
 * the info->result field.
 */
int restorer_worker_thread(void *unused)
{
	set_cpus_allowed_ptr(current, cpu_possible_mask);

	for (;;) {
		/* Sleep until someone wakes me up before september ends */
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (list_empty(&restorer_work_list))
			schedule();
		__set_current_state(TASK_RUNNING);

		spin_lock(&restorer_work_lock);
		while (!list_empty(&restorer_work_list)) {
			struct restorer_work_info *info;

			info = list_entry(restorer_work_list.next,
					struct restorer_work_info, list);
			list_del_init(&info->list);

			/*
			 * Release the lock so others can attach work.
			 * The real work may take some time.
			 */
			spin_unlock(&restorer_work_lock);

			create_restorer(info);

			spin_lock(&restorer_work_lock);
		}
		spin_unlock(&restorer_work_lock);
	}

	return 0;
}

/**
 * restore_process_snapshot	-	Restore a process from snapshot
 * @pss: the snapshot
 *
 * This function is synchronized. It will wait until the new process
 * is live from the snapshot. The real work of restoring is done by
 * restorer thread.
 *
 * Return the task_struct of new thread-group leader.
 * On failure, ERR_PTR is returned.
 */
struct task_struct *restore_process_snapshot(struct process_snapshot *pss)
{
	DEFINE_COMPLETION(done);
	struct restorer_work_info info;

	/*
	 * Note:
	 * If we decide to make this function a-sync later,
	 * we need to allocate info instead of using stack.
	 */
	info.pss = pss;
	info.done = &done;

	spin_lock(&restorer_work_lock);
	list_add_tail(&info.list, &restorer_work_list);
	spin_unlock(&restorer_work_lock);

	wake_up_process(restorer_worker);
	wait_for_completion(&done);

	return info.result;
}

void __init checkpoint_init(void)
{
	restorer_worker = kthread_run(restorer_worker_thread, NULL, "restorer");
	if (IS_ERR(restorer_worker))
		panic("Fail to create checkpointing restore thread!");
}