/*
 * CPU Offline framework core
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Mike Turquette <mturquette@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/cpuoffline.h>
#include <linux/slab.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/cpumask.h>

#include <asm/cputime.h>

//#include <linux/sched.h>

/*
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
*/

#define AVGLOAD_DEFAULT_SAMPLING_RATE		1000000
#define AVGLOAD_DEFAULT_ONLINE_THRESHOLD	80
#define AVGLOAD_DEFAULT_OFFLINE_THRESHOLD	20

DEFINE_MUTEX(avgload_mutex);

/* XXX partition->private_data should point to this */
struct avgload_instance {
	struct cpuoffline_partition *partition;
	cputime64_t prev_time_wall;
	struct delayed_work work;
	struct mutex timer_mutex;
	int sampling_rate;
	int online_threshold;
	int offline_threshold;
};

struct avgload_cpu_data {
	cputime64_t prev_time_idle;
	bool offline;
};

/* XXX this seems pretty inefficient... */
DEFINE_PER_CPU(struct avgload_cpu_data, avgload_data);

static void avgload_do_work(struct avgload_instance *instance)
{
	unsigned int cpu;
	cputime64_t cur_time_wall, cur_time_idle;
	unsigned int delta_wall, delta_idle;
	unsigned int load = 0;
	//struct avgload_partition_data *partition_data;
	struct cpuoffline_partition *partition = instance->partition;
	struct avgload_cpu_data *cpu_data;
	struct cpumask partition_online_mask;
	struct cpumask partition_hotplug_mask;
	struct cpumask cpu_offline_mask;

	pr_err("%s: here\n", __func__);

	if (!instance || !partition) {
		pr_err("%s: data does not exist\n", __func__);
		return;
	}

	pr_err("%s: weight of cpu_online_mask is %d\n", __func__, cpumask_weight(cpu_online_mask));
	pr_err("%s: weight of partition->cpus is %d\n", __func__, cpumask_weight(partition->cpus));
	/* find cpu's in this partition that are online */
	cpumask_and(&partition_online_mask, cpu_online_mask, partition->cpus);

	pr_err("%s: weight of partition_online_mask is %d\n", __func__, cpumask_weight(&partition_online_mask));
	if (!cpumask_weight(&partition_online_mask)) {
		pr_err("%s: no cpus are online in this partition.  aborting\n",
				__func__);
		return;
	}

	for_each_cpu(cpu, &partition_online_mask) {
		pr_err("%s: in the loop with CPU%d\n", __func__, cpu);
		cpu_data = &per_cpu(avgload_data, cpu);

		cur_time_idle = get_cpu_idle_time_us(cpu, &cur_time_wall);

		pr_err("%s: cur_time_wall is %lu\n", __func__, cur_time_wall);

		pr_err("%s: instance->prev_time_wall is %lu\n", __func__,
				instance->prev_time_wall);

		delta_wall = (unsigned int) cputime64_sub(cur_time_wall,
				instance->prev_time_wall);

		delta_idle = (unsigned int) cputime64_sub(cur_time_idle,
				cpu_data->prev_time_idle);

		if (!delta_wall || delta_wall < delta_idle)
			continue;

		load += 100 * (delta_wall - delta_idle) / delta_wall;
	}

	/* average the load */
	load /= cpumask_weight(&partition_online_mask);
	pr_err("%s: load is %u\n", __func__, load);

	/* bring a cpu back online */
	if (load > instance->online_threshold) {
		/* which cpu's are offline and support hotplug? */
		cpumask_complement(&cpu_offline_mask, cpu_online_mask);
		cpumask_and(&partition_hotplug_mask,
				&cpu_offline_mask, cpu_hotpluggable_mask);

		/* pick a CPU from the partition that come back online */
		cpu = cpumask_any_and(&partition_hotplug_mask, partition->cpus);

		pr_err("%s: CPU%d up\n", __func__, cpu);
		cpu_up(cpu);

		return;
	}

	/* take a cpu offline */
	if (load < instance->offline_threshold) {
		/* which cpu's are online and support hotplug? */
		cpumask_and(&partition_hotplug_mask,
				cpu_online_mask, cpu_hotpluggable_mask);

		/* pick a CPU from the partition that can go offline */
		cpu = cpumask_any_and(&partition_hotplug_mask, partition->cpus);

		pr_err("%s: CPU%d down\n", __func__, cpu);
		cpu_down(cpu);

		return;
	}
}

