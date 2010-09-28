/*
 * Copyright (C) 2010 Texas Instruments, Inc.
 *   Written by Mike Turquette <mturquette@ti.com>
 *   based on ondemand governor
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/err.h>

/* greater than 80% avg load across a processor group increases frequency */
#define DEF_UP_FREQ_MIN_LOAD			(80)

/* less than 20% avg load across a processor group decreases frequency */
#define DEF_DOWN_FREQ_MAX_LOAD			(20)

/* plug-in auxillary cpus anytime freq increases */
#define DEF_PLUG_IN_MIN_FREQ			(0)

/* plug-out auxillary cpus when at lowest frequency in the cpufreq table */
#define DEF_PLUG_OUT_MAX_FREQ			(0)

/* default sampling period (uSec) is bogus; 10x ondemand's default for x86 */
#define DEF_SAMPLING_PERIOD			(100000)

static unsigned int min_sampling_rate;
static void do_dbs_timer(struct work_struct *work);
static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
				unsigned int event);

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_HOTPLUG
static
#endif
struct cpufreq_governor cpufreq_gov_hotplug = {
       .name                   = "hotplug",
       .governor               = cpufreq_governor_dbs,
       .owner                  = THIS_MODULE,
};

struct cpu_dbs_info_s {
	cputime64_t prev_cpu_idle;
#if 0
	cputime64_t prev_cpu_iowait;
#endif
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_nice;
	struct cpufreq_policy *cur_policy;
	struct delayed_work work;
	struct cpufreq_frequency_table *freq_table;
	int cpu;
	/*
	 * percpu mutex that serializes governor limit change with
	 * do_dbs_timer invocation. We do not want do_dbs_timer to run
	 * when user is changing the governor or limits.
	 */
	struct mutex timer_mutex;
};
static DEFINE_PER_CPU(struct cpu_dbs_info_s, hp_cpu_dbs_info);

static unsigned int dbs_enable;	/* number of CPUs using this policy */

/*
 * dbs_mutex protects data in dbs_tuners_ins from concurrent changes on
 * different CPUs. It protects dbs_enable in governor start/stop.
 */
static DEFINE_MUTEX(dbs_mutex);

static struct workqueue_struct	*khotplug_wq;

static struct dbs_tuners {
	unsigned int sampling_rate;
	unsigned int up_threshold;
	unsigned int down_threshold;
	unsigned int plug_out_freq;
	unsigned int plug_in_freq;
	unsigned int plug_out_min_sampling_periods;
	unsigned int plug_in_min_sampling_periods;
	unsigned int num_plug_out_freq_periods;
	unsigned int num_plug_in_freq_periods;
	unsigned int ignore_nice;
	unsigned int io_is_busy;
} dbs_tuners_ins = {
	.sampling_rate = DEF_SAMPLING_PERIOD,
	.up_threshold = DEF_UP_FREQ_MIN_LOAD,
	.down_threshold = DEF_DOWN_FREQ_MAX_LOAD,
	.plug_out_freq = DEF_PLUG_OUT_MAX_FREQ,
	.plug_in_freq = DEF_PLUG_IN_MIN_FREQ,
	.plug_out_min_sampling_periods = 0,
	.plug_in_min_sampling_periods = 0,
	.num_plug_out_freq_periods = 0,
	.num_plug_in_freq_periods = 0,
	.ignore_nice = 0,
	.io_is_busy = 0,
};

/*
 * A corner case exists when switching io_is_busy at run-time: comparing idle
 * times from a non-io_is_busy period to an io_is_busy period (or vice-versa)
 * will misrepresent the actual change in system idleness.  We ignore this
 * corner case: enabling io_is_busy might cause freq increase and disabling
 * might cause freq decrease, which probably matches the original intent.
 */
