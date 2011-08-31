/*
 * devfreq: Generic Dynamic Voltage and Frequency Scaling (DVFS) Framework
 *	    for Non-CPU Devices Based on OPP.
 *
 * Copyright (C) 2011 Samsung Electronics
 *	MyungJoo Ham <myungjoo.ham@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __LINUX_DEVFREQ_H__
#define __LINUX_DEVFREQ_H__

#include <linux/device.h>
#include <linux/notifier.h>
#include <linux/opp.h>

#define DEVFREQ_NAME_LEN 16

struct devfreq;
struct devfreq_dev_status {
	/* both since the last measure */
	unsigned long total_time;
	unsigned long busy_time;
	unsigned long current_frequency;
};

struct devfreq_dev_profile {
	unsigned long max_freq; /* may be larger than the actual value */
	unsigned long initial_freq;
	unsigned int polling_ms;	/* 0 for at opp change only */

	int (*target)(struct device *dev, struct opp *opp);
	int (*get_dev_status)(struct device *dev,
			      struct devfreq_dev_status *stat);
};

/**
 * struct devfreq_governor - Devfreq policy governor
 * @name		Governor's name
 * @get_target_freq	Returns desired operating frequency for the device.
 *			Basically, get_target_freq will run
 *			devfreq_dev_profile.get_dev_status() to get the
 *			status of the device (load = busy_time / total_time).
 * @init		Called when the devfreq is being attached to a device
 * @exit		Called when the devfreq is being removed from a device
 *
 * Note that the callbacks are called with devfreq->lock locked by devfreq.
 */
struct devfreq_governor {
	char name[DEVFREQ_NAME_LEN];
	int (*get_target_freq)(struct devfreq *this, unsigned long *freq);
	int (*init)(struct devfreq *this);
	void (*exit)(struct devfreq *this);
};

/**
 * struct devfreq - Device devfreq structure
 * @node	list node - contains the devices with devfreq that have been
 *		registered.
 * @lock	a mutex to protect accessing devfreq.
 * @dev		device pointer
 * @profile	device-specific devfreq profile
 * @governor	method how to choose frequency based on the usage.
 * @nb		notifier block registered to the corresponding OPP to get
 *		notified for frequency availability updates.
 * @polling_jiffies	interval in jiffies.
 * @previous_freq	previously configured frequency value.
 * @next_polling	the number of remaining jiffies to poll with
 *			"devfreq_monitor" executions to reevaluate
 *			frequency/voltage of the device. Set by
 *			profile's polling_ms interval.
 * @data	Private data of the governor. The devfreq framework does not
 *		touch this.
 *
 * This structure stores the devfreq information for a give device.
 *
 * Note that when a governor accesses entries in struct devfreq in its
 * functions except for the context of callbacks defined in struct
 * devfreq_governor, the governor should protect its access with the
 * struct mutex lock in struct devfreq. A governor may use this mutex
 * to protect its own private data in void *data as well.
 */
struct devfreq {
	struct list_head node;

	struct mutex lock;
	struct device *dev;
	struct devfreq_dev_profile *profile;
	struct devfreq_governor *governor;
	struct notifier_block nb;

	unsigned long polling_jiffies;
	unsigned long previous_freq;
	unsigned int next_polling;

	void *data; /* private data for governors */
};

#if defined(CONFIG_PM_DEVFREQ)
extern int devfreq_add_device(struct device *dev,
			   struct devfreq_dev_profile *profile,
			   struct devfreq_governor *governor,
			   void *data);
extern int devfreq_remove_device(struct device *dev);
#else /* !CONFIG_PM_DEVFREQ */
static int devfreq_add_device(struct device *dev,
			   struct devfreq_dev_profile *profile,
			   struct devfreq_governor *governor,
			   void *data)
{
	return 0;
}

static int devfreq_remove_device(struct device *dev)
{
	return 0;
}
#endif /* CONFIG_PM_DEVFREQ */

#endif /* __LINUX_DEVFREQ_H__ */
