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

#define AVGLOAD_SAMPLING_RATE	1000000

DEFINE_MUTEX(avgload_mutex);

/* XXX partition->private_data should point to this */
struct avgload_partition_data {
	/*cputime64_t prev_time_idle*/;
	cputime64_t prev_time_wall;
	struct delayed_work work;
	struct mutex timer_mutex;
	int sampling_rate;
};

struct avgload_cpu_data {
	cputime64_t prev_time_idle;
	bool offline;
};

/* XXX this seems pretty inefficient... */
DEFINE_PER_CPU(struct avgload_cpu_data, avgload_data);

static void avgload_do_work(struct cpuoffline_partition *partition)
{
	unsigned int cpu;
	cputime64_t cur_time_wall, cur_time_idle;
	unsigned int delta_wall, delta_idle;
	unsigned int load = 0;
	struct avgload_partition_data *partition_data;
	struct avgload_cpu_data *cpu_data;

	pr_err("%s: here\n", __func__);

	partition_data = partition->private_data;

	for_each_cpu(cpu, partition->cpus) {
		cpu_data = &per_cpu(cpu, avgload_data);

		cur_time_idle = get_cpu_idle_time_us(cpu, &cur_time_wall);

		delta_wall = (unsigned int) cputime64_sub(cur_wall_time,
				partition_data->prev_time_wall);

		delta_idle = (unsigned int) cputime64_sub(cur_idle_time,
				cpu_data->prev_time_idle);

		if (!wall_time || wall_time < idle_time)
			continue;

		load += 100 * (delta_wall - delta_idle) / delta_wall;
	}

	load /= cpumask_weight(partition->cpus);

	if (load > partition_data->online_threshold) {
		/*
		 * XXX this should be the intersection point between:
		 * 	cpumask_hotpluggable
		 * 	partition->cpus
		 * 	!cpumask_online
		 */
		cpu_up(...);
		goto out;
	}

	if (load < partition_data->offline_threshold) {
		/*
		 * XXX this should be the intersection point between:
		 * 	cpumask_hotpluggable
		 * 	partition->cpus
		 * 	cpumask_online
		 */
		cpu_down(...);
		goto out;
	}

out:
	return 0;
}

static void do_avgload_timer(struct work_struct *work)
{
	int delay;
	struct avgload_partition_data *data =
		container_of(work, struct avgload_partition_data, work.work);
	struct cpuoffline_partition *partition = container_of((void *)data,
			struct cpuoffline_partition, private_data);

	mutex_lock(&data->timer_mutex);

	/* do the work */
	avgload_do_work(partition);

	delay = usecs_to_jiffies(data->sampling_rate);
	schedule_delayed_work(&data->work, delay);

	mutex_unlock(&data->timer_mutex);
}

static void avgload_timer_init(struct avgload_partition_data *data)
{
	int delay = usecs_to_jiffies(data->sampling_rate);

	INIT_DELAYED_WORK_DEFERRABLE(&data->work, do_avgload_timer);
	schedule_delayed_work(&data->work, delay);
}

static void avgload_timer_exit(struct avgload_partition_data *data)
{
	cancel_delayed_work_sync(&data->work);
}

static int cpuoffline_avgload_start(struct cpuoffline_partition *partition)
{
	struct cpuoffline_governor *gov;
	struct avgload_partition_data *partition_data;
	struct avgload_cpu_data *cpu_data;
	int cpu;

	/* XXX kick off wq that prints hello, world */
	pr_err("%s: OMG HI2U\n", __func__);

	/* XXX where does private_data get allocated?  here? */
	if (!partition->private_data) {
		pr_err("%s: no partition data\n", __func__);
		partition->private_data =
			kmalloc(sizeof(struct avgload_partition_data),
					GFP_KERNEL);
	}

	partition_data = partition->private_data;

	gov = partition->governor;
	if (!gov) {
		pr_err("%s: no governor\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&avgload_mutex);

	partition_data->sampling_rate = AVGLOAD_SAMPLING_RATE;

	/* populate idle times before kicking off the workqueue */
	for_each_cpu(cpu, partition->cpus) {
		pr_err("%s: cpu is %d\n", __func__, cpu);
		cpu_data = &per_cpu(avgload_data, cpu);

		cpu_data->prev_time_idle = get_cpu_idle_time_us(cpu,
				&partition_data->prev_time_wall);
	}

	/* XXX initialize sysfs stuff here */

	mutex_unlock(&avgload_mutex);

	mutex_init(&partition_data->timer_mutex);
	avgload_timer_init(partition_data);

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
