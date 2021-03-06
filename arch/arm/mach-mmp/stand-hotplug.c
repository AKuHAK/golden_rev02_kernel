/* linux/arch/arm/mach-exynos/stand-hotplug.c
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * EXYNOS - Dynamic CPU hotpluging
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/serial_core.h>
#include <linux/platform_device.h>
#include <linux/cpu.h>
#include <linux/tick.h>
#include <linux/suspend.h>
#include <linux/reboot.h>
#include <linux/cpufreq.h>
#include <linux/clk.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>

#include <plat/clock.h>
#include <plat/pxa_trace.h>

#include <mach/clock-pxa988.h>

#define BOOT_DELAY	60
#define CHECK_DELAY_NOP	(.5 * HZ * 16)
#define CHECK_DELAY_ON	(.5 * HZ * 4)
#define CHECK_DELAY_OFF	(.5*HZ)
#define TRANS_RQ 1
#define TRANS_LOAD_RQ 8
#define NUM_CPUS num_possible_cpus()
#define CPULOAD_TABLE (NR_CPUS + 1)

#if (NR_CPUS > 2)
#define MIPS_HIGH	624000
#define MIPS_LOW	250000
#else
#define TRANS_LOAD_L0 0
#define TRANS_LOAD_H0 25
#define TRANS_LOAD_L1 10
#define TRANS_LOAD_H1 100
#endif

#define cputime64_sub(__a, __b)	((__a) - (__b))

static struct workqueue_struct *hotplug_wq;
static struct delayed_work hotplug_work;

static unsigned int freq_max;
static unsigned int freq_min = -1UL;
static unsigned int max_performance;

static unsigned int hotpluging_rate = CHECK_DELAY_OFF;
static unsigned int hotpluging_nop_time = CHECK_DELAY_NOP;
static unsigned int nop_time;
static unsigned int user_lock;
static unsigned int trans_rq = TRANS_RQ;
static unsigned int trans_load_rq = TRANS_LOAD_RQ;

#if (NR_CPUS <= 2)
static unsigned int trans_load_l0 = TRANS_LOAD_L0;
static unsigned int trans_load_h0 = TRANS_LOAD_H0;
static unsigned int trans_load_l1 = TRANS_LOAD_L1;
static unsigned int trans_load_h1 = TRANS_LOAD_H1;
#else
static unsigned int trans_load_l0;
static unsigned int trans_load_l1;
static unsigned int trans_load_l2;
static unsigned int trans_load_l3;
static unsigned int trans_load_h0;
static unsigned int trans_load_h1;
static unsigned int trans_load_h2;
static unsigned int trans_load_h3 = 100;
#endif



enum flag {
	HOTPLUG_NOP,
	HOTPLUG_IN,
	HOTPLUG_OUT
};

enum {
	THRESHOLD_LOW = 0,
	THRESHOLD_HIGH,
};

struct cpu_time_info {
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_wall;
	cputime64_t total_wall_time;	/* True wall time*/
	unsigned int load;		/* Not consider freq */
	unsigned int avg_load;		/* Consider freq */
};

struct cpu_hotplug_info {
	unsigned long nr_running;
	pid_t tgid;
};


static DEFINE_PER_CPU(struct cpu_time_info, hotplug_cpu_time);

static void init_hotplug_statistics(void)
{
	int i;

	/* Initialize tmp_info */
	for_each_online_cpu(i) {
		struct cpu_time_info *tmp_info;
		cputime64_t cur_wall_time, cur_idle_time;
		tmp_info = &per_cpu(hotplug_cpu_time, i);

		/* Get current idle and wall time */
		cur_idle_time = get_cpu_idle_time(i, &cur_wall_time);

		/* Initial tmp_info */
		tmp_info->load  = 0;
		tmp_info->avg_load  = 0;
		tmp_info->prev_cpu_idle  = cur_idle_time;
		tmp_info->prev_cpu_wall  = cur_wall_time;
		tmp_info->total_wall_time = cur_wall_time;
	}

	/* Initialize nop_time */
	nop_time = 0;
}

/* mutex can be used since hotplug_timer does not run in
   timer(softirq) context but in process context */
