/*
 * devfreq - a framework for scaling device clock frequencies
 *
 * Copyright (C) 2011 Samsung Electronics
 *	MyungJoo Ham <myungjoo.ham@samsung.com>
 * Copyright (C) 2011 Texas Instruments, Inc.
 * 	Mike Turquette <mturquette@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __LINUX_DEVFREQ_H__
#define __LINUX_DEVFREQ_H__

#include <linux/device.h>
#include <linux/notifier.h>

#define DEVFREQ_NAME_LEN 16

/**
 * devfreq_dev_status - track idleness of device
 * @total_time: number of jiffies over measurement period
 * @busy_time: number of jiffies spent working over measurement period
 *
 * XXX paragraph below sucks!  devfreq shouldn't really make any assumptions!
 *
 * One of only two assumptions that devfreq makes about your device is that it
 * can measure its idleness over time.  devfreq_dev_status is uses to capture
 * those values and determine if your device's frequency should scale.
 *
 * XXX this belongs to the governor
 *
 * XXX WRONG! this belongs to devfreq_device->priv_data
 */
struct devfreq_dev_status {
	/* both since the last measure */
	unsigned long total_time;
	unsigned long busy_time;
};

/*
 * FIXME
 * The following members used to be in struct devfreq_device_profile
 * Instead they will be populated by devfreq_driver->init
	struct devfreq_frequency_table **freqs;
	unsigned long min_freq;
	unsigned long max_freq;
 */

/**
 * devfreq_governor - frequency scaling policy for devfreq_device
 * @name:	unique governor name
 * @node:	entry in the global governor list
 * @start:	function to begin governor frequency scaling
 * @stop:	function to halt governor frequency scaling
 *
 * Governors implement the frequency scaling policy for a
 * devfreq_device.  ->start is only called at boot, when loading the
 * devfreq module, after switching to a new governor.  ->stop is called
 * when unloading the module or before switching governors.  The
 * powersave, performance and userspace governors are always compiled
 * in.  If a device does not list a default governor during
 * registration, or requests an unavailable governor then the powersave
 * governor will be used as the default.
 */
struct devfreq_governor {
	char 			name[DEVFREQ_NAME_LEN];
	struct list_head	node;
	struct module		*owner;
	struct kobject		kobj;

	int (*start)(struct devfreq *this);
	void (*stop)(struct devfreq *this);
};

/**
 * devfreq_driver - device data & calbacks needed during registration
 *
 * @gov:		governor to control policy for this device
 * @gov_name:		governor to scale this device's clock rate
 * @sample_rate:	time between governor work in milliseconds
 * @sample:		function that returns busyness percentage
 * @target:		function to scale clock rate to new frequency
 */
struct devfreq_driver {
	struct devfreq_governor *gov; /* OR */
	char *gov_name;
	unsigned int sample_rate;

	int (*sample)(struct devfreq_device *df);
	int (*target)(struct devfreq_device *df, unsigned long rate);
};

/**
 * devfreq_device - device with scalable frequencies
 *
 * @dev:	the device to be scaled
 * @node:	all scalable devices are members of this list
 * @lock:	mutex for operations on this device
 * @freq_nb:	notifier for handling changes in available frequencies
 * @freqs:	table of the devices available frequencies
 * @min_freq:	minimum clock frequency the device supports
 * @max_freq:	maximum clock frequency the device supports
 * @curr_freq:	primary clock frequency the device is running at
 * @gov:	policy for determining clock frequency
 * @gov_data:	used by some devfreq governors
 * 	FIXME should gov_data go away and just have priv_data?
 * 	the shift is away from governors doing:
 * 	new_timestamp - priv_data.old_timestamp = time_delta
 * 	instead the devfreq_device->priv_data can do this for us as a
 * 	part of devfreq_device->sample, which knows the details
 * @priv_data:	used by some devfreq devices for determining idleness
 * @sample:	returns percentage of time device spent idle
 * @target:	scale device to a new rate
 *
 * devfreq devices are used to scale clock frequency for a device whose
 * time-normalized busyness/idlenss can be measured at run time.  If a
 * device's driver (or userspace) has no method to measure a device's
 * busyness/idleness then it should not use devfreq.  The mutex must be
 * held any time a devfreq_device is accessed.
 */
struct devfreq_device {
	struct device *dev;
	struct list_head node;
	struct mutex lock;
	struct notifier_block freq_nb;

	struct devfreq_frequency_table **freqs; /* if NULL then max/min_freq must exist */
	unsigned int min_freq;	/* necessary? yes, for devices without freq_table */
	unsigned int max_freq;	/* necessary? yes, for devices with arbitrary freqs */

	unsigned int curr_freq;
	struct devfreq_governor *gov;

	struct kobject kobj;
	struct completion kobj_unregister;

	/*void *gov_data;*/
	void *priv_data;
	int (*sample)(struct devfreq_device *df);
	int (*target)(struct devfreq_device *df, unsigned long rate);
};

#if defined(CONFIG_PM_DEVFREQ)
/* FIXME change away from "profile" to "driver" model */
extern int devfreq_register_device(struct *dev, struct devfreq_driver *driver);

extern int devfreq_add_device(struct device *dev,
		struct devfreq_device_profile *profile,
		struct devfreq_governor	*governor, void *data);
extern int devfreq_remove_device(struct device *dev);

#else /* !CONFIG_PM_DEVFREQ */
static int devfreq_add_device(struct device *dev,
		struct devfreq_dev_profile *profile,
		struct devfreq_governor *governor, void *data)
{
	return 0;
}

static int devfreq_remove_device(struct device *dev)
{
	return 0;
}

#endif /* CONFIG_PM_DEVFREQ */

#endif /* __LINUX_DEVFREQ_H__ */
