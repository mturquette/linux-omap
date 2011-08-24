/*
 * Copyright (C) 2010-2011 Canonical Ltd <jeremy.kerr@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Standard functionality for the common clock API.
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/device.h>

struct clk {
	const char		*name;
	const struct clk_hw_ops	*ops;
	struct clk_hw		*hw;
	unsigned int		enable_count;
	unsigned int		prepare_count;
	struct clk		*parent;
	struct hlist_head	children;
	struct hlist_node	child_node;
	unsigned long		rate;
};

static DEFINE_SPINLOCK(enable_lock);
static DEFINE_MUTEX(prepare_lock);

static void __clk_unprepare(struct clk *clk)
{
	if (!clk)
		return;

	if (WARN_ON(clk->prepare_count == 0))
		return;

	if (--clk->prepare_count > 0)
		return;

	WARN_ON(clk->enable_count > 0);

	if (clk->ops->unprepare)
		clk->ops->unprepare(clk->hw);

	__clk_unprepare(clk->parent);
}

void clk_unprepare(struct clk *clk)
{
	mutex_lock(&prepare_lock);
	__clk_unprepare(clk);
	mutex_unlock(&prepare_lock);
}
EXPORT_SYMBOL_GPL(clk_unprepare);

static int __clk_prepare(struct clk *clk)
{
	int ret = 0;

	if (!clk)
		return 0;

	if (clk->prepare_count == 0) {
		ret = __clk_prepare(clk->parent);
		if (ret)
			return ret;

		if (clk->ops->prepare) {
			ret = clk->ops->prepare(clk->hw);
			if (ret) {
				__clk_unprepare(clk->parent);
				return ret;
			}
		}
	}

	clk->prepare_count++;

	return 0;
}

int clk_prepare(struct clk *clk)
{
	int ret;

	mutex_lock(&prepare_lock);
	ret = __clk_prepare(clk);
	mutex_unlock(&prepare_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(clk_prepare);

static void __clk_disable(struct clk *clk)
{
	if (!clk)
		return;

	if (WARN_ON(clk->enable_count == 0))
		return;

	if (--clk->enable_count > 0)
		return;

	if (clk->ops->disable)
		clk->ops->disable(clk->hw);
	__clk_disable(clk->parent);
}

void clk_disable(struct clk *clk)
{
	unsigned long flags;

	spin_lock_irqsave(&enable_lock, flags);
	__clk_disable(clk);
	spin_unlock_irqrestore(&enable_lock, flags);
}
EXPORT_SYMBOL_GPL(clk_disable);

static int __clk_enable(struct clk *clk)
{
	int ret;

	if (!clk)
		return 0;

	if (WARN_ON(clk->prepare_count == 0))
		return -ESHUTDOWN;


	if (clk->enable_count == 0) {
		ret = __clk_enable(clk->parent);
		if (ret)
			return ret;

		if (clk->ops->enable) {
			ret = clk->ops->enable(clk->hw);
			if (ret) {
				__clk_disable(clk->parent);
				return ret;
			}
		}
	}

	clk->enable_count++;
	return 0;
}

int clk_enable(struct clk *clk)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&enable_lock, flags);
	ret = __clk_enable(clk);
	spin_unlock_irqrestore(&enable_lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(clk_enable);

unsigned long clk_get_rate(struct clk *clk)
{
	if (!clk)
		return 0;
	return clk->rate;
}
EXPORT_SYMBOL_GPL(clk_get_rate);

long clk_round_rate(struct clk *clk, unsigned long rate)
{
	if (clk && clk->ops->round_rate)
		return clk->ops->round_rate(clk->hw, rate);
	return rate;
}
EXPORT_SYMBOL_GPL(clk_round_rate);

/*
 * clk_recalc_rates - Given a clock (with a recently updated clk->rate),
 *	notify its children that the rate may need to be recalculated, using
 *	ops->recalc_rate().
 */
static void clk_recalc_rates(struct clk *clk)
{
	struct hlist_node *tmp;
	struct clk *child;

	if (clk->ops->recalc_rate)
		clk->rate = clk->ops->recalc_rate(clk->hw);

	hlist_for_each_entry(child, tmp, &clk->children, child_node)
		clk_recalc_rates(child);
}

int clk_set_rate(struct clk *clk, unsigned long rate)
{
	unsigned long parent_rate, new_rate;
	int ret;

	if (!clk->ops->set_rate)
		return -ENOSYS;

	new_rate = rate;

	/* prevent racing with updates to the clock topology */
	mutex_lock(&prepare_lock);

propagate:
	ret = clk->ops->set_rate(clk->hw, new_rate, &parent_rate);

	if (ret < 0)
		return ret;

	/* ops->set_rate may require the parent's rate to change (to
	 * parent_rate), we need to propagate the set_rate call to the
	 * parent.
	 */
	if (ret == CLK_SET_RATE_PROPAGATE) {
		new_rate = parent_rate;
		clk = clk->parent;
		goto propagate;
	}

	/* If successful (including propagation to the parent clock(s)),
	 * recalculate the rates of the clock, including children.  We'll be
	 * calling this on the 'parent-most' clock that we propagated to.
	 */
	clk_recalc_rates(clk);

	mutex_unlock(&prepare_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(clk_set_rate);

struct clk *clk_get_parent(struct clk *clk)
{
	if (!clk)
		return NULL;

	return clk->parent;
}
EXPORT_SYMBOL_GPL(clk_get_parent);

int clk_set_parent(struct clk *clk, struct clk *parent)
{
	/* not yet implemented */
	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(clk_set_parent);

struct clk *clk_register(struct device *dev, const struct clk_hw_ops *ops,
			 struct clk_hw *hw, const char *name)
{
	struct clk *clk;
	char *new_name;
	size_t name_len;

	clk = kzalloc(sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return NULL;

	clk->ops = ops;
	clk->hw = hw;
	hw->clk = clk;

	/* Since we currently match clock providers on a purely string
	 * based method add a prefix based on the device name if a
	 * device is provided.  When we have support for device tree
	 * based clock matching it should be possible to avoid this
	 * mangling and instead use the struct device to hook into
	 * the bindings.
	 *
	 * As we don't currently support unregistering clocks we don't
	 * need to worry about cleanup as yet.
	 */
	if (dev) {
		name_len = strlen(name) + strlen(dev_name(dev)) + 2;
		new_name = kzalloc(name_len, GFP_KERNEL);
		if (!new_name)
			goto err;

		snprintf(new_name, name_len, "%s-%s", dev_name(dev), name);

		clk->name = new_name;
	} else {
		clk->name = name;
	}

	/* Query the hardware for parent and initial rate. We may alter
	 * the clock topology, making this clock available from the parent's
	 * children list. So, we need to protect against concurrent
	 * accesses through set_rate
	 */
	mutex_lock(&prepare_lock);

	if (clk->ops->get_parent) {
		clk->parent = clk->ops->get_parent(clk->hw);
		if (clk->parent)
			hlist_add_head(&clk->child_node,
					&clk->parent->children);
	}

	if (clk->ops->recalc_rate)
		clk->rate = clk->ops->recalc_rate(clk->hw);

	mutex_unlock(&prepare_lock);

	return clk;

err:
	kfree(clk);
	return NULL;
}
EXPORT_SYMBOL_GPL(clk_register);