static DEFINE_MUTEX(hotplug_stat_lock);
static DEFINE_MUTEX(hotplug_user_lock);
static struct clk *cpu_clk;
static inline enum flag
standalone_hotplug(unsigned int avg_load, unsigned int cur_freq,
		unsigned long nr_rq_min, unsigned int cpu_rq_min)
{
	unsigned int nr_online_cpu;

	/*load threshold*/
	unsigned int threshold[CPULOAD_TABLE][2] = {
		{trans_load_l0, trans_load_h0},
		{trans_load_l1, trans_load_h1},
#if (NR_CPUS > 2)
		{trans_load_l2, trans_load_h2},
		{trans_load_l3, trans_load_h3},
#endif
		{0, 0}
	};

	nr_online_cpu = num_online_cpus();

	if (avg_load < threshold[nr_online_cpu - 1][THRESHOLD_LOW]) {
		nop_time = 0;
		if (nr_online_cpu > 1)
			return HOTPLUG_OUT;
		/* If total nr_running is less than cpu(on-state) number
		 * hotplug do not hotplug-in
		 */
	} else if (avg_load > threshold[nr_online_cpu - 1][THRESHOLD_HIGH]) {
		nop_time = 0;
		if ((nr_running() > nr_online_cpu) &&
		    (cur_freq > freq_min))
			return HOTPLUG_IN;
	} else if (nr_online_cpu > 1 && nr_rq_min < trans_rq) {
		struct cpu_time_info *tmp_info;

		tmp_info = &per_cpu(hotplug_cpu_time, cpu_rq_min);
		/* If CPU(cpu_rq_min) load is less than trans_load_rq
		 * hotplug-out operation need
		 */
		if ((tmp_info->load < trans_load_rq) &&
		    (avg_load < threshold[nr_online_cpu - 2][THRESHOLD_HIGH])) {
			nop_time = 0;
			return HOTPLUG_OUT;
		}
	} else if (nr_online_cpu > 1) {
		if ((avg_load >=
		     threshold[nr_online_cpu - 2][THRESHOLD_HIGH]) &&
		    (avg_load <=
		     threshold[nr_online_cpu - 1][THRESHOLD_HIGH]))
			nop_time = 0;
		else if ((avg_load >=
			  threshold[nr_online_cpu - 1][THRESHOLD_LOW]) &&
			 (avg_load <
			  threshold[nr_online_cpu - 2][THRESHOLD_HIGH])) {
			nop_time += hotpluging_rate;

			/*
			 * If load_l <= avg_load <= former load_h,
			 * more than one online cpu, and nop_time
			 * >= hotpluging_nop_time, plug-out cpu
			 */
			if (nop_time >= hotpluging_nop_time) {
				nop_time = 0;
				return HOTPLUG_OUT;
			}
		}
	}

	return HOTPLUG_NOP;
}

static int hotplug_freq_notifer_call(struct notifier_block *nb,
	       unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = data;
	int i;

	if (val != CPUFREQ_POSTCHANGE)
		return 0;

	/* If lock this hotplug, user_lock=1, return */
	if (user_lock == 1)
		return  0;

	mutex_lock(&hotplug_stat_lock);

	for_each_online_cpu(i) {
		struct cpu_time_info *tmp_info;
		cputime64_t cur_wall_time, cur_idle_time;
		unsigned int idle_time, wall_time;

		tmp_info = &per_cpu(hotplug_cpu_time, i);

		/* Get idle time and wall time */
		cur_idle_time = get_cpu_idle_time(i, &cur_wall_time);

		/* Update idle time */
		idle_time = (unsigned int)cputime64_sub(cur_idle_time, \
					tmp_info->prev_cpu_idle);
		tmp_info->prev_cpu_idle = cur_idle_time;

		/* Update wall time */
		wall_time = (unsigned int)cputime64_sub(cur_wall_time, \
					tmp_info->prev_cpu_wall);
		tmp_info->prev_cpu_wall = cur_wall_time;

		/*
		 * Let idle & wall time to be divided by 1024
		 * to avoid overflow
		 */
		idle_time >>= 10;
		wall_time >>= 10;

		/* Update load */
		tmp_info->load += (wall_time - idle_time);

		/*
		 * Update avg_load, and let freq to be divided
		 * by 1024 to avoid overflow
		 */
		tmp_info->avg_load += ((wall_time - idle_time) \
					* (freq->old >> 10));
	}

	mutex_unlock(&hotplug_stat_lock);

	return 0;
}

