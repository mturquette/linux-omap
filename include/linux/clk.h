/*
 *  linux/include/linux/clk.h
 *
 *  Copyright (C) 2004 ARM Limited.
 *  Written by Deep Blue Solutions Limited.
 *  Copyright (c) 2010-2011 Jeremy Kerr <jeremy.kerr@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __LINUX_CLK_H
#define __LINUX_CLK_H

#include <linux/err.h>
#include <linux/spinlock.h>

struct device;

struct clk;

#ifdef CONFIG_GENERIC_CLK

struct clk_hw {
	struct clk *clk;
};

/**
 * struct clk_hw_ops -  Callback operations for hardware clocks; these are to
 * be provided by the clock implementation, and will be called by drivers
 * through the clk_* API.
 *
 * @prepare:	Prepare the clock for enabling. This must not return until
 *		the clock is fully prepared, and it's safe to call clk_enable.
 *		This callback is intended to allow clock implementations to
 *		do any initialisation that may sleep. Called with
 *		prepare_lock held.
 *
 * @unprepare:	Release the clock from its prepared state. This will typically
 *		undo any work done in the @prepare callback. Called with
 *		prepare_lock held.
 *
 * @enable:	Enable the clock atomically. This must not return until the
 *		clock is generating a valid clock signal, usable by consumer
 *		devices. Called with enable_lock held. This function must not
 *		sleep.
 *
 * @disable:	Disable the clock atomically. Called with enable_lock held.
 *		This function must not sleep.
 *
 * @recalc_rate	Recalculate the rate of this clock, by quering hardware
 *		and/or the clock's parent. Called with the global clock mutex
 *		held. Optional, but recommended - if this op is not set,
 *		clk_get_rate will return 0.
 *
 * @get_parent	Query the parent of this clock; for clocks with multiple
 *		possible parents, query the hardware for the current
 *		parent. Currently only called when the clock is first
 *		registered.
 *
 * @set_rate	Change the rate of this clock. If this callback returns
 *		CLK_SET_RATE_PROPAGATE, the rate change will be propagated to
 *		the parent clock (which may propagate again). The requested
 *		rate of the parent is passed back from the callback in the
 *		second 'unsigned long *' argument.
 *
 * The clk_enable/clk_disable and clk_prepare/clk_unprepare pairs allow
 * implementations to split any work between atomic (enable) and sleepable
 * (prepare) contexts.  If a clock requires sleeping code to be turned on, this
 * should be done in clk_prepare. Switching that will not sleep should be done
 * in clk_enable.
 *
 * Typically, drivers will call clk_prepare when a clock may be needed later
 * (eg. when a device is opened), and clk_enable when the clock is actually
 * required (eg. from an interrupt). Note that clk_prepare *must* have been
 * called before clk_enable.
 */
struct clk_hw_ops {
	int		(*prepare)(struct clk_hw *);
	void		(*unprepare)(struct clk_hw *);
	int		(*enable)(struct clk_hw *);
	void		(*disable)(struct clk_hw *);
	unsigned long	(*recalc_rate)(struct clk_hw *);
	long		(*round_rate)(struct clk_hw *, unsigned long);
	int		(*set_rate)(struct clk_hw *,
					unsigned long, unsigned long *);
	struct clk *	(*get_parent)(struct clk_hw *);
};

enum {
	CLK_SET_RATE_PROPAGATE = 1,
};

/**
 * clk_prepare - prepare clock for atomic enabling.
 *
 * @clk: The clock to prepare
 *
 * Do any possibly sleeping initialisation on @clk, allowing the clock to be
 * later enabled atomically (via clk_enable). This function may sleep.
 */
int clk_prepare(struct clk *clk);

/**
 * clk_unprepare - release clock from prepared state
 *
 * @clk: The clock to release
 *
 * Do any (possibly sleeping) cleanup on clk. This function may sleep.
 */
void clk_unprepare(struct clk *clk);

/* Base clock implementations. Platform clock implementations can use these
 * directly, or 'subclass' as approprate */

#ifdef CONFIG_GENERIC_CLK_FIXED

struct clk_hw_fixed {
	struct clk_hw	hw;
	unsigned long	rate;
};

extern struct clk_hw_ops clk_fixed_ops;

#endif /* CONFIG_GENERIC_CLK_FIXED */

#ifdef CONFIG_GENERIC_CLK_GATE

struct clk_gate {
	struct clk_hw	hw;
	void __iomem	*reg;
	u8		bit_idx;
};

extern struct clk_hw_ops clk_gate_ops;

#endif /* CONFIG_GENERIC_CLK_GATE */

/**
 * clk_register - register and initialize a new clock
 *
 * @dev: device providing the clock or NULL
 * @ops: ops for the new clock
 * @hw: struct clk_hw to be passed to the ops of the new clock
 * @name: name to use for the new clock
 *
 * Register a new clock with the clk subsytem.  If dev is provided
 * then it will be used to disambiguate between multiple instances of
 * the same device in the system, typically this should only be done
 * for devices that are not part of the core SoC unless device tree is
 * in use.
 *
 * Returns either a struct clk for the new clock or a NULL pointer.
 */