static inline cputime64_t get_cpu_idle_time(unsigned int cpu, cputime64_t *wall)
{
        u64 idle_time;
        u64 iowait_time;

        /* cpufreq-hotplug always assumes CONFIG_NO_HZ */
        idle_time = get_cpu_idle_time_us(cpu, wall);

	/* add time spent doing I/O to idle time */
        if (dbs_tuners_ins.io_is_busy) {
                iowait_time = get_cpu_iowait_time_us(cpu, wall);
                /* cpufreq-hotplug always assumes CONFIG_NO_HZ */
                if (iowait_time != -1ULL && idle_time >= iowait_time)
                        idle_time -= iowait_time;
        }

        return idle_time;
}

/************************** sysfs interface ************************/

/* XXX look at global sysfs macros in cpufreq.h, can those be used here? */

/* cpufreq_hotplug Governor Tunables */
#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)              \
{									\
	return sprintf(buf, "%u\n", dbs_tuners_ins.object);		\
}
show_one(sampling_rate, sampling_rate);
show_one(up_threshold, up_threshold);
show_one(down_threshold, down_threshold);
show_one(plug_out_freq, plug_out_freq);
show_one(plug_in_freq, plug_in_freq);
show_one(plug_out_min_sampling_periods, plug_out_min_sampling_periods);
show_one(plug_in_min_sampling_periods, plug_in_min_sampling_periods);
show_one(ignore_nice_load, ignore_nice);
show_one(io_is_busy, io_is_busy);

static ssize_t store_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.sampling_rate = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_up_threshold(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input <= dbs_tuners_ins.down_threshold) {
		return -EINVAL;
	}

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.up_threshold = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_down_threshold(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input >= dbs_tuners_ins.up_threshold) {
		return -EINVAL;
	}

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.down_threshold = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_plug_out_freq(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || (dbs_tuners_ins.plug_in_freq &&
				input >= dbs_tuners_ins.plug_in_freq))
			return -EINVAL;

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.plug_out_freq = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_plug_in_freq(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || (dbs_tuners_ins.plug_out_freq &&
				input <= dbs_tuners_ins.plug_out_freq))
			return -EINVAL;

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.plug_in_freq = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_plug_out_min_sampling_periods(struct kobject *a, struct
		attribute *b, const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.plug_out_min_sampling_periods = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_plug_in_min_sampling_periods(struct kobject *a, struct
		attribute *b, const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.plug_in_min_sampling_periods = input;
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_ignore_nice_load(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	unsigned int j;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	mutex_lock(&dbs_mutex);
	if (input == dbs_tuners_ins.ignore_nice) { /* nothing to do */
		mutex_unlock(&dbs_mutex);
		return count;
	}
	dbs_tuners_ins.ignore_nice = input;

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct cpu_dbs_info_s *dbs_info;
		dbs_info = &per_cpu(hp_cpu_dbs_info, j);
		dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&dbs_info->prev_cpu_wall);
		if (dbs_tuners_ins.ignore_nice)
			dbs_info->prev_cpu_nice = kstat_cpu(j).cpustat.nice;

	}
	mutex_unlock(&dbs_mutex);

	return count;
}

static ssize_t store_io_is_busy(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	mutex_lock(&dbs_mutex);
	dbs_tuners_ins.io_is_busy = !!input;
	mutex_unlock(&dbs_mutex);

	return count;
}

define_one_global_rw(sampling_rate);
define_one_global_rw(up_threshold);
define_one_global_rw(down_threshold);
define_one_global_rw(plug_out_freq);
define_one_global_rw(plug_in_freq);
define_one_global_rw(plug_out_min_sampling_periods);
define_one_global_rw(plug_in_min_sampling_periods);
define_one_global_rw(ignore_nice_load);
define_one_global_rw(io_is_busy);