static struct notifier_block hotplug_freq_notifier = {
	.notifier_call = hotplug_freq_notifer_call
};

static void __ref hotplug_timer(struct work_struct *work)
{
	struct cpu_hotplug_info tmp_hotplug_info[4];
	struct sched_param param_normal = { .sched_priority = 0 };
	struct sched_param param_rt = { .sched_priority = 1};
	int i;
	unsigned int avg_load = 0;
	unsigned int cpu_rq_min = 0;
	unsigned long nr_rq_min = -1UL;
	unsigned int select_off_cpu = 0;
	unsigned int cur_freq;
	enum flag flag_hotplug;

	/* If lock this hotplug, user_lock=1, return */
	if (user_lock == 1)
		return;

	mutex_lock(&hotplug_stat_lock);

	/* Get current cpu freq */
	cur_freq = cpu_clk->ops->getrate(cpu_clk) / 1000;

	for_each_online_cpu(i) {
		struct cpu_time_info *tmp_info;
		cputime64_t cur_wall_time, cur_idle_time;
		unsigned int idle_time, wall_time, total_wall_time;

		tmp_info = &per_cpu(hotplug_cpu_time, i);

		/* Get idle time and wall time */
		cur_idle_time = get_cpu_idle_time(i, &cur_wall_time);

		/* Get idle_time and wall_time */
		idle_time = (unsigned int)cputime64_sub(cur_idle_time, \
					tmp_info->prev_cpu_idle);
		wall_time = (unsigned int)cputime64_sub(cur_wall_time, \
					tmp_info->prev_cpu_wall);

		/* Check wall time and idle time */
		if (wall_time < idle_time) {
			mutex_unlock(&hotplug_stat_lock);
			goto no_hotplug;
		}

		/* Update idle time and wall time */
		tmp_info->prev_cpu_idle = cur_idle_time;
		tmp_info->prev_cpu_wall = cur_wall_time;

		/* Update total wall time */
		total_wall_time = (unsigned int)cputime64_sub(cur_wall_time, \
					tmp_info->total_wall_time);
		tmp_info->total_wall_time = cur_wall_time;

		/*
		 * Let idle time, wall time and total wall time
		 * to be divided by 1024 to avoid overflow
		 */
		idle_time >>= 10;
		wall_time >>= 10;
		total_wall_time >>= 10;

		/*For once Divide-by-Zero issue*/
		if (total_wall_time == 0)
			total_wall_time++;

		/* Update load */
		tmp_info->load += (wall_time - idle_time);
		/* Get real load(xx%) */
		tmp_info->load = 100 * tmp_info->load / total_wall_time;

		/*
		 * Update avg_load on current freq, let freq to
		 * be divied by 1024 to avoid overflow
		 */
		tmp_info->avg_load += ((wall_time - idle_time) \
					* (cur_freq >> 10));

		/* Get real avg_load(xx%) */
		tmp_info->avg_load = ((100 * tmp_info->avg_load) \
					/ total_wall_time) \
				     / (max_performance >> 10);

		trace_pxa_hp_single(i,
				    get_cpu_nr_running(i),
				    tmp_info->load,
				    (tmp_info->avg_load *
				     (max_performance >> 10) / 100));

		/* Get avg_load of two cores */
		avg_load += tmp_info->avg_load;

		/* Find minimum runqueue length */
		tmp_hotplug_info[i].nr_running = get_cpu_nr_running(i);

		if (i && nr_rq_min > tmp_hotplug_info[i].nr_running) {
			nr_rq_min = tmp_hotplug_info[i].nr_running;

			cpu_rq_min = i;
		}
	}

	trace_pxa_hotplug_stand_mips((cur_freq / 1000),
				     (avg_load * (max_performance >> 10) / 100),
				     num_online_cpus());

	for (i = NUM_CPUS - 1; i > 0; --i) {
		if (cpu_online(i) == 0) {
			select_off_cpu = i;
			break;
		}
	}

	/*standallone hotplug*/
	flag_hotplug = standalone_hotplug(avg_load, cur_freq,
					nr_rq_min, cpu_rq_min);

	/* Initial tmp_info */
	for_each_online_cpu(i) {
		struct cpu_time_info *tmp_info;
		tmp_info = &per_cpu(hotplug_cpu_time, i);

		/* Initial load, avg_load, total wall time */
		tmp_info->load = 0;
		tmp_info->avg_load = 0;
	}

	/*cpu hotplug*/
	if ((flag_hotplug == HOTPLUG_IN) &&
	    /* Avoid running cpu_up(0) */
	    select_off_cpu &&
	    (!cpu_online(select_off_cpu))) {
		pr_info("cpu%d turning on!\n", select_off_cpu);
		mutex_unlock(&hotplug_stat_lock);

		/* Plug-In one cpu */
		sched_setscheduler(kthreadd_task, SCHED_FIFO, &param_rt);
		cpu_up(select_off_cpu);
		sched_setscheduler(kthreadd_task, SCHED_NORMAL, &param_normal);
		pr_info("cpu%d on\n", select_off_cpu);

		hotpluging_rate = CHECK_DELAY_ON;
	} else if ((flag_hotplug == HOTPLUG_OUT) &&
		   /* Avoid running cpu_down(0) */
		   cpu_rq_min &&
		   (cpu_online(cpu_rq_min))) {
		pr_info("cpu%d turnning off!\n", cpu_rq_min);
		mutex_unlock(&hotplug_stat_lock);

		/* Plug-Out one cpu */
		cpu_down(cpu_rq_min);
		pr_info("cpu%d off!\n", cpu_rq_min);

		hotpluging_rate = CHECK_DELAY_OFF;
	} else
		mutex_unlock(&hotplug_stat_lock);

no_hotplug:
	queue_delayed_work_on(0, hotplug_wq, \
				&hotplug_work, hotpluging_rate);
}

