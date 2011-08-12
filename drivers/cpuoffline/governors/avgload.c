/*
 * CPU Offline Average Load governor
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Mike Turquette <mturquette@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/types.h>
#include <linux/cpuoffline.h>
#include <linux/slab.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/cpumask.h>

#include <asm/cputime.h>

#define AVGLOAD_DEFAULT_SAMPLING_RATE		1000000
#define AVGLOAD_DEFAULT_ONLINE_THRESHOLD	80
#define AVGLOAD_DEFAULT_OFFLINE_THRESHOLD	20

DEFINE_MUTEX(avgload_mutex);

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
static DEFINE_PER_CPU(struct avgload_cpu_data, avgload_data);

static void avgload_do_work(struct avgload_instance *instance)
{
	unsigned int cpu;
	cputime64_t cur_time_wall, cur_time_idle;
	cputime64_t delta_wall, delta_idle;
	u64 load = 0;
	struct cpuoffline_partition *partition = instance->partition;
	struct cpumask mask;

	if (!instance || !partition) {
		pr_warning("%s: data does not exist\n", __func__);
		return;
	}

	/* find CPUs in this partition that are online */
	cpumask_and(&mask, cpu_online_mask, partition->cpus);

	/* this should only happen if CPUs are offlined from userspace */
	if (!cpumask_weight(&mask)) {
		pr_err("%s: no cpus are online in this partition.  aborting\n",
				__func__);
		return;
	}

	/* determine load for all online CPUs in the partition */
	for_each_cpu(cpu, &mask) {
		cur_time_idle = get_cpu_idle_time_us(cpu, &cur_time_wall);

		delta_wall = cputime64_sub(cur_time_wall,
				instance->prev_time_wall);
		delta_idle = cputime64_sub(cur_time_idle,
				per_cpu(avgload_data, cpu).prev_time_idle);

		per_cpu(avgload_data, cpu).prev_time_idle = cur_time_idle;

		/* rollover happens often when bringing a CPU back online */
		if (!delta_wall || delta_wall < delta_idle)
			continue;

		/* aggregate load */
		delta_idle = 100 * (delta_wall - delta_idle);
		do_div(delta_idle, delta_wall);
		load += delta_idle;
	}

	/* save last timestamp for next iteration */
	instance->prev_time_wall = cur_time_wall;

	/* average the load */
	do_div(load, cpumask_weight(&mask));

	/* bring a cpu back online */
	if (load > instance->online_threshold) {
		/* which CPUs are offline? */
		cpumask_complement(&mask, cpu_online_mask);

		/* which offline CPUs are in this partition? */
		cpumask_and(&mask, &mask, partition->cpus);

		/* which offline CPUs in this partition can hotplug? */
		cpumask_and(&mask, &mask, cpu_hotpluggable_mask);

		/* bail out if all CPUs are online */
		if (!cpumask_weight(&mask))
			return;

		/* pick a "random" CPU to bring online */
		cpu = cpumask_any(&mask);

		cpu_up(cpu);

		return;
	}

	/* take a cpu offline */
	if (load < instance->offline_threshold) {
		/* can any of those CPUs hotplug? */
		cpumask_and(&mask, &mask, cpu_hotpluggable_mask);

		if (!cpumask_weight(&mask))
			return;

		/* pick a "random" CPU to go offline */
		cpu = cpumask_any(&mask);

		cpu_down(cpu);

		return;
	}
}

static void do_avgload_timer(struct work_struct *work)
{
	int delay;
	struct avgload_instance *instance =
		container_of(work, struct avgload_instance, work.work);

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
	struct avgload_instance *instance;
	struct avgload_cpu_data *cpu_data;
	int cpu;

	instance = kmalloc(sizeof(struct avgload_instance), GFP_KERNEL);
	if (!instance)
		return -ENOMEM;

	gov = partition->governor;
	if (!gov) {
		pr_err("%s: no governor\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&avgload_mutex);

	/* initialize defaults */
	instance->sampling_rate = AVGLOAD_DEFAULT_SAMPLING_RATE;
	instance->online_threshold = AVGLOAD_DEFAULT_ONLINE_THRESHOLD;
	instance->offline_threshold = AVGLOAD_DEFAULT_OFFLINE_THRESHOLD;

	/* remember who we are */
	instance->partition = partition;
	partition->private_data = instance;

	/* populate idle times before kicking off the workqueue */
	for_each_cpu(cpu, partition->cpus) {
		cpu_data = &per_cpu(avgload_data, cpu);

		cpu_data->prev_time_idle = (cputime_t) get_cpu_idle_time_us(cpu,
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
	struct avgload_instance *instance = partition->private_data;

	if (!instance)
		return -EINVAL;

	mutex_lock(&instance->timer_mutex);
	avgload_timer_exit(instance);
	mutex_unlock(&instance->timer_mutex);

	mutex_lock(&partition->mutex);
	partition->private_data = NULL;
	mutex_unlock(&partition->mutex);

	kfree(instance);

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
	pr_notice("%s: registering avgload\n", __func__);
	return cpuoffline_register_governor(&cpuoffline_governor_avgload);
}

static void __exit cpuoffline_avgload_exit(void)
{
	pr_notice("%s: unregistering avgload\n", __func__);
	cpuoffline_unregister_governor(&cpuoffline_governor_avgload);
}

MODULE_AUTHOR("Mike Turquette <mturquette@ti.com>");
MODULE_DESCRIPTION("cpuoffline_avgload - offline CPUs based on their load");
MODULE_LICENSE("GPL");

module_init(cpuoffline_avgload_init);