static struct attribute *dbs_attributes[] = {
#if 0
	&sampling_rate_min.attr,
#endif
	&sampling_rate.attr,
	&up_threshold.attr,
	&down_threshold.attr,
	&plug_out_freq.attr,
	&plug_in_freq.attr,
	&plug_out_min_sampling_periods.attr,
	&plug_in_min_sampling_periods.attr,
	&ignore_nice_load.attr,
	&io_is_busy.attr,
	NULL
};

static struct attribute_group dbs_attr_group = {
	.attrs = dbs_attributes,
	.name = "hotplug",
};

/************************** sysfs end ************************/

static void dbs_check_cpu(struct cpu_dbs_info_s *this_dbs_info)
{
	/* combined load of all enabled CPUs */
	unsigned int total_load = 0;
	/* single largest CPU load */
	unsigned int max_load = 0;
	/* average load across enabled CPUs */
	unsigned int avg_load = 0;

	unsigned int index = 0;
	unsigned int requested_freq = 0;

	struct cpufreq_policy *policy;
	unsigned int j;

	policy = this_dbs_info->cur_policy;

	/* XXX
	 * this should be run per cpu group
	 * cpu groups are CPUs that are related
	 */

	/* get highest load and total load across the CPUs */
	for_each_cpu(j, policy->cpus) {
		unsigned int load;
		unsigned int idle_time, wall_time;
		cputime64_t cur_wall_time, cur_idle_time;
		struct cpu_dbs_info_s *j_dbs_info;

		j_dbs_info = &per_cpu(hp_cpu_dbs_info, j);

		/* update both cur_idle_time and cur_wall_time */
		cur_idle_time = get_cpu_idle_time(j, &cur_wall_time);

		/* how much wall time has passed since last iteration? */
		wall_time = (unsigned int) cputime64_sub(cur_wall_time,
				j_dbs_info->prev_cpu_wall);
		j_dbs_info->prev_cpu_wall = cur_wall_time;

		/* how much idle time has passed since last iteration? */
		idle_time = (unsigned int) cputime64_sub(cur_idle_time,
				j_dbs_info->prev_cpu_idle);
		j_dbs_info->prev_cpu_idle = cur_idle_time;

		if (unlikely(!wall_time || wall_time < idle_time))
			continue;

		/* load is the percentage of time not spent in idle */
		//pr_err("wall_time is %d\n", wall_time);
		load = 100 * (wall_time - idle_time) / wall_time;

		/* keep track of combined load across all related CPUs */
		total_load += load;

		/* keep track of highest single load across all related CPUs */
		if (load > max_load)
			max_load = load;
	}

	/* XXX should below code use policy->cpus or something avoid race? */
	/* calculate the average load across all related CPUs */
	//pr_err("num_online_cpus() is %d\n", num_online_cpus());
	avg_load = total_load / num_online_cpus();

	pr_err("avg_load is %d, max_load is %d\n",
			avg_load, max_load);

	/* update the number of periods at or above plug_in_freq */
	if (!dbs_tuners_ins.plug_in_freq ||
			policy->cur >= dbs_tuners_ins.plug_in_freq)
		dbs_tuners_ins.num_plug_in_freq_periods++;
	else
		dbs_tuners_ins.num_plug_in_freq_periods = 0;

	/* update the number of periods spent at or below plug_out_freq */
	if (!dbs_tuners_ins.plug_in_freq ||
			policy->cur <= dbs_tuners_ins.plug_out_freq)
		dbs_tuners_ins.num_plug_out_freq_periods++;
	else
		dbs_tuners_ins.num_plug_out_freq_periods = 0;

	/* check for frequency increase */
	if (avg_load > dbs_tuners_ins.up_threshold) {
		/* enable auxillary CPUs if all requirements met */
		if ((num_online_cpus() < 2) &&
		    (dbs_tuners_ins.num_plug_in_freq_periods >=
		     dbs_tuners_ins.plug_in_min_sampling_periods) &&
		    ((total_load / NR_CPUS) > dbs_tuners_ins.down_threshold)) {
			//pr_err("%s: enabling nonboot cpus\n", __func__);
			enable_nonboot_cpus();
			return;
		}

		/* increase to highest frequency supported */
		if (policy->cur < policy->max)
			__cpufreq_driver_target(policy, policy->max,
					CPUFREQ_RELATION_H);

		return;
	}

	/* check for frequency decrease */
	if (avg_load < dbs_tuners_ins.down_threshold) {
		/* disable auxillary CPUs if all requirements met */
		if ((num_online_cpus() > 1) &&
		    (dbs_tuners_ins.num_plug_out_freq_periods >=
		     dbs_tuners_ins.plug_out_min_sampling_periods) &&
		    (total_load < dbs_tuners_ins.up_threshold)) {
			//pr_err("%s: disabling nonboot cpus\n", __func__);
			disable_nonboot_cpus();
			return;
		}

		/* at minimum frequency already, bail out */
		if (policy->cur == policy->min)
			return;

		/*
		 * XXX would be nice to have next_highest, next_lowest accesor
		 * api for table manipulation
		 */

		/* bump down to the next lowest frequency in the table */
		/*requested_freq = policy->cur - 1;

		pr_err("policy is %p, this_dbs_info->freq_table is %p, requested_freq is %d, CPUFREQ_RELATION_H is %d, &index is %p\n",
				policy, this_dbs_info->freq_table,
				requested_freq, CPUFREQ_RELATION_H, &index);*/

		/*cpufreq_frequency_table_target(policy,
				this_dbs_info->freq_table, requested_freq,
				CPUFREQ_RELATION_H, &index);*/

		/*cpufreq_frequency_table_target(policy,
				this_dbs_info->freq_table, (policy->cur - 1),
				CPUFREQ_RELATION_H, &index);*/

		/*if(cpufreq_frequency_table_next_highest(policy,
					this_dbs_info->freq_table, &index)) {
				pr_err("%s: could not get next highest freq\n",
					__func__);
				return;
				}

		requested_freq = this_dbs_info->freq_table[index].frequency;
		__cpufreq_driver_target(policy, requested_freq,
				CPUFREQ_RELATION_L);*/
		/*__cpufreq_driver_target(policy, this_dbs_info->requested_freq,
		  CPUFREQ_RELATION_H);*/

		if (cpufreq_frequency_table_next_lowest(policy,
				this_dbs_info->freq_table, &index)) {
			pr_err("%s: failed to get next lowest frequency\n",
					__func__);
			return;
		}

		__cpufreq_driver_target(policy,
				this_dbs_info->freq_table[index].frequency,
				CPUFREQ_RELATION_L);
	}
}