static int standalone_hotplug_notifier_event(struct notifier_block *this,
					     unsigned long event, void *ptr)
{
	static unsigned user_lock_saved;

	switch (event) {
	case PM_SUSPEND_PREPARE:
		mutex_lock(&hotplug_user_lock);
		user_lock_saved = user_lock;
		user_lock = 1;
		pr_info("%s: saving pm_hotplug lock %x\n",
			__func__, user_lock_saved);
		mutex_unlock(&hotplug_user_lock);
		return NOTIFY_OK;
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		mutex_lock(&hotplug_user_lock);
		pr_info("%s: restoring pm_hotplug lock %x\n",
			__func__, user_lock_saved);
		user_lock = user_lock_saved;
		mutex_unlock(&hotplug_user_lock);
		/*
		 * corner case:
		 * user_lock set to 1 during suspend, and work_queue may goto
		 * "unlock", then work_queue never have chance to run again
		 */
		if (0 == user_lock) {
			flush_delayed_work(&hotplug_work);
			mutex_lock(&hotplug_stat_lock);
			/* Initialize data */
			init_hotplug_statistics();
			mutex_unlock(&hotplug_stat_lock);
			queue_delayed_work_on(0, hotplug_wq, &hotplug_work,
				hotpluging_rate);
		}

		return NOTIFY_OK;
	}
	return NOTIFY_DONE;
}

/*TODO: Whether the PM notifier is enabled in our suspend process*/
static struct notifier_block standalone_hotplug_notifier = {
	.notifier_call = standalone_hotplug_notifier_event,
};

static int hotplug_reboot_notifier_call(struct notifier_block *this,
					unsigned long code, void *_cmd)
{
	mutex_lock(&hotplug_user_lock);
	pr_err("%s: disabling pm hotplug\n", __func__);
	user_lock = 1;
	mutex_unlock(&hotplug_user_lock);

	return NOTIFY_DONE;
}

static struct notifier_block hotplug_reboot_notifier = {
	.notifier_call = hotplug_reboot_notifier_call,
};

static struct kobject hotplug_kobj;

static int bound_freq_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%d\n", freq_min);
}
static int bound_freq_set(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	int freq_tmp;
	if (!sscanf(buf, "%d", &freq_tmp))
		return -EINVAL;
	freq_min = freq_tmp;
	return count;
}
static DEVICE_ATTR(bound_freq, S_IRUGO | S_IWUSR, bound_freq_get,
		bound_freq_set);


