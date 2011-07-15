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

/*
 * per-CPU data is supplied by the driver during initialization.  It consists
 * of two members:
 *
 * struct cpuoffline_partition *cpuoffline_partition_data - a pointer to the
 * partition instance to which this CPU belongs.
 *
 * int can_offline - flag stating whether this CPU can be put offline as a
 * matter of policy; this flag is not meant to declare which CPUs can't go
 * offline due to hardware constraints.
 */

#ifndef _LINUX_CPUOFFLINE_H
#define _LINUX_CPUOFFLINE_H

#define NAME_LEN		16

DECLARE_PER_CPU(struct cpuoffline_partition *, cpuoffline_partition);
DECLARE_PER_CPU(int, cpuoffline_can_offline);

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
};

struct cpuoffline_driver {
	int (*init)(struct cpuoffline_partition *partition);
	int (*exit)(struct cpuoffline_partition *partition);
};

/*
 * sysfs thoughts:
 * /sys/devices/system/cpu/cpuoffline - partition level control
 * 	/sys/devices/system/cpu/cpuoffline/partition1name
 * 	/sys/devices/system/cpu/cpuoffline/partition2name
 *
 * /sys/devices/system/cpu/cpu0/cpuoffline - cpu level control
 * 	/sys/devices/system/cpu/cpu0/cpuoffline/can_offline
 * 	/sys/devices/system/cpu/cpu0/cpuoffline/consider_load
 */

/* registration functions */
/*int cpuoffline_register_governor(struct cpuoffline_governor *governor);
void cpuoffline_unregister_governor(struct cpuoffline_governor *governor);*/

/*int cpuoffline_register_partition(struct cpuoffline_partition *cp);
void cpuoffline_unregister_partition(struct cpuoffline_partition *cp);*/

int cpuoffline_register_driver(struct cpuoffline_driver *driver);
int cpuoffline_unregister_driver(struct cpuoffline_driver *driver);
#endif