static void do_dbs_timer(struct work_struct *work)
{
	struct cpu_dbs_info_s *dbs_info =
		container_of(work, struct cpu_dbs_info_s, work.work);
	unsigned int cpu = dbs_info->cpu;

	/* We want all CPU groups to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);
	delay -= jiffies % delay;

	mutex_lock(&dbs_info->timer_mutex);
	dbs_check_cpu(dbs_info);
	queue_delayed_work_on(cpu, khotplug_wq, &dbs_info->work, delay);
	mutex_unlock(&dbs_info->timer_mutex);
}

static inline void dbs_timer_init(struct cpu_dbs_info_s *dbs_info)
{
	/* We want all CPU groups to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);
	delay -= jiffies % delay;

	INIT_DELAYED_WORK_DEFERRABLE(&dbs_info->work, do_dbs_timer);
	queue_delayed_work_on(dbs_info->cpu, khotplug_wq, &dbs_info->work,
		delay);
}

static inline void dbs_timer_exit(struct cpu_dbs_info_s *dbs_info)
{
	cancel_delayed_work_sync(&dbs_info->work);
}

static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
				   unsigned int event)
{
	unsigned int cpu = policy->cpu;
	struct cpu_dbs_info_s *this_dbs_info;
	unsigned int j;
	int rc;

	this_dbs_info = &per_cpu(hp_cpu_dbs_info, cpu);

	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;

		mutex_lock(&dbs_mutex);
#if 0
		rc = sysfs_create_group(&policy->kobj, &dbs_attr_group_old);
		if (rc) {
			mutex_unlock(&dbs_mutex);
			return rc;
		}
#endif
		dbs_enable++;
		for_each_cpu(j, policy->cpus) {
			struct cpu_dbs_info_s *j_dbs_info;
			j_dbs_info = &per_cpu(hp_cpu_dbs_info, j);
			j_dbs_info->cur_policy = policy;

			j_dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&j_dbs_info->prev_cpu_wall);
			if (dbs_tuners_ins.ignore_nice) {
				j_dbs_info->prev_cpu_nice =
						kstat_cpu(j).cpustat.nice;
			}
		}
		this_dbs_info->cpu = cpu;
		this_dbs_info->freq_table = cpufreq_frequency_get_table(cpu);
		/*
		 * Start the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 1) {
			/*unsigned int latency;*/

			rc = sysfs_create_group(cpufreq_global_kobject,
						&dbs_attr_group);
			if (rc) {
				mutex_unlock(&dbs_mutex);
				return rc;
			}

			/* XXX can I get rid of the below block? */
			/* policy latency is in nS. Convert it to uS first */
			/*latency = policy->cpuinfo.transition_latency / 1000;
			if (latency == 0)
				latency = 1;*/
		}
		mutex_unlock(&dbs_mutex);

		mutex_init(&this_dbs_info->timer_mutex);
		dbs_timer_init(this_dbs_info);
		break;

	case CPUFREQ_GOV_STOP:
		dbs_timer_exit(this_dbs_info);

		mutex_lock(&dbs_mutex);
