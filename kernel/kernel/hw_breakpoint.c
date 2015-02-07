


#include <linux/irqflags.h>
#include <linux/kallsyms.h>
#include <linux/notifier.h>
#include <linux/kprobes.h>
#include <linux/kdebug.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/smp.h>

#include <linux/hw_breakpoint.h>



/* Number of pinned cpu breakpoints in a cpu */
static DEFINE_PER_CPU(unsigned int, nr_cpu_bp_pinned[TYPE_MAX]);

/* Number of pinned task breakpoints in a cpu */
static DEFINE_PER_CPU(unsigned int *, nr_task_bp_pinned[TYPE_MAX]);

/* Number of non-pinned cpu/task breakpoints in a cpu */
static DEFINE_PER_CPU(unsigned int, nr_bp_flexible[TYPE_MAX]);

static int nr_slots[TYPE_MAX];

static int constraints_initialized;

/* Gather the number of total pinned and un-pinned bp in a cpuset */
struct bp_busy_slots {
	unsigned int pinned;
	unsigned int flexible;
};

/* Serialize accesses to the above constraints */
static DEFINE_MUTEX(nr_bp_mutex);

__weak int hw_breakpoint_weight(struct perf_event *bp)
{
	return 1;
}

static inline enum bp_type_idx find_slot_idx(struct perf_event *bp)
{
	if (bp->attr.bp_type & HW_BREAKPOINT_RW)
		return TYPE_DATA;

	return TYPE_INST;
}

static unsigned int max_task_bp_pinned(int cpu, enum bp_type_idx type)
{
	int i;
	unsigned int *tsk_pinned = per_cpu(nr_task_bp_pinned[type], cpu);

	for (i = nr_slots[type] - 1; i >= 0; i--) {
		if (tsk_pinned[i] > 0)
			return i + 1;
	}

	return 0;
}

static int task_bp_pinned(struct task_struct *tsk, enum bp_type_idx type)
{
	struct perf_event_context *ctx = tsk->perf_event_ctxp;
	struct list_head *list;
	struct perf_event *bp;
	unsigned long flags;
	int count = 0;

	if (WARN_ONCE(!ctx, "No perf context for this task"))
		return 0;

	list = &ctx->event_list;

	raw_spin_lock_irqsave(&ctx->lock, flags);

	/*
	 * The current breakpoint counter is not included in the list
	 * at the open() callback time
	 */
	list_for_each_entry(bp, list, event_entry) {
		if (bp->attr.type == PERF_TYPE_BREAKPOINT)
			if (find_slot_idx(bp) == type)
				count += hw_breakpoint_weight(bp);
	}

	raw_spin_unlock_irqrestore(&ctx->lock, flags);

	return count;
}

static void
fetch_bp_busy_slots(struct bp_busy_slots *slots, struct perf_event *bp,
		    enum bp_type_idx type)
{
	int cpu = bp->cpu;
	struct task_struct *tsk = bp->ctx->task;

	if (cpu >= 0) {
		slots->pinned = per_cpu(nr_cpu_bp_pinned[type], cpu);
		if (!tsk)
			slots->pinned += max_task_bp_pinned(cpu, type);
		else
			slots->pinned += task_bp_pinned(tsk, type);
		slots->flexible = per_cpu(nr_bp_flexible[type], cpu);

		return;
	}

	for_each_online_cpu(cpu) {
		unsigned int nr;

		nr = per_cpu(nr_cpu_bp_pinned[type], cpu);
		if (!tsk)
			nr += max_task_bp_pinned(cpu, type);
		else
			nr += task_bp_pinned(tsk, type);

		if (nr > slots->pinned)
			slots->pinned = nr;

		nr = per_cpu(nr_bp_flexible[type], cpu);

		if (nr > slots->flexible)
			slots->flexible = nr;
	}
}

static void
fetch_this_slot(struct bp_busy_slots *slots, int weight)
{
	slots->pinned += weight;
}

static void toggle_bp_task_slot(struct task_struct *tsk, int cpu, bool enable,
				enum bp_type_idx type, int weight)
{
	unsigned int *tsk_pinned;
	int old_count = 0;
	int old_idx = 0;
	int idx = 0;

	old_count = task_bp_pinned(tsk, type);
	old_idx = old_count - 1;
	idx = old_idx + weight;

