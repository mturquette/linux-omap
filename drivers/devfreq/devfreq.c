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

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/opp.h>
#include <linux/devfreq.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/list.h>
#include <linux/printk.h>
#include <linux/hrtimer.h>

/*
 * devfreq_work periodically monitors every registered device.
 * The minimum polling interval is one jiffy. The polling interval is
 * determined by the minimum polling period among all polling devfreq
 * devices. The resolution of polling interval is one jiffy.
 */
static bool polling;
static struct workqueue_struct *devfreq_wq;
static struct delayed_work devfreq_work;

/* The list of all device-devfreq */
static LIST_HEAD(devfreq_list);
static DEFINE_MUTEX(devfreq_list_lock);

/**
 * find_device_devfreq() - find devfreq struct using device pointer
 * @dev:	device pointer used to lookup device devfreq.
 *
 * Search the list of device devfreqs and return the matched device's
 * devfreq info. devfreq_list_lock should be held by the caller.
 */
static struct devfreq *find_device_devfreq(struct device *dev)
{
	struct devfreq *tmp_devfreq;

	if (unlikely(IS_ERR_OR_NULL(dev))) {
		pr_err("DEVFREQ: %s: Invalid parameters\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	list_for_each_entry(tmp_devfreq, &devfreq_list, node) {
		if (tmp_devfreq->dev == dev)
			return tmp_devfreq;
	}

	return ERR_PTR(-ENODEV);
}

/**
 * get_devfreq() - find devfreq struct. a wrapped find_device_devfreq()
 *		with mutex protection. exported for governors
 * @dev:	device pointer used to lookup device devfreq.
 */
struct devfreq *get_devfreq(struct device *dev)
{
	struct devfreq *ret;

	mutex_lock(&devfreq_list_lock);
	ret = find_device_devfreq(dev);
	mutex_unlock(&devfreq_list_lock);

	return ret;
}

/**
 * devfreq_do() - Check the usage profile of a given device and configure
 *		frequency and voltage accordingly
 * @devfreq:	devfreq info of the given device
 */
static int devfreq_do(struct devfreq *devfreq)
{
	struct opp *opp;
	unsigned long freq;
	int err;

	err = devfreq->governor->get_target_freq(devfreq, &freq);
	if (err)
		return err;

	opp = opp_find_freq_ceil(devfreq->dev, &freq);
	if (opp == ERR_PTR(-ENODEV))
		opp = opp_find_freq_floor(devfreq->dev, &freq);

	if (IS_ERR(opp))
		return PTR_ERR(opp);

	if (devfreq->previous_freq == freq)
		return 0;

	err = devfreq->profile->target(devfreq->dev, opp);
	if (err)
		return err;

	devfreq->previous_freq = freq;
	return 0;
}

/**
 * update_devfreq() - Notify that the device OPP or frequency requirement
 *		has been changed. This function is exported for governors.
 * @devfreq:	the devfreq instance.
 *
 * Note: lock devfreq->lock before calling update_devfreq
 */
int update_devfreq(struct devfreq *devfreq)
{
	int err = 0;

	if (!mutex_is_locked(&devfreq->lock)) {
		WARN(true, "devfreq->lock must be locked by the caller.\n");
		return -EINVAL;
	}

	/* Reevaluate the proper frequency */
	err = devfreq_do(devfreq);
	return err;
}

/**
 * devfreq_update() - Notify that the device OPP has been changed.
 * @dev:	the device whose OPP has been changed.
 *
 * Called by OPP notifier.
 */
static int devfreq_update(struct notifier_block *nb, unsigned long type,
			  void *devp)
{
	struct devfreq *devfreq = container_of(nb, struct devfreq, nb);
	int ret;

	mutex_lock(&devfreq->lock);
	ret = update_devfreq(devfreq);
	mutex_unlock(&devfreq->lock);

	return ret;
}

/**
 * devfreq_monitor() - Periodically run devfreq_do()
 * @work: the work struct used to run devfreq_monitor periodically.
 *
 */
static void devfreq_monitor(struct work_struct *work)
{
	static unsigned long last_polled_at;
	struct devfreq *devfreq, *tmp;
	int error;
	unsigned long jiffies_passed;
	unsigned long next_jiffies = ULONG_MAX, now = jiffies;

	/* Initially last_polled_at = 0, polling every device at bootup */
	jiffies_passed = now - last_polled_at;
	last_polled_at = now;
	if (jiffies_passed == 0)
		jiffies_passed = 1;

	mutex_lock(&devfreq_list_lock);

	list_for_each_entry_safe(devfreq, tmp, &devfreq_list, node) {
		mutex_lock(&devfreq->lock);

		if (devfreq->next_polling == 0) {
			mutex_unlock(&devfreq->lock);
			continue;
		}

		/*
		 * Reduce more next_polling if devfreq_wq took an extra
		 * delay. (i.e., CPU has been idled.)
		 */
		if (devfreq->next_polling <= jiffies_passed) {
			error = devfreq_do(devfreq);

			/* Remove a devfreq with an error. */
			if (error && error != -EAGAIN) {
				dev_err(devfreq->dev, "Due to devfreq_do error(%d), devfreq(%s) is removed from the device\n",
					error, devfreq->governor->name);

				list_del(&devfreq->node);
				mutex_unlock(&devfreq->lock);
				kfree(devfreq);
				continue;
			}
			devfreq->next_polling = devfreq->polling_jiffies;

			/* No more polling required (polling_ms changed) */
			if (devfreq->next_polling == 0) {
				mutex_unlock(&devfreq->lock);
				continue;
			}
		} else {
			devfreq->next_polling -= jiffies_passed;
		}

		next_jiffies = (next_jiffies > devfreq->next_polling) ?
				devfreq->next_polling : next_jiffies;

		mutex_unlock(&devfreq->lock);
	}

	if (next_jiffies > 0 && next_jiffies < ULONG_MAX) {
		polling = true;
		queue_delayed_work(devfreq_wq, &devfreq_work, next_jiffies);
	} else {
		polling = false;
	}

	mutex_unlock(&devfreq_list_lock);
}

/**
 * devfreq_add_device() - Add devfreq feature to the device
 * @dev:	the device to add devfreq feature.
 * @profile:	device-specific profile to run devfreq.
 * @governor:	the policy to choose frequency.
 * @data:	private data for the governor. The devfreq framework does not
 *		touch this value.
 */
int devfreq_add_device(struct device *dev, struct devfreq_dev_profile *profile,
		       struct devfreq_governor *governor, void *data)
{
	struct devfreq *devfreq;
	struct srcu_notifier_head *nh;
	int err = 0;