#if 0
		sysfs_remove_group(&policy->kobj, &dbs_attr_group_old);
#endif
		mutex_destroy(&this_dbs_info->timer_mutex);
		dbs_enable--;
		mutex_unlock(&dbs_mutex);
		if (!dbs_enable)
			sysfs_remove_group(cpufreq_global_kobject,
					   &dbs_attr_group);
		if(num_online_cpus() < NR_CPUS)
			enable_nonboot_cpus();
		break;

	case CPUFREQ_GOV_LIMITS:
		mutex_lock(&this_dbs_info->timer_mutex);
		if (policy->max < this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(this_dbs_info->cur_policy,
				policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(this_dbs_info->cur_policy,
				policy->min, CPUFREQ_RELATION_L);
		mutex_unlock(&this_dbs_info->timer_mutex);
		break;
	}
	return 0;
}

static int __init cpufreq_gov_dbs_init(void)
{
	int err;
	cputime64_t wall;
	u64 idle_time;
	int cpu = get_cpu();

	idle_time = get_cpu_idle_time_us(cpu, &wall);
	put_cpu();
	if (idle_time != -1ULL) {
		dbs_tuners_ins.up_threshold = DEF_UP_FREQ_MIN_LOAD;
	} else {
		pr_warn("cpufreq-hotplug: %s: assumes CONFIG_NO_HZ\n",
				__func__);
		return -EINVAL;
	}

	khotplug_wq = create_workqueue("khotplug");
	if (!khotplug_wq) {
		printk(KERN_ERR "Creation of khotplug failed\n");
		return -EFAULT;
	}
	err = cpufreq_register_governor(&cpufreq_gov_hotplug);
	if (err)
		destroy_workqueue(khotplug_wq);

	return err;
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_hotplug);
	destroy_workqueue(khotplug_wq);
}

MODULE_AUTHOR("Mike Turquette <mturquette@ti.com>");
MODULE_DESCRIPTION("'cpufreq_hotplug' - cpufreq governor for dynamic frequency scaling and CPU hotplugging");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_HOTPLUG
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