	tsk_pinned = per_cpu(nr_task_bp_pinned[type], cpu);
	if (enable) {
		tsk_pinned[idx]++;
		if (old_count > 0)
			tsk_pinned[old_idx]--;
	} else {
		tsk_pinned[idx]--;
		if (old_count > 0)
			tsk_pinned[old_idx]++;
	}
}

static void
toggle_bp_slot(struct perf_event *bp, bool enable, enum bp_type_idx type,
	       int weight)
{
	int cpu = bp->cpu;
	struct task_struct *tsk = bp->ctx->task;

	/* Pinned counter task profiling */
	if (tsk) {
		if (cpu >= 0) {
			toggle_bp_task_slot(tsk, cpu, enable, type, weight);
			return;
		}

		for_each_online_cpu(cpu)
			toggle_bp_task_slot(tsk, cpu, enable, type, weight);
		return;
	}

	/* Pinned counter cpu profiling */
	if (enable)
		per_cpu(nr_cpu_bp_pinned[type], bp->cpu) += weight;
	else
		per_cpu(nr_cpu_bp_pinned[type], bp->cpu) -= weight;
}

static int __reserve_bp_slot(struct perf_event *bp)
{
	struct bp_busy_slots slots = {0};
	enum bp_type_idx type;
	int weight;

	/* We couldn't initialize breakpoint constraints on boot */
	if (!constraints_initialized)
		return -ENOMEM;

	/* Basic checks */
	if (bp->attr.bp_type == HW_BREAKPOINT_EMPTY ||
	    bp->attr.bp_type == HW_BREAKPOINT_INVALID)
		return -EINVAL;

	type = find_slot_idx(bp);
	weight = hw_breakpoint_weight(bp);

	fetch_bp_busy_slots(&slots, bp, type);
	fetch_this_slot(&slots, weight);

	/* Flexible counters need to keep at least one slot */
	if (slots.pinned + (!!slots.flexible) > nr_slots[type])
		return -ENOSPC;

	toggle_bp_slot(bp, true, type, weight);

	return 0;
}

int reserve_bp_slot(struct perf_event *bp)
{
	int ret;

	mutex_lock(&nr_bp_mutex);

	ret = __reserve_bp_slot(bp);

	mutex_unlock(&nr_bp_mutex);

	return ret;
}

static void __release_bp_slot(struct perf_event *bp)
{
	enum bp_type_idx type;
	int weight;

	type = find_slot_idx(bp);
	weight = hw_breakpoint_weight(bp);
	toggle_bp_slot(bp, false, type, weight);
}

void release_bp_slot(struct perf_event *bp)
{
	mutex_lock(&nr_bp_mutex);

	__release_bp_slot(bp);

	mutex_unlock(&nr_bp_mutex);
}

int dbg_reserve_bp_slot(struct perf_event *bp)
{
	if (mutex_is_locked(&nr_bp_mutex))
		return -1;

	return __reserve_bp_slot(bp);
}

int dbg_release_bp_slot(struct perf_event *bp)
{
	if (mutex_is_locked(&nr_bp_mutex))
		return -1;

	__release_bp_slot(bp);

	return 0;
}

static int validate_hw_breakpoint(struct perf_event *bp)
{
	int ret;

	ret = arch_validate_hwbkpt_settings(bp);
	if (ret)
		return ret;

	if (arch_check_bp_in_kernelspace(bp)) {
		if (bp->attr.exclude_kernel)
			return -EINVAL;
		/*
		 * Don't let unprivileged users set a breakpoint in the trap
		 * path to avoid trap recursion attacks.
		 */
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
	}

	return 0;
}

int register_perf_hw_breakpoint(struct perf_event *bp)
{
	int ret;

	ret = reserve_bp_slot(bp);
	if (ret)
		return ret;

	ret = validate_hw_breakpoint(bp);

	/* if arch_validate_hwbkpt_settings() fails then release bp slot */
	if (ret)
		release_bp_slot(bp);

	return ret;
}

struct perf_event *
register_user_hw_breakpoint(struct perf_event_attr *attr,
			    perf_overflow_handler_t triggered,
			    struct task_struct *tsk)
{
	return perf_event_create_kernel_counter(attr, -1, task_pid_vnr(tsk),
						triggered);
}
EXPORT_SYMBOL_GPL(register_user_hw_breakpoint);

