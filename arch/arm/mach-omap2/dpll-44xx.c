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
#include <plat/prcm.h>

#include <mach/emif.h>
#include <mach/omap4-common.h>

#include "clock.h"
#include "cm.h"
#include "cm-regbits-44xx.h"
#include "prm-regbits-44xx.h"

#define MAX_FREQ_UPDATE_TIMEOUT  100000
#define DPLL_ABE_CLKSEL_SYS_32K	0x1
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

int omap4_dpll_low_power_cascade_enter()
{
	int ret = 0;
	struct clk *dpll_abe_ck, *dpll_core_ck, dpll_mpu_ck;
	struct clk *sys_32k_ck;
	struct clk *abe_clk, *abe_dpll_refclk_mux_ck;
	unsigned long clk_rate;

	/*
	 * Reparent DPLL_ABE so that it is fed by SYS_32K_CK.  Magical
	 * REGM4XEN registers allows us to multiply MN dividers by 4 so that
	 * we can get 196.608MHz out of DPLL_ABE (other DPLLs do not have this
	 * feature).  Divide the output of that clock by 4 so that AESS
	 * functional clock can be 48.152MHz.
	 */

	pr_err("%s\n", __func__);

	dpll_abe_ck = clk_get(NULL, "dpll_abe_ck");
	if (!dpll_abe_ck)
		pr_err("%s: could not get dpll_abe_ck\n", __func__);

	sys_32k_ck = clk_get(NULL, "sys_32k_ck");
	if (!sys_32k_ck)
		pr_err("%s: could not get sys_32k_ck\n", __func__);

	abe_clk = clk_get(NULL, "abe_clk");
	if (!abe_clk)
		pr_err("%s: could not get abe_clk\n", __func__);

	abe_dpll_refclk_mux_ck = clk_get(NULL, "abe_dpll_refclk_mux_ck");
	if (!abe_dpll_refclk_mux_ck)
		pr_err("%s: could not get abe_dpll_refclk_mux_ck\n", __func__);

	/* Device RET/OFF are not supported in DPLL cascading; gate them */
	omap3_dpll_deny_idle(dpll_abe_ck);

	/* should I bypass DPLL_ABE here? */

	/* set SYS_32K_CK as input to DPLL_ABE */
	/* the right way to switch to 32k clk */
	ret = omap2_clk_set_parent(abe_dpll_refclk_mux_ck, sys_32k_ck);

	pr_err("%s: omap2_clk_set_parent returns %d\n", __func__, ret);

	/* the hacky way to switch to 32k clk */
	/*omap4_prm_rmw_reg_bits(OMAP4430_DPLL_ABE_CLKSEL_MASK,
			DPLL_ABE_CLKSEL_SYS_32K,
			OMAP4430_CM_ABE_PLL_REF_CLKSEL);*/

	/* multiply MN dividers by 4; only supported by DPLL_ABE */
	/* should this be done while bypassed? what is the proper sequence? */
	cm_rmw_mod_reg_bits(OMAP4430_DPLL_REGM4XEN_MASK,
			DPLL_REGM4XEN_ENABLE << OMAP4430_DPLL_REGM4XEN_SHIFT,
			OMAP4430_CM1_CKGEN_MOD,
			OMAP4_CM_CLKMODE_DPLL_ABE_OFFSET);

	/* program DPLL_ABE for 196.608MHz */
	/*
	 * XXX this should use the new omap4_dpll_regm4xen_round_rate and
	 * relock it
	 */
	clk_set_rate(dpll_abe_ck, 196608000);

	/* enable DPLL_ABE if not done already */
	clk_enable(dpll_abe_ck);

	pr_err("%s: clk_get_rate(dpll_abe_ck) is %lu\n",
			__func__, clk_get_rate(dpll_abe_ck));

	/* XXX below code is not yet ready */
#if 0

	/* divide 196.608MHz by 4 to get for AESS clock */
	clk_set_rate(abe_clk, 49152000);

	/* check if CLKSEL_OPP is 0x2 */
	if(!(cm_read_mod_reg(OMAP4430_CM1_CKGEN_MOD,
					OMAP4_CM_CLKSEL_ABE_OFFSET) && 0x2)) {
		pr_err("%s: CKGEN_CM1.CM_CLKSEL_ABB.CLKSEL_OPP is not 0x2\n",
				__func__);
		ret = -EINVAL;
		goto out_clksel_opp;
	}
#endif

out_clksel_opp:
out:
	return ret;
}

int omap4_dpll_low_power_cascade_exit()
{
	pr_err("%s\n", __func__);
	return 0;
}
