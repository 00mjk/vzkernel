/*
 *  linux/kernel/ve/ve.c
 *
 *  Copyright (C) 2000-2005  SWsoft
 *  All rights reserved.
 *  
 *  Licensing governed by "linux/COPYING.SWsoft" file.
 *
 */

/*
 * 've.c' helper file performing VE sub-system initialization
 */

#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/capability.h>
#include <linux/ve.h>
#include <linux/init.h>

#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/sys.h>
#include <linux/kdev_t.h>
#include <linux/termios.h>
#include <linux/netdevice.h>
#include <linux/utsname.h>
#include <linux/proc_fs.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/rcupdate.h>
#include <linux/ve_proto.h>
#include <linux/devpts_fs.h>
#include <linux/user_namespace.h>
#include <linux/init_task.h>
#include <linux/mutex.h>
#include <linux/percpu.h>

#include <linux/vzcalluser.h>

unsigned long vz_rstamp = 0x37e0f59d;
EXPORT_SYMBOL(vz_rstamp);

#ifdef CONFIG_MODULES
struct module no_module = { .state = MODULE_STATE_GOING };
EXPORT_SYMBOL(no_module);
#endif

void (*do_env_free_hook)(struct ve_struct *ve);
EXPORT_SYMBOL(do_env_free_hook);

void do_env_free(struct ve_struct *env)
{
	BUG_ON(env->is_running);

	preempt_disable();
	do_env_free_hook(env);
	preempt_enable();
}
EXPORT_SYMBOL(do_env_free);

int (*do_ve_enter_hook)(struct ve_struct *ve, unsigned int flags);
EXPORT_SYMBOL(do_ve_enter_hook);

struct ve_struct ve0 = {
	.counter		= ATOMIC_INIT(1),
	.ve_list		= LIST_HEAD_INIT(ve0.ve_list),
	.start_jiffies		= INITIAL_JIFFIES,
	.ve_ns			= &init_nsproxy,
	.ve_netns		= &init_net,
	.user_ns		= &init_user_ns,
	.is_running		= 1,
	.op_sem			= __RWSEM_INITIALIZER(ve0.op_sem),
#ifdef CONFIG_VE_IPTABLES
	.ipt_mask		= VE_IP_ALL,	/* everything is allowed */
	._iptables_modules	= VE_IP_NONE,	/* but nothing yet loaded */
#endif
	.features		= -1,
	.meminfo_val		= VE_MEMINFO_SYSTEM,
	._randomize_va_space	=
#ifdef CONFIG_COMPAT_BRK
					1,
#else
					2,
#endif
	.devices		= LIST_HEAD_INIT(ve0.devices),
	.init_cred		= &init_cred,
	.sync_mutex		= __MUTEX_INITIALIZER(ve0.sync_mutex),
};

EXPORT_SYMBOL(ve0);

LIST_HEAD(ve_list_head);
DEFINE_MUTEX(ve_list_lock);

unsigned task_veid(struct task_struct *task)
{
	return task->task_ve->veid;
}
EXPORT_SYMBOL(task_veid);

struct ve_struct *__find_ve_by_id(envid_t veid)
{
	struct ve_struct *ve;

	for_each_ve(ve) {
		if (ve->veid == veid)
			return ve;
	}
	return NULL;
}
EXPORT_SYMBOL(__find_ve_by_id);

struct ve_struct *get_ve_by_id(envid_t veid)
{
	struct ve_struct *ve;
	mutex_lock(&ve_list_lock);
	ve = __find_ve_by_id(veid);
	get_ve(ve);
	mutex_unlock(&ve_list_lock);
	return ve;
}
EXPORT_SYMBOL(get_ve_by_id);

EXPORT_SYMBOL(ve_list_lock);
EXPORT_SYMBOL(ve_list_head);

static DEFINE_PER_CPU(struct kstat_lat_pcpu_snap_struct, ve0_lat_stats);

void init_ve0(void)
{
	struct ve_struct *ve;

	ve = get_ve0();
	ve->sched_lat_ve.cur = &ve0_lat_stats;
	list_add(&ve->ve_list, &ve_list_head);
}

int vz_security_family_check(struct net *net, int family)
{
	if (ve_is_super(net->owner_ve))
		return 0;

	switch (family) {
	case PF_UNSPEC:
	case PF_PACKET:
	case PF_NETLINK:
	case PF_UNIX:
	case PF_INET:
	case PF_INET6:
	case PF_PPPOX:
	case PF_KEY:
		return 0;
	default:
		return -EAFNOSUPPORT;
	}
}
EXPORT_SYMBOL_GPL(vz_security_family_check);

int vz_security_protocol_check(struct net *net, int protocol)
{
	if (ve_is_super(net->owner_ve))
		return 0;

	switch (protocol) {
	case  IPPROTO_IP:
	case  IPPROTO_TCP:
	case  IPPROTO_UDP:
	case  IPPROTO_RAW:
	case  IPPROTO_DCCP:
	case  IPPROTO_GRE:
	case  IPPROTO_ESP:
	case  IPPROTO_AH:
		return 0;
	default:
		return -EAFNOSUPPORT;
	}
}
EXPORT_SYMBOL_GPL(vz_security_protocol_check);

int nr_threads_ve(struct ve_struct *ve)
{
	return cgroup_task_count(ve->ve_cgroup);
}