int modify_user_hw_breakpoint(struct perf_event *bp, struct perf_event_attr *attr)
{
	u64 old_addr = bp->attr.bp_addr;
	u64 old_len = bp->attr.bp_len;
	int old_type = bp->attr.bp_type;
	int err = 0;

	perf_event_disable(bp);

	bp->attr.bp_addr = attr->bp_addr;
	bp->attr.bp_type = attr->bp_type;
	bp->attr.bp_len = attr->bp_len;

	if (attr->disabled)
		goto end;

	err = validate_hw_breakpoint(bp);
	if (!err)
		perf_event_enable(bp);

	if (err) {
		bp->attr.bp_addr = old_addr;
		bp->attr.bp_type = old_type;
		bp->attr.bp_len = old_len;
		if (!bp->attr.disabled)
			perf_event_enable(bp);

		return err;
	}

end:
	bp->attr.disabled = attr->disabled;

	return 0;
}
EXPORT_SYMBOL_GPL(modify_user_hw_breakpoint);

void unregister_hw_breakpoint(struct perf_event *bp)
{
	if (!bp)
		return;
	perf_event_release_kernel(bp);
}
EXPORT_SYMBOL_GPL(unregister_hw_breakpoint);

struct perf_event * __percpu *
register_wide_hw_breakpoint(struct perf_event_attr *attr,
			    perf_overflow_handler_t triggered)
{
	struct perf_event * __percpu *cpu_events, **pevent, *bp;
	long err;
	int cpu;

	cpu_events = alloc_percpu(typeof(*cpu_events));
	if (!cpu_events)
		return (void __percpu __force *)ERR_PTR(-ENOMEM);

	get_online_cpus();
	for_each_online_cpu(cpu) {
		pevent = per_cpu_ptr(cpu_events, cpu);
		bp = perf_event_create_kernel_counter(attr, cpu, -1, triggered);

		*pevent = bp;

		if (IS_ERR(bp)) {
			err = PTR_ERR(bp);
			goto fail;
		}
	}
	put_online_cpus();

	return cpu_events;

fail:
	for_each_online_cpu(cpu) {
		pevent = per_cpu_ptr(cpu_events, cpu);
		if (IS_ERR(*pevent))
			break;
		unregister_hw_breakpoint(*pevent);
	}
	put_online_cpus();

	free_percpu(cpu_events);
	return (void __percpu __force *)ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(register_wide_hw_breakpoint);

void unregister_wide_hw_breakpoint(struct perf_event * __percpu *cpu_events)
{
	int cpu;
	struct perf_event **pevent;

	for_each_possible_cpu(cpu) {
		pevent = per_cpu_ptr(cpu_events, cpu);
		unregister_hw_breakpoint(*pevent);
	}
	free_percpu(cpu_events);
}
EXPORT_SYMBOL_GPL(unregister_wide_hw_breakpoint);

static struct notifier_block hw_breakpoint_exceptions_nb = {
	.notifier_call = hw_breakpoint_exceptions_notify,
	/* we need to be notified first */
	.priority = 0x7fffffff
};

static int __init init_hw_breakpoint(void)
{
	unsigned int **task_bp_pinned;
	int cpu, err_cpu;
	int i;

	for (i = 0; i < TYPE_MAX; i++)
		nr_slots[i] = hw_breakpoint_slots(i);

	for_each_possible_cpu(cpu) {
		for (i = 0; i < TYPE_MAX; i++) {
			task_bp_pinned = &per_cpu(nr_task_bp_pinned[i], cpu);
			*task_bp_pinned = kzalloc(sizeof(int) * nr_slots[i],
						  GFP_KERNEL);
			if (!*task_bp_pinned)
				goto err_alloc;
		}
	}

	constraints_initialized = 1;

	return register_die_notifier(&hw_breakpoint_exceptions_nb);

 err_alloc:
	for_each_possible_cpu(err_cpu) {
		if (err_cpu == cpu)
			break;
		for (i = 0; i < TYPE_MAX; i++)
			kfree(per_cpu(nr_task_bp_pinned[i], cpu));
	}

	return -ENOMEM;
}
core_initcall(init_hw_breakpoint);


struct pmu perf_ops_bp = {
	.enable		= arch_install_hw_breakpoint,
	.disable	= arch_uninstall_hw_breakpoint,
	.read		= hw_breakpoint_pmu_read,
};
