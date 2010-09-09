/*
 * OMAP4 OPP table definitions.
 *
 * Copyright (C) 2009 - 2010 Texas Instruments Incorporated.
 *	Nishanth Menon
 * Copyright (C) 2009 - 2010 Deep Root Systems, LLC.
 *	Kevin Hilman
 * Copyright (C) 2010 Nokia Corporation.
 *      Eduardo Valentin
 * Copyright (C) 2010 Texas Instruments Incorporated.
 *	Thara Gopinath
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 * History:
 */

#include <linux/module.h>
#include <linux/err.h>
#include <linux/delay.h>

#include <plat/opp.h>
#include <plat/clock.h>

#include "cm-regbits-34xx.h"
#include "prm.h"
#include "opp44xx.h"

static struct clk *dpll_mpu_clk, *iva_clk, *dsp_clk, *l3_clk;
static struct clk *core_m3_clk, *core_m6_clk, *per_m3_clk, *per_m6_clk;

static struct omap_opp_def __initdata omap44xx_opp_def_list[] = {
	/* MPU OPP1 - OPP50 */
	OMAP_OPP_DEF("mpu", true, 300000000, 930000),
	/* MPU OPP2 - OPP100 */
	OMAP_OPP_DEF("mpu", true, 600000000, 1100000),
	/* MPU OPP3 - OPP-Turbo */
	OMAP_OPP_DEF("mpu", true, 800000000, 1260000),
	/* MPU OPP4 - OPP-SB */
	OMAP_OPP_DEF("mpu", true, 1008000000, 1350000),
	/* IVA OPP1 - OPP50 */
	OMAP_OPP_DEF("iva", true,  133000000, 930000),
	/* IVA OPP2 - OPP100 */
	OMAP_OPP_DEF("iva", true,  266000000, 1100000),
	/* IVA OPP3 - OPP-Turbo */
	OMAP_OPP_DEF("iva", false, 332000000, 1260000),
	/* DSP OPP1 - OPP50 */
	OMAP_OPP_DEF("dsp", true, 232800000, 930000),
	/* DSP OPP2 - OPP100 */
	OMAP_OPP_DEF("dsp", true, 465600000, 1100000),
	/* DSP OPP3 - OPPTB */
	OMAP_OPP_DEF("dsp", false, 498000000, 1260000),
	/* ABE OPP1 - OPP50 */
	OMAP_OPP_DEF("l4_abe", true, 98300000, 930000),
	/* ABE OPP2 - OPP100 */
	OMAP_OPP_DEF("l4_abe", true, 196600000, 1100000),
	/* ABE OPP3 - OPPTB */
	OMAP_OPP_DEF("l4_abe", false, 196600000, 1260000),
	/* L3 OPP1 - OPP50 */
	OMAP_OPP_DEF("l3_main_1", true, 100000000, 930000),
	/* L3 OPP2 - OPP100, OPP-Turbo, OPP-SB */
	OMAP_OPP_DEF("l3_main_1", true, 200000000, 1100000),
	/* CAM FDIF OPP1 - OPP50 */
	OMAP_OPP_DEF("fdif", true, 64000000, 930000),
	/* CAM FDIF OPP2 - OPP100 */
	OMAP_OPP_DEF("fdif", true, 128000000, 1100000),
	/* SGX OPP1 - OPP50 */
	OMAP_OPP_DEF("gpu", true, 100000000, 930000),
	/* SGX OPP2 - OPP100 */
	OMAP_OPP_DEF("gpu", true, 200000000, 1100000),
};

#define	L3_OPP50_RATE			100000000
#define DPLL_CORE_M3_OPP50_RATE		100000000
#define DPLL_CORE_M3_OPP100_RATE	160000000
#define DPLL_CORE_M6_OPP50_RATE		100000000
#define DPLL_CORE_M6_OPP100_RATE	133300000
#define DPLL_PER_M3_OPP50_RATE		96000000
#define DPLL_PER_M3_OPP100_RATE		128000000
#define DPLL_PER_M6_OPP50_RATE		96000000
#define DPLL_PER_M6_OPP100_RATE		192000000

static u32 omap44xx_opp_def_size = ARRAY_SIZE(omap44xx_opp_def_list);

static unsigned long omap4_mpu_get_rate(struct device *dev);

#ifndef CONFIG_CPU_FREQ
static unsigned long compute_lpj(unsigned long ref, u_int div, u_int mult)
{
	unsigned long new_jiffy_l, new_jiffy_h;

	/*
	 * Recalculate loops_per_jiffy.  We do it this way to
	 * avoid math overflow on 32-bit machines.  Maybe we
	 * should make this architecture dependent?  If you have
	 * a better way of doing this, please replace!
	 *
	 *    new = old * mult / div
	 */
	new_jiffy_h = ref / div;
	new_jiffy_l = (ref % div) / 100;
	new_jiffy_h *= mult;
	new_jiffy_l = new_jiffy_l * mult / div;

	return new_jiffy_h + new_jiffy_l * 100;
}
#endif

