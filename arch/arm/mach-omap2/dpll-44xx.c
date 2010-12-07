/*
 * OMAP4 DDR clock node
 *
 * Copyright (C) 2010 Texas Instruments, Inc.
 *
 * Santosh Shilimkar <santosh.shilimkar@ti.com>
 * Rajendra Nayak <rnayak@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/clk.h>

#include <plat/common.h>
#include <plat/clockdomain.h>

#include <mach/emif.h>
#include <mach/omap4-common.h>

#include "clock.h"
#include "cm.h"
#include "cm-regbits-44xx.h"

#define MAX_FREQ_UPDATE_TIMEOUT  100000
#define DPLL_REGM4XEN_ENABLE	0x1

static struct clockdomain *l3_emif_clkdm;

/**
 * omap4_core_dpll_m2_set_rate - set CORE DPLL M2 divider
 * @clk: struct clk * of DPLL to set
 * @rate: rounded target rate
 *
 * Programs the CM shadow registers to update CORE DPLL M2
 * divider. M2 divider is used to clock external DDR and its
 * reconfiguration on frequency change is managed through a
 * hardware sequencer. This is managed by the PRCM with EMIF
 * uding shadow registers.
 * Returns -EINVAL/-1 on error and 0 on success.
 */
int omap4_core_dpll_m2_set_rate(struct clk *clk, unsigned long rate)
{
	int i = 0;
	u32 validrate = 0, shadow_freq_cfg1 = 0, new_div = 0;

	if (!clk || !rate)
		return -EINVAL;

	validrate = omap2_clksel_round_rate_div(clk, rate, &new_div);
	if (validrate != rate)
		return -EINVAL;

	/* Just to avoid look-up on every call to speed up */
	if (!l3_emif_clkdm)
		l3_emif_clkdm = clkdm_lookup("l3_emif_clkdm");

	/* Configures MEMIF domain in SW_WKUP */
	omap2_clkdm_wakeup(l3_emif_clkdm);

	/*
	 * Program EMIF timing parameters in EMIF shadow registers
	 * for targetted DRR clock.
	 * DDR Clock = core_dpll_m2 / 2
	 */
	omap_emif_setup_registers(validrate >> 1, LPDDR2_VOLTAGE_STABLE);

	/*
	 * FREQ_UPDATE sequence:
	 * - DLL_OVERRIDE=0 (DLL lock & code must not be overridden
	 *	after CORE DPLL lock)
	 * - DLL_RESET=1 (DLL must be reset upon frequency change)
	 * - DPLL_CORE_M2_DIV with same value as the one already
	 *	in direct register
	 * - DPLL_CORE_DPLL_EN=0x7 (to make CORE DPLL lock)
	 * - FREQ_UPDATE=1 (to start HW sequence)
	 */
	shadow_freq_cfg1 = (1 << OMAP4430_DLL_RESET_SHIFT) |
			(new_div << OMAP4430_DPLL_CORE_M2_DIV_SHIFT) |
			(DPLL_LOCKED << OMAP4430_DPLL_CORE_DPLL_EN_SHIFT) |
			(1 << OMAP4430_FREQ_UPDATE_SHIFT);
	__raw_writel(shadow_freq_cfg1, OMAP4430_CM_SHADOW_FREQ_CONFIG1);

	/* wait for the configuration to be applied */
	omap_test_timeout(((__raw_readl(OMAP4430_CM_SHADOW_FREQ_CONFIG1)
				& OMAP4430_FREQ_UPDATE_MASK) == 0),
				MAX_FREQ_UPDATE_TIMEOUT, i);

	/* Configures MEMIF domain back to HW_WKUP */
	omap2_clkdm_allow_idle(l3_emif_clkdm);

	if (i == MAX_FREQ_UPDATE_TIMEOUT) {
		pr_err("%s: Frequency update for CORE DPLL M2 change failed\n",
				__func__);
		return -1;
	}

	return 0;
}


/**
 * omap4_prcm_freq_update - set freq_update bit
 *
 * Programs the CM shadow registers to update EMIF
 * parametrs. Few usecase only few registers needs to
 * be updated using prcm freq update sequence.
 * EMIF read-idle control and zq-config needs to be
 * updated for temprature alerts and voltage change
 * Returns -1 on error and 0 on success.
 */