	if (!dev || !profile || !governor) {
		dev_err(dev, "%s: Invalid parameters.\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&devfreq_list_lock);

	devfreq = find_device_devfreq(dev);
	if (!IS_ERR(devfreq)) {
		dev_err(dev, "%s: Unable to create devfreq for the device. It already has one.\n", __func__);
		err = -EINVAL;
		goto out;
	}

	devfreq = kzalloc(sizeof(struct devfreq), GFP_KERNEL);
	if (!devfreq) {
		dev_err(dev, "%s: Unable to create devfreq for the device\n",
			__func__);
		err = -ENOMEM;
		goto out;
	}

	mutex_init(&devfreq->lock);
	mutex_lock(&devfreq->lock);
	devfreq->dev = dev;
	devfreq->profile = profile;
	devfreq->governor = governor;
	devfreq->next_polling = devfreq->polling_jiffies
			      = msecs_to_jiffies(devfreq->profile->polling_ms);
	devfreq->previous_freq = profile->initial_freq;
	devfreq->data = data;

	devfreq->nb.notifier_call = devfreq_update;

	nh = opp_get_notifier(dev);
	if (IS_ERR(nh)) {
		err = PTR_ERR(nh);
		goto err_opp;
	}
	err = srcu_notifier_chain_register(nh, &devfreq->nb);
	if (err)
		goto err_opp;

	if (governor->init)
		err = governor->init(devfreq);
	if (err)
		goto err_init;

	list_add(&devfreq->node, &devfreq_list);

	if (devfreq_wq && devfreq->next_polling && !polling) {
		polling = true;
		queue_delayed_work(devfreq_wq, &devfreq_work,
				   devfreq->next_polling);
	}
	mutex_unlock(&devfreq->lock);
	goto out;
err_init:
	srcu_notifier_chain_unregister(nh, &devfreq->nb);
err_opp:
	mutex_unlock(&devfreq->lock);
	kfree(devfreq);
out:
	mutex_unlock(&devfreq_list_lock);
	return err;
}

/**
 * devfreq_remove_device() - Remove devfreq feature from a device.
 * @device:	the device to remove devfreq feature.
 */
int devfreq_remove_device(struct device *dev)
{
	struct devfreq *devfreq;
	struct srcu_notifier_head *nh;
	int err = 0;

	if (!dev)
		return -EINVAL;

	mutex_lock(&devfreq_list_lock);
	devfreq = find_device_devfreq(dev);
	if (IS_ERR(devfreq)) {
		err = PTR_ERR(devfreq);
		goto out;
	}

	mutex_lock(&devfreq->lock);
	nh = opp_get_notifier(dev);
	if (IS_ERR(nh)) {
		err = PTR_ERR(nh);
		mutex_unlock(&devfreq->lock);
		goto out;
	}

	list_del(&devfreq->node);

	if (devfreq->governor->exit)
		devfreq->governor->exit(devfreq);

	srcu_notifier_chain_unregister(nh, &devfreq->nb);
	mutex_unlock(&devfreq->lock);
	kfree(devfreq);
out:
	mutex_unlock(&devfreq_list_lock);
	return 0;
}

/**
 * devfreq_init() - Initialize data structure for devfreq framework and
 *		  start polling registered devfreq devices.
 */
static int __init devfreq_init(void)
{
	mutex_lock(&devfreq_list_lock);
	polling = false;
	devfreq_wq = create_freezable_workqueue("devfreq_wq");
	INIT_DELAYED_WORK_DEFERRABLE(&devfreq_work, devfreq_monitor);
	mutex_unlock(&devfreq_list_lock);

	devfreq_monitor(&devfreq_work.work);
	return 0;
}
late_initcall(devfreq_init);
