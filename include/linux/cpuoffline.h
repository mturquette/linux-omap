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

#ifndef _LINUX_CPUOFFLINE_H
#define _LINUX_CPUOFFLINE_H

#define NAME_LEN		16

DECLARE_PER_CPU(struct cpuoffline_partition *, cpuoffline_partition);
DECLARE_PER_CPU(int, cpuoffline_can_offline);

/* struct attribute should give us show/store function pointers */
struct cpuoffline_attribute {
	struct attribute attr;
	ssize_t (*show)(struct cpuoffline_partition *partition, char *buf);
	ssize_t (*store)(struct cpuoffline_partition *partition,
			const char *buf, size_t count);
};

struct cpuoffline_governor {
	char		name[NAME_LEN];
};

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
	cpumask_var_t			cpus;
	cpumask_var_t			cpus_can_offline;
	int				min_cpus_online;
	int				max_cpus_online;
	struct cpuoffline_governor	*governor;

	struct kobject			kobj;
	struct completion		kobj_unregister;

	/* XXX hack for testing */
	char gov_string[16];
};

struct cpuoffline_driver {
	int (*init)(struct cpuoffline_partition *partition);
	int (*exit)(struct cpuoffline_partition *partition);
};

/* registration functions */

/*
int cpuoffline_register_governor(struct cpuoffline_governor *governor);
void cpuoffline_unregister_governor(struct cpuoffline_governor *governor);
*/

int cpuoffline_register_driver(struct cpuoffline_driver *driver);
int cpuoffline_unregister_driver(struct cpuoffline_driver *driver);
#endif
