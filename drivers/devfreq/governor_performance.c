/*
 *  linux/drivers/devfreq/governor_performance.c
 *
 *  Copyright (C) 2011 Samsung Electronics
 *	MyungJoo Ham <myungjoo.ham@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/devfreq.h>

static int devfreq_performance_func(struct devfreq *df,
				    unsigned long *freq)
{
	*freq = UINT_MAX; /* devfreq_do will run "floor" */
	return 0;
}

struct devfreq_governor devfreq_performance = {
	.name = "performance",
	.get_target_freq = devfreq_performance_func,
};
