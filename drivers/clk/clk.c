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

struct clk {
	const char		*name;
	struct clk_hw_ops	*ops;
	struct clk_hw		*hw;
	unsigned int		enable_count;
	unsigned int		prepare_count;
	struct clk		*parent;
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

int clk_set_rate(struct clk *clk, unsigned long rate)
{
	/* not yet implemented */
	return -ENOSYS;
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

struct clk *clk_register(struct clk_hw_ops *ops, struct clk_hw *hw,
		const char *name)
{
	struct clk *clk;

	clk = kzalloc(sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return NULL;

	clk->name = name;
	clk->ops = ops;
	clk->hw = hw;
	hw->clk = clk;

	/* Query the hardware for parent and initial rate */

	if (clk->ops->get_parent)
		/* We don't to lock against prepare/enable here, as
		 * the clock is not yet accessible from anywhere */
		clk->parent = clk->ops->get_parent(clk->hw);

	if (clk->ops->recalc_rate)
		clk->rate = clk->ops->recalc_rate(clk->hw);


	return clk;
}
EXPORT_SYMBOL_GPL(clk_register);