static int lock_get(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return sprintf(buf, "%d\n", user_lock);
}

static int lock_set(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	u32 val;
	int restart_hp = 0;
	bool waitqueue_finish = false;

	if (!sscanf(buf, "%d", &val))
		return -EINVAL;

	mutex_lock(&hotplug_user_lock);
	/* we want to re-enable governor */
	if ((1 == user_lock) && (0 == val))
		restart_hp = 1;
	/*
	 * when lock governor, we should wait workqueue finish
	 * before return, it can avoid user lock and gonvernor work
	 * concurrent conflict corner case
	 */
	if ((0 == user_lock) && (1 == !!val))
		waitqueue_finish = true;
	user_lock = val ? 1 : 0;
	mutex_unlock(&hotplug_user_lock);

	if (restart_hp) {
		flush_delayed_work(&hotplug_work);
		mutex_lock(&hotplug_stat_lock);
		/* Initialize data */
		init_hotplug_statistics();
		mutex_unlock(&hotplug_stat_lock);
		queue_delayed_work_on(0, hotplug_wq, &hotplug_work,
				CHECK_DELAY_OFF);
	}
	if (waitqueue_finish)
		cancel_delayed_work_sync(&hotplug_work);

	return count;
}
static DEVICE_ATTR(lock, S_IRUGO | S_IWUSR, lock_get, lock_set);

