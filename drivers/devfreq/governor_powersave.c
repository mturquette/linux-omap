/*
 *  linux/drivers/devfreq/governor_powersave.c
 *
 *  Copyright (C) 2011 Samsung Electronics
 *	MyungJoo Ham <myungjoo.ham@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/devfreq.h>

static int devfreq_powersave_func(struct devfreq *df,
				  unsigned long *freq)
{
	*freq = 0; /* devfreq_do will run "ceiling" to 0 */
	return 0;
}

struct devfreq_governor devfreq_powersave = {
	.name = "powersave",
	.get_target_freq = devfreq_powersave_func,
};