struct clk *clk_register(struct device *dev, const struct clk_hw_ops *ops,
			 struct clk_hw *hw, const char *name);

/**
 * clk_unregister - remove a clock
 *
 * @clk: clock to unregister
 *
 * Remove a clock from the clk subsystem.  This is currently not
 * implemented but is provided to allow unregistration code to be
 * written in drivers ready for use when an implementation is
 * provided.
 */
static inline int clk_unregister(struct clk *clk)
{
	return -ENOTSUPP;
}

#else /* !CONFIG_GENERIC_CLK */

/*
 * For !CONFIG_GENERIC_CLK, we don't enforce any atomicity
 * requirements for clk_enable/clk_disable, so the prepare and unprepare
 * functions are no-ops
 */
static inline int clk_prepare(struct clk *clk) { return 0; }
static inline void clk_unprepare(struct clk *clk) { }

#endif /* !CONFIG_GENERIC_CLK */

/**
 * clk_get - lookup and obtain a reference to a clock producer.
 * @dev: device for clock "consumer"
 * @id: clock comsumer ID
 *
 * Returns a struct clk corresponding to the clock producer, or
 * valid IS_ERR() condition containing errno.  The implementation
 * uses @dev and @id to determine the clock consumer, and thereby
 * the clock producer.  (IOW, @id may be identical strings, but
 * clk_get may return different clock producers depending on @dev.)
 *
 * Drivers must assume that the clock source is not enabled.
 *
 * clk_get should not be called from within interrupt context.
 */
struct clk *clk_get(struct device *dev, const char *id);

/**
 * clk_enable - inform the system when the clock source should be running.
 * @clk: clock source
 *
 * If the clock can not be enabled/disabled, this should return success.
 *
 * Returns success (0) or negative errno.
 */
int clk_enable(struct clk *clk);

/**
 * clk_disable - inform the system when the clock source is no longer required.
 * @clk: clock source
 *
 * Inform the system that a clock source is no longer required by
 * a driver and may be shut down.
 *
 * Implementation detail: if the clock source is shared between
 * multiple drivers, clk_enable() calls must be balanced by the
 * same number of clk_disable() calls for the clock source to be
 * disabled.
 */
void clk_disable(struct clk *clk);

/**
 * clk_get_rate - obtain the current clock rate (in Hz) for a clock source.
 *		  This is only valid once the clock source has been enabled.
 *		  Returns zero if the clock rate is unknown.
 * @clk: clock source
 */
unsigned long clk_get_rate(struct clk *clk);

/**
 * clk_put	- "free" the clock source
 * @clk: clock source
 *
 * Note: drivers must ensure that all clk_enable calls made on this
 * clock source are balanced by clk_disable calls prior to calling
 * this function.
 *
 * clk_put should not be called from within interrupt context.
 */
void clk_put(struct clk *clk);

/**
 * clk_round_rate - adjust a rate to the exact rate a clock can provide
 * @clk: clock source
 * @rate: desired clock rate in Hz
 *
 * Returns rounded clock rate in Hz, or negative errno.
 */
long clk_round_rate(struct clk *clk, unsigned long rate);
 
/**
 * clk_set_rate - set the clock rate for a clock source
 * @clk: clock source
 * @rate: desired clock rate in Hz
 *
 * Returns success (0) or negative errno.
 */
int clk_set_rate(struct clk *clk, unsigned long rate);
 
/**
 * clk_set_parent - set the parent clock source for this clock
 * @clk: clock source
 * @parent: parent clock source
 *
 * Returns success (0) or negative errno.
 */
int clk_set_parent(struct clk *clk, struct clk *parent);

/**
 * clk_get_parent - get the parent clock source for this clock
 * @clk: clock source
 *
 * Returns struct clk corresponding to parent clock source, or
 * valid IS_ERR() condition containing errno.
 */
struct clk *clk_get_parent(struct clk *clk);

/**
 * clk_get_sys - get a clock based upon the device name
 * @dev_id: device name
 * @con_id: connection ID
 *
 * Returns a struct clk corresponding to the clock producer, or
 * valid IS_ERR() condition containing errno.  The implementation
 * uses @dev_id and @con_id to determine the clock consumer, and
 * thereby the clock producer. In contrast to clk_get() this function
 * takes the device name instead of the device itself for identification.
 *
 * Drivers must assume that the clock source is not enabled.
 *
 * clk_get_sys should not be called from within interrupt context.
 */
struct clk *clk_get_sys(const char *dev_id, const char *con_id);

/**
 * clk_add_alias - add a new clock alias
 * @alias: name for clock alias
 * @alias_dev_name: device name
 * @id: platform specific clock name
 * @dev: device
 *
 * Allows using generic clock names for drivers by adding a new alias.
 * Assumes clkdev, see clkdev.h for more info.
 */
int clk_add_alias(const char *alias, const char *alias_dev_name, char *id,
			struct device *dev);

#endif