int omap4_set_freq_update(void)
{
	u32 shadow_freq_cfg1;
	int i = 0;

	/* Just to avoid look-up on every call to speed up */
	if (!l3_emif_clkdm)
		l3_emif_clkdm = clkdm_lookup("l3_emif_clkdm");

	/* Configures MEMIF domain in SW_WKUP */
	omap2_clkdm_wakeup(l3_emif_clkdm);

	/*
	 * FREQ_UPDATE sequence:
	 * - DLL_OVERRIDE=0 (DLL lock & code must not be overridden
	 *	after CORE DPLL lock)
	 * - FREQ_UPDATE=1 (to start HW sequence)
	 */
	shadow_freq_cfg1 = __raw_readl(OMAP4430_CM_SHADOW_FREQ_CONFIG1);
	shadow_freq_cfg1 |= (1 << OMAP4430_DLL_RESET_SHIFT) |
			   (1 << OMAP4430_FREQ_UPDATE_SHIFT);
	__raw_writel(shadow_freq_cfg1, OMAP4430_CM_SHADOW_FREQ_CONFIG1);

	/* wait for the configuration to be applied */
	omap_test_timeout(((__raw_readl(OMAP4430_CM_SHADOW_FREQ_CONFIG1)
				& OMAP4430_FREQ_UPDATE_MASK) == 0),
				MAX_FREQ_UPDATE_TIMEOUT, i);

	/* Configures MEMIF domain back to HW_WKUP */
	omap2_clkdm_allow_idle(l3_emif_clkdm);

	if (i == MAX_FREQ_UPDATE_TIMEOUT) {
		pr_err("%s: Frequency update failed\n",	__func__);
		return -1;
	}

	return 0;
}

long omap4_dpll_regm4xen_round_rate(struct clk *clk, unsigned long target_rate)
{
	int ret = 0, regm4xen = 1;
	u32 reg;

	if(strcmp(clk->name, "dpll_abe_ck")) {
		pr_warn("%s: clk is not DPLL_ABE.  Clock data bug?\n",
				__func__);
		ret = -EINVAL;
		goto out;
	}

	/* regm4xen adds a multiplier of 4 to DPLL calculations */
	reg = cm_read_mod_reg(OMAP4430_CM1_CKGEN_MOD,
			OMAP4_CM_CLKMODE_DPLL_ABE_OFFSET);
	if (reg && (DPLL_REGM4XEN_ENABLE << OMAP4430_DPLL_REGM4XEN_SHIFT)) {
#if 0
		/* begin: gross hack to force MN dividers */
		pr_err("%s: GROSS HACK\n", __func__);
		clk->dpll_data->last_rounded_m = 0x1e;
		clk->dpll_data->last_rounded_n = 0x18;
		clk->dpll_data->last_rounded_rate = 196608000;
		goto out;
		/* end: gross hack to force MN dividers */
#endif
		/*
		 * This should be 4 but I have to make this 8 for the math to
		 * come out correctly when determining what the rate is.  Why
		 * is that?
		 */
		regm4xen = 8;
	}

	/*
	 * XXX this is lazy.  sue me.
	 * Basic idea here is to use existing round rate function to generate
	 * MN dividers.  If REGM4XEN is set we divide the desired rate by the
	 * multiplier to trick round_rate function into determining the
	 * correct MN values.
	 */
	ret = omap2_dpll_round_rate(clk, (target_rate / regm4xen));

	pr_err("%s: clk->name is %s, target_rate is %lu, omap2_dpll_round_rate returned %d\n",
			__func__, clk->name, target_rate, ret);

	pr_err("%s: last_rounded_m is %d, last_rounded_n is %d, last_rounded_rate is %lu\n",
			__func__, clk->dpll_data->last_rounded_m,
			clk->dpll_data->last_rounded_n,
			clk->dpll_data->last_rounded_rate);

	clk->dpll_data->last_rounded_rate *= 8;
	pr_err("%s: last_rounded_rate hacked to become %lu\n", __func__,
			clk->dpll_data->last_rounded_rate);

out:
	return clk->dpll_data->last_rounded_rate;
}
