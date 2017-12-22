/*
 * Copyright (c) 2016-2017 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <lego/sched.h>
#include <processor/processor.h>

#ifdef CONFIG_DEBUG_FORK
#define fork_debug(fmt, ...)						\
	pr_debug("%s(cpu%d): " fmt "\n", __func__, smp_processor_id(),	\
		__VA_ARGS__)
#else
static inline void fork_debug(const char *fmt, ...) { }
#endif

/* Return 0 on success, other on failure */
int p2m_fork(struct task_struct *p, unsigned long clone_flags)
{
	struct p2m_fork_struct payload;
	int ret, retbuf;

	BUG_ON(!p);

	fork_debug("comm:%s, pid:%d, tgid:%d",
		p->comm, p->pid, p->tgid);

	payload.pid = p->pid;
	payload.tgid = p->tgid;
	payload.parent_tgid = p->real_parent->tgid;
	payload.clone_flags = clone_flags;
	memcpy(payload.comm, p->comm, TASK_COMM_LEN);

	ret = net_send_reply_timeout(p->home_node, P2M_FORK, &payload,
				sizeof(payload), &retbuf, sizeof(retbuf), false,
				DEF_NET_TIMEOUT);

	WARN(retbuf, ret_to_string(retbuf));
	return retbuf;
}