static void do_avgload_timer(struct work_struct *work)
{
	int delay;
#if 0
	struct avgload_partition_data *data =
		container_of(work, struct avgload_partition_data, work.work);
	struct cpuoffline_partition *partition = container_of((void *)data,
			struct cpuoffline_partition, private_data);
#endif

	struct avgload_instance *instance =
		container_of(work, struct avgload_instance, work.work);
	struct cpuoffline_partition *partition = instance->partition;

	pr_err("%s: weight of partition->cpus is %d\n", __func__, cpumask_weight(partition->cpus));
	pr_err("%s: instance is %p\n", __func__, instance);
	pr_err("%s: partition is %p\n", __func__, partition);
	//pr_err("%s: partition->private_data is %p\n", partition->private_data);
	mutex_lock(&instance->timer_mutex);

	/* do the work */
	avgload_do_work(instance);

	delay = usecs_to_jiffies(instance->sampling_rate);
	schedule_delayed_work(&instance->work, delay);

	mutex_unlock(&instance->timer_mutex);
}

static void avgload_timer_init(struct avgload_instance *instance)
{
	int delay = usecs_to_jiffies(instance->sampling_rate);

	INIT_DELAYED_WORK_DEFERRABLE(&instance->work, do_avgload_timer);
	schedule_delayed_work(&instance->work, delay);
}

static void avgload_timer_exit(struct avgload_instance *instance)
{
	cancel_delayed_work_sync(&instance->work);
}

static int cpuoffline_avgload_start(struct cpuoffline_partition *partition)
{
	struct cpuoffline_governor *gov;
	//struct avgload_partition_data *partition_data;
	struct avgload_instance *instance;
	struct avgload_cpu_data *cpu_data;
	int cpu;

	pr_err("%s: weight of partition->cpus is %d\n", __func__, cpumask_weight(partition->cpus));
	/* XXX kick off wq that prints hello, world */
	pr_err("%s: OMG HI2U\n", __func__);

#if 0
	/* XXX where does private_data get allocated?  here? */
	if (!partition->private_data) {
		pr_err("%s: no partition data\n", __func__);
		partition->private_data =
			kmalloc(sizeof(struct avgload_partition_data),
					GFP_KERNEL);
	}

	if (!partition->private_data) {
		pr_err("%s: bummer, no memory\n", __func__);
		return -ENOMEM;
	}

	pr_err("%s: partition->private_data is %p\n", __func__, partition->private_data);
	partition_data = partition->private_data;
#endif

	instance = kmalloc(sizeof(struct avgload_instance), GFP_KERNEL);
	if (!instance)
		return -ENOMEM;

	gov = partition->governor;
	if (!gov) {
		pr_err("%s: no governor\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&avgload_mutex);

	instance->partition = partition;
	instance->sampling_rate = AVGLOAD_DEFAULT_SAMPLING_RATE;
	instance->online_threshold = AVGLOAD_DEFAULT_ONLINE_THRESHOLD;
	instance->offline_threshold = AVGLOAD_DEFAULT_OFFLINE_THRESHOLD;
	//instance->prev_time_wall = ktime_to_us(ktime_get());

	/* populate idle times before kicking off the workqueue */
	for_each_cpu(cpu, partition->cpus) {
		pr_err("%s: cpu is %d\n", __func__, cpu);
		cpu_data = &per_cpu(avgload_data, cpu);

		cpu_data->prev_time_idle = get_cpu_idle_time_us(cpu,
				&instance->prev_time_wall);
	}

	/* XXX initialize sysfs stuff here */

	mutex_unlock(&avgload_mutex);

	mutex_init(&instance->timer_mutex);
	avgload_timer_init(instance);

	return 0;
}

static int cpuoffline_avgload_stop(struct cpuoffline_partition *partition)
{
	/* XXX stop workqueue and print "finished" */
	pr_err("%s: OMG BAI2U\n", __func__);
	return 0;
}

struct cpuoffline_governor cpuoffline_governor_avgload = {
	.name	= "avgload",
	.owner	= THIS_MODULE,
	.start	= cpuoffline_avgload_start,
	.stop	= cpuoffline_avgload_stop,
};

static int __init cpuoffline_avgload_init(void)
{
	pr_err("%s: registering avload\n", __func__);
	return cpuoffline_register_governor(&cpuoffline_governor_avgload);
}

static void __exit cpuoffline_avgload_exit(void)
{
	cpuoffline_unregister_governor(&cpuoffline_governor_avgload);
}

MODULE_AUTHOR("Mike Turquette <mturquette@ti.com>");
MODULE_DESCRIPTION("cpuoffline_avgload - offline CPUs based on their load");
MODULE_LICENSE("GPL");

module_init(cpuoffline_avgload_init);
