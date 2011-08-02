/*
 * cpuoffline.h
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Mike Turquette <mturquette@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/cpu.h>
#include <linux/mutex.h>

#ifndef _LINUX_CPUOFFLINE_H
#define _LINUX_CPUOFFLINE_H

#define MAX_NAME_LEN		16

//DECLARE_PER_CPU(struct cpuoffline_partition *, cpuoffline_partition);
//DECLARE_PER_CPU(int, cpuoffline_can_offline);

struct cpuoffline_partition;

struct cpuoffline_governor {
	char			name[MAX_NAME_LEN];
	struct list_head	governor_list;
	struct mutex		mutex;
	struct module		*owner;
	int (*start)(struct cpuoffline_partition *partition);
	int (*stop)(struct cpuoffline_partition *partition);
	struct kobject kobj;
#if 0
	int (*governor)(struct cpufreq_policy *policy,
			unsigned int event);
	ssize_t (*show_setspeed)        (struct cpufreq_policy *policy,
			char *buf);
	int     (*store_setspeed)       (struct cpufreq_policy *policy,
			unsigned int freq);
	unsigned int max_transition_latency; /* HW must be able to switch to
						next freq faster than this value in nano secs or we
						will fallback to performance governor */
#endif
};

#if 0
struct cpuoffline_governor {
	char		name[MAX_NAME_LEN];
};
#endif

/**
 * cpuoffline_parition - set of CPUs affected by a CPUoffline governor
 *
 * @cpus - bitmask of CPUs managed by this partition
 * @cpus_can_offline - bitmask of CPUs in this partition that can go offline
 * @min_cpus_online - limit how many CPUs are offline for performance
 * @max_cpus_online - limits how many CPUs are online for power capping
 * @cpuoffline_governor - governor policy for hotplugging CPUs
 */
struct cpuoffline_partition {
	int				id;
	char				name[MAX_NAME_LEN];
	cpumask_var_t			cpus;
	/*cpumask_var_t			cpus_can_offline;*/
	int				min_cpus_online;
	/*int				max_cpus_online;*/
	struct cpuoffline_governor	*governor;

	struct kobject			kobj;
	struct completion		kobj_unregister;

	struct mutex			mutex;
	/* XXX hack for testing */
	char				gov_string[MAX_NAME_LEN];
};

struct cpuoffline_driver {
	int (*init)(struct cpuoffline_partition *partition);
	int (*exit)(struct cpuoffline_partition *partition);
};

/* kobject/sysfs definitions */
struct cpuoffline_attribute {
	struct attribute attr;
	ssize_t (*show)(struct cpuoffline_partition *partition, char *buf);
	ssize_t (*store)(struct cpuoffline_partition *partition,
			const char *buf, size_t count);
};

/* registration functions */

int cpuoffline_register_governor(struct cpuoffline_governor *governor);
void cpuoffline_unregister_governor(struct cpuoffline_governor *governor);

int cpuoffline_register_driver(struct cpuoffline_driver *driver);
int cpuoffline_unregister_driver(struct cpuoffline_driver *driver);
#endif