#define define_store_thhd_function(_name) \
static ssize_t load_set_##_name(struct device *dev, \
			struct device_attribute *attr, \
			const char *buf, size_t count) \
{ \
	int tmp; \
	if (!sscanf(buf, "%d", &tmp)) \
		return -EINVAL; \
	trans_load_##_name = tmp; \
	return count; \
}
#define define_show_thhd_function(_name) \
static ssize_t load_get_##_name(struct device *dev, \
				struct device_attribute *attr, \
				char *buf) \
{ \
	return sprintf(buf, "%d\n", (int) trans_load_##_name); \
}

define_store_thhd_function(h0);
define_show_thhd_function(h0);
define_store_thhd_function(l1);
define_show_thhd_function(l1);
static DEVICE_ATTR(load_h0, S_IRUGO | S_IWUSR, load_get_h0, load_set_h0);
static DEVICE_ATTR(load_l1, S_IRUGO | S_IWUSR, load_get_l1, load_set_l1);

#if (NR_CPUS > 2)
define_store_thhd_function(h1);
define_show_thhd_function(h1);
define_store_thhd_function(l2);
define_show_thhd_function(l2);
define_store_thhd_function(h2);
define_show_thhd_function(h2);
define_store_thhd_function(l3);
define_show_thhd_function(l3);
static DEVICE_ATTR(load_h1, S_IRUGO | S_IWUSR, load_get_h1, load_set_h1);
static DEVICE_ATTR(load_l2, S_IRUGO | S_IWUSR, load_get_l2, load_set_l2);
static DEVICE_ATTR(load_h2, S_IRUGO | S_IWUSR, load_get_h2, load_set_h2);
static DEVICE_ATTR(load_l3, S_IRUGO | S_IWUSR, load_get_l3, load_set_l3);
#endif

static struct attribute *hotplug_attributes[] = {
	&dev_attr_lock.attr,
	&dev_attr_load_h0.attr,
	&dev_attr_load_l1.attr,
#if (NR_CPUS > 2)
	&dev_attr_load_h1.attr,
	&dev_attr_load_l2.attr,
	&dev_attr_load_h2.attr,
	&dev_attr_load_l3.attr,
#endif
	&dev_attr_bound_freq.attr,
	NULL,
};

static struct kobj_type hotplug_dir_ktype = {
	.sysfs_ops	= &kobj_sysfs_ops,
	.default_attrs	= hotplug_attributes,
};

static int __init stand_alone_hotplug_init(void)
{
	unsigned int freq;
	int i, ret;
	struct cpufreq_frequency_table *table;

	pr_info("%s, PM-hotplug init function\n", __func__);
	hotplug_wq = create_singlethread_workqueue("dynamic hotplug");
	if (!hotplug_wq) {
		printk(KERN_ERR "Creation of hotplug work failed\n");
		ret = -EFAULT;
		goto err_create_singlethread_workqueue;
	}

	if (!cpu_clk) {
		cpu_clk = clk_get_sys(NULL, "cpu");
		if (IS_ERR(cpu_clk)) {
			ret = PTR_ERR(cpu_clk);
			goto err_clk_get_sys;
		}
	}

	/* Register cpufreq change notifier call */
	ret = cpufreq_register_notifier(&hotplug_freq_notifier,
				CPUFREQ_TRANSITION_NOTIFIER);
	if (ret)
		goto err_cpufreq_register_notifier;

	/* Initialize data */
	init_hotplug_statistics();

	INIT_DELAYED_WORK(&hotplug_work, hotplug_timer);

	queue_delayed_work_on(0, hotplug_wq, &hotplug_work, BOOT_DELAY * HZ);
	table = cpufreq_frequency_get_table(0);

	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) {
		freq = table[i].frequency;
		if (freq != CPUFREQ_ENTRY_INVALID && freq > freq_max)
			freq_max = freq;
		if (freq != CPUFREQ_ENTRY_INVALID && freq_min > freq)
			freq_min = freq;
	}

	max_performance = freq_max * NUM_CPUS;
	pr_info("init max_performance : %u\n", max_performance);

#if (NR_CPUS > 2)
	/* Set trans_load_XX with MIPS_XX */
	trans_load_l1 = MIPS_LOW * 100 * 1 / max_performance;
	trans_load_l2 = MIPS_LOW * 100 * 2 / max_performance;
	trans_load_l3 = MIPS_LOW * 100 * 3 / max_performance;
	trans_load_h0 = MIPS_HIGH * 100 * 1 / max_performance;
	trans_load_h1 = MIPS_HIGH * 100 * 2 / max_performance;
	trans_load_h2 = MIPS_HIGH * 100 * 3 / max_performance;

	/* Show trans_load_XX */

	pr_info("--------------------\n"
		"|CPU|LOW(%%)|HIGH(%%)|\n"
		"|  1|   %3d|    %3d|\n"
		"|  2|   %3d|    %3d|\n"
		"|  3|   %3d|    %3d|\n"
		"|  4|   %3d|    %3d|\n"
		"--------------------\n",
		trans_load_l0, trans_load_h0,
		trans_load_l1, trans_load_h1,
		trans_load_l2, trans_load_h2,
		trans_load_l3, trans_load_h3);

#endif

	ret = kobject_init_and_add(&hotplug_kobj, &hotplug_dir_ktype,
				&(cpu_subsys.dev_root->kobj), "hotplug");
	if (ret) {
		pr_err("%s: Failed to add kobject for hotplug\n", __func__);
		goto err_kobject_init_and_add;
	}

	ret = register_pm_notifier(&standalone_hotplug_notifier);
	if (ret)
		goto err_register_pm_notifier;

	ret = register_reboot_notifier(&hotplug_reboot_notifier);
	if (ret)
		goto err_register_reboot_notifier;

	return 0;

err_register_reboot_notifier:
	unregister_pm_notifier(&standalone_hotplug_notifier);
err_register_pm_notifier:
err_kobject_init_and_add:
	cancel_delayed_work(&hotplug_work);
	cpufreq_unregister_notifier(&hotplug_freq_notifier,
				CPUFREQ_POLICY_NOTIFIER);
err_cpufreq_register_notifier:
	clk_put(cpu_clk);
err_clk_get_sys:
	destroy_workqueue(hotplug_wq);
err_create_singlethread_workqueue:
	return ret;
}

module_init(stand_alone_hotplug_init);

static struct platform_device standalone_hotplug_device = {
	.name = "standalone-cpu-hotplug",
	.id = -1,
};

static int __init standalone_hotplug_device_init(void)
{
	int ret;

	ret = platform_device_register(&standalone_hotplug_device);

	if (ret) {
		pr_err("Register device Failed\n");
		return ret;
	}

	printk(KERN_INFO "standalone_hotplug_device_init: %d\n", ret);

	return ret;
}

module_init(standalone_hotplug_device_init);
