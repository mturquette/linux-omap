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

static int cpuoffline_avgload_start(struct cpuoffline_partition *partition)
{
	/* XXX kick off wq that prints hello, world */
	pr_err("%s: OMG HI2U\n", __func__);
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
	pr_err("%s: registering avload\n");
	return cpuoffline_register_governor(&cpuoffline_governor_avgload);
}

static void __exit cpuoffline_avgload_exit(void)
{
	cpuoffline_unregister_governor(&cpuoffline_governor_avgload);
}

MODULE_AUTHOR("Mike Turquette <mturquette@ti.com>");
MODULE_DESCRIPTION("cpuoffline_avgload - policy for offlining CPUs based on "
		"their load");
MODULE_LICENSE("GPL");

module_init(cpuoffline_avgload_init);