static int omap4_mpu_set_rate(struct device *dev, unsigned long rate)
{
	unsigned long cur_rate = omap4_mpu_get_rate(dev);
	int ret;

#ifdef CONFIG_CPU_FREQ
	struct cpufreq_freqs freqs_notify;

	freqs_notify.old = cur_rate / 1000;
	freqs_notify.new = rate / 1000;
	freqs_notify.cpu = 0;
	/* Send pre notification to CPUFreq */
	cpufreq_notify_transition(&freqs_notify, CPUFREQ_PRECHANGE);
#endif
	ret = clk_set_rate(dpll_mpu_clk, rate);
	if (ret) {
		dev_warn(dev, "%s: Unable to set rate to %ld\n",
			__func__, rate);
		return ret;
	}

#ifdef CONFIG_CPU_FREQ
	/* Send a post notification to CPUFreq */
	cpufreq_notify_transition(&freqs_notify, CPUFREQ_POSTCHANGE);
#endif

#ifndef CONFIG_CPU_FREQ
	/*Update loops_per_jiffy if processor speed is being changed*/
	loops_per_jiffy = compute_lpj(loops_per_jiffy,
			cur_rate / 1000, rate / 1000);
#endif
	return 0;
}

static unsigned long omap4_mpu_get_rate(struct device *dev)
{
	return dpll_mpu_clk->rate;
}

static int omap4_iva_set_rate(struct device *dev, unsigned long rate)
{
	if (dev == omap2_get_iva_device()) {
		unsigned long round_rate;
		/*
		 * Round rate is required as the actual iva clock rate is
		 * weird for OMAP4
		 */
		round_rate = clk_round_rate(iva_clk, rate / 2);
		return clk_set_rate(iva_clk, round_rate);
	} else if (dev == omap4_get_dsp_device()) {
		return clk_set_rate(dsp_clk, rate / 2);
	} else {
		dev_warn(dev, "%s: Wrong device pointer\n", __func__);
		return -EINVAL;
	}

}

static unsigned long omap4_iva_get_rate(struct device *dev)
{
	if (dev == omap2_get_iva_device()) {
		return iva_clk->rate * 2;
	} else if (dev == omap4_get_dsp_device()) {
		return dsp_clk->rate * 2;
	} else {
		dev_warn(dev, "%s: Wrong device pointer\n", __func__);
		return -EINVAL;
	}
}

static int omap4_l3_set_rate(struct device *dev, unsigned long rate)
{
	u32 d_core_m3_rate, d_core_m6_rate, d_per_m3_rate, d_per_m6_rate;

	if (rate <= L3_OPP50_RATE) {
		d_core_m3_rate = DPLL_CORE_M3_OPP50_RATE;
		d_core_m6_rate = DPLL_CORE_M6_OPP50_RATE;
		d_per_m3_rate = DPLL_PER_M3_OPP50_RATE;
		d_per_m6_rate = DPLL_PER_M6_OPP50_RATE;
	} else {
		d_core_m3_rate = DPLL_CORE_M3_OPP100_RATE;
		d_core_m6_rate = DPLL_CORE_M6_OPP100_RATE;
		d_per_m3_rate = DPLL_PER_M3_OPP100_RATE;
		d_per_m6_rate = DPLL_PER_M6_OPP100_RATE;
	}

	clk_set_rate(core_m3_clk, d_core_m3_rate);
	d_core_m6_rate = clk_round_rate(core_m6_clk, d_core_m6_rate);
	clk_set_rate(core_m6_clk, d_core_m6_rate);
	clk_set_rate(per_m3_clk, d_per_m3_rate);
	clk_set_rate(per_m6_clk, d_per_m6_rate);
	return clk_set_rate(l3_clk, rate);
}

static unsigned long omap4_l3_get_rate(struct device *dev)
{
	return l3_clk->rate;
}

/* Temp variable to allow multiple calls */
static u8 __initdata omap4_table_init;

int __init omap4_pm_init_opp_table(void)
{
	struct omap_opp_def *opp_def;
	struct device *dev;
	int i, r;

	/*
	 * Allow multiple calls, but initialize only if not already initalized
	 * even if the previous call failed, coz, no reason we'd succeed again
	 */
	if (omap4_table_init)
		return 0;
	omap4_table_init = 1;

	opp_def = omap44xx_opp_def_list;

	for (i = 0; i < omap44xx_opp_def_size; i++) {
		r = opp_add(opp_def++);
		if (r)
			pr_err("unable to add OPP %ld Hz for %s\n",
				opp_def->freq, opp_def->hwmod_name);
	}

	dpll_mpu_clk = clk_get(NULL, "dpll_mpu_ck");
	iva_clk = clk_get(NULL, "dpll_iva_m5_ck");
	dsp_clk = clk_get(NULL, "dpll_iva_m4_ck");
	l3_clk = clk_get(NULL, "dpll_core_m5_ck");
	core_m3_clk = clk_get(NULL, "dpll_core_m3_ck");
	core_m6_clk = clk_get(NULL, "dpll_core_m6_ck");
	per_m3_clk = clk_get(NULL, "dpll_per_m3_ck");
	per_m6_clk = clk_get(NULL, "dpll_per_m6_ck");

	/* Populate the set rate and get rate for mpu, iva, dsp and l3 device */
	dev = omap2_get_mpuss_device();
	if (dev)
		opp_populate_rate_fns(dev, omap4_mpu_set_rate,
				omap4_mpu_get_rate);

	dev = omap2_get_iva_device();
	if (dev)
		opp_populate_rate_fns(dev, omap4_iva_set_rate,
				omap4_iva_get_rate);

	dev = omap4_get_dsp_device();
	if (dev)
		opp_populate_rate_fns(dev, omap4_iva_set_rate,
				omap4_iva_get_rate);

	dev = omap2_get_l3_device();
	if (dev)
		opp_populate_rate_fns(dev, omap4_l3_set_rate,
				omap4_l3_get_rate);

	return 0;
}
