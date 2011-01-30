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
#include "clock44xx.h"
#include "cm.h"
#include "cm-regbits-44xx.h"
#include "prm-regbits-44xx.h"

#define MAX_FREQ_UPDATE_TIMEOUT  100000
#define DPLL_REGM4XEN_ENABLE	0x1

u32 cm_clkmode_dpll_abe_mask =
		(OMAP4430_DPLL_REGM4XEN_MASK |
		 OMAP4430_DPLL_LPMODE_EN_MASK |
		 OMAP4430_DPLL_RELOCK_RAMP_EN_MASK |
		 OMAP4430_DPLL_RAMP_RATE_MASK |
		 OMAP4430_DPLL_DRIFTGUARD_EN_MASK);

static struct clockdomain *l3_emif_clkdm;

static struct dpll_cascade_saved_state {
	unsigned long dpll_abe_ck_rate;
	unsigned long cm_clkmode_dpll_abe;
	struct clk *abe_dpll_refclk_mux_ck_parent;
	unsigned long dpll_mpu_ck_rate;
	unsigned long dpll_iva_ck_rate;
	unsigned long div_mpu_hs_clk_rate;
	unsigned long div_iva_hs_clk_rate;
	struct clk *iva_hsd_byp_clk_mux_ck_parent;
	struct clk *core_hsd_byp_clk_mux_ck_parent;
	unsigned long div_core_ck_rate;
	unsigned long dpll_core_m2_ck_rate;
	unsigned long dpll_core_m5x2_ck_rate;
	struct clk *l4_wkup_clk_mux_ck_parent;
	struct clk *pmd_stm_clock_mux_ck_parent;
	struct clk *pmd_trace_clk_mux_ck_parent;
	u32 clkreqctrl;
} state;

/**
 * omap4_core_dpll_m2_set_rate - set CORE DPLL M2 divider
 * @clk: struct clk * of DPLL to set
 * @rate: rounded target rate
 *
 * Programs the CM shadow registers to update CORE DPLL M2 divider. M2 divider
 * is used to clock external DDR and its reconfiguration on frequency change
 * is managed through a hardware sequencer. This is managed by the PRCM with
 * EMIF using shadow registers.  If rate specified matches DPLL_CORE's bypass
 * clock rate then put it in Low-Power Bypass.
 * Returns negative int on error and 0 on success.
 */
int omap4_core_dpll_m2_set_rate(struct clk *clk, unsigned long rate)
{
	u32 validrate = 0, shadow_freq_cfg1 = 0, new_div = 0;
	struct clk *dpll_core_ck;
	struct dpll_data *dd;

	/* DPLL_CORE is parent of CORE_M2 */
	dpll_core_ck = clk_get(NULL, "dpll_core_ck");
	dd = dpll_core_ck->dpll_data;

	if (!clk || !rate)
		return -EINVAL;

	if (!dpll_core_ck || !dd)
		return -ENODEV;

	/* check for bypass rate */
	if (rate == dd->clk_bypass->rate) {
		/*
		 * DDR clock = DPLL_CORE_M2_CK / 2.  Program EMIF timing
		 * parameters in EMIF shadow registers for bypass clock rate
		 * divided by 2
		 */
		omap_emif_setup_registers(rate / 2, LPDDR2_VOLTAGE_STABLE);

		/*
		 * program CM_DIV_M2_DPLL_CORE.DPLL_CLKOUT_DIV for divide by
		 * two and put DPLL_CORE into LP Bypass
		 */
		shadow_freq_cfg1 = (0x2 << 11) | (0x5 << 8);
		__raw_writel(shadow_freq_cfg1, OMAP4430_CM_SHADOW_FREQ_CONFIG1);
	} else {
		/* check for valid rate to lock DPLL_CORE */
		validrate = omap2_clksel_round_rate_div(clk, rate, &new_div);
		if (validrate != rate)
			return -EINVAL;

		/*
		 * DDR clock = DPLL_CORE_M2_CK / 2.  Program EMIF timing
		 * parameters in EMIF shadow registers for validrate divided
		 * by 2.
		 */
		omap_emif_setup_registers(validrate / 2, LPDDR2_VOLTAGE_STABLE);

		/*
		 * program DPLL_CORE_M2_DIV with same value as the one already
		 * in direct register and lock DPLL_CORE
		 */
		shadow_freq_cfg1 = (new_div <<
				OMAP4430_DPLL_CORE_M2_DIV_SHIFT) |
			(DPLL_LOCKED << OMAP4430_DPLL_CORE_DPLL_EN_SHIFT);
		__raw_writel(shadow_freq_cfg1, OMAP4430_CM_SHADOW_FREQ_CONFIG1);
	}

	return omap4_set_freq_update();
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

int omap4_noncore_dpll_mn_bypass(struct clk *clk)
{
	int i, ret = 0;
	u32 reg;
	struct dpll_data *dd;

	if (!clk || !clk->dpll_data)
		return -EINVAL;

	dd = clk->dpll_data;

	if (!(clk->dpll_data->modes & (1 << DPLL_MN_BYPASS)))
		return -EINVAL;

	pr_debug("%s: configuring DPLL %s for MN bypass\n",
			__func__, clk->name);

	/* protect the DPLL during programming; usecount++ */
	clk_enable(dd->clk_bypass);

	omap4_prm_rmw_reg_bits(dd->enable_mask,
			(DPLL_MN_BYPASS << __ffs(dd->enable_mask)),
			dd->control_reg);

	/* wait for DPLL to enter bypass */
	for (i = 0; i < 1000000; i++) {
		reg = __raw_readl(dd->idlest_reg) & dd->mn_bypass_st_mask;
		if (reg)
			break;
	}

	if (reg) {
		if (clk->usecount) {
			/* DPLL is actually needed right now; usecount++ */
			clk_enable(dd->clk_bypass);
			clk_disable(clk->parent);
		}
		pr_err("%s: reparenting %s to %s, and setting old rate %lu to new rate %lu\n",
				__func__, clk->name, dd->clk_bypass->name,
				clk->rate, dd->clk_bypass->rate);
		clk_reparent(clk, dd->clk_bypass);
		clk->rate = dd->clk_bypass->rate;
	} else
		ret = -ENODEV;

	/* done programming, no need to protect DPLL; usecount-- */
	clk_disable(dd->clk_bypass);

	return ret;
}

unsigned long omap4_dpll_regm4xen_recalc(struct clk *clk)
{
	unsigned long rate;
	u32 reg;
	struct dpll_data *dd;

	if (!clk || !clk->dpll_data)
		return -EINVAL;

	dd = clk->dpll_data;

	rate = omap2_get_dpll_rate(clk);

	/* regm4xen adds a multiplier of 4 to DPLL calculations */
	reg = __raw_readl(dd->control_reg);
	if (reg & (DPLL_REGM4XEN_ENABLE << OMAP4430_DPLL_REGM4XEN_SHIFT))
		rate *= OMAP4430_REGM4XEN_MULT;

	return rate;
}

long omap4_dpll_regm4xen_round_rate(struct clk *clk, unsigned long target_rate)
{
	u32 reg;
	struct dpll_data *dd;

	dd = clk->dpll_data;

	/* REGM4XEN add 4x multiplier to MN dividers; check if it is set */
	reg =__raw_readl(dd->control_reg);
	reg &= OMAP4430_DPLL_REGM4XEN_MASK;
	if (reg)
		dd->max_multiplier = OMAP4430_MAX_DPLL_MULT * OMAP4430_REGM4XEN_MULT;
	else
		dd->max_multiplier = OMAP4430_MAX_DPLL_MULT;

	omap2_dpll_round_rate(clk, target_rate);

	if (reg) {
		/*
		 * FIXME this is lazy; we only support values of M that are
		 * divisible by 4 (a safe bet) and for which M/4 is >= 2
		 */
		if (dd->last_rounded_m % OMAP4430_REGM4XEN_MULT)
			pr_warn("%s: %s's M (%u) is not divisible by 4\n",
					__func__, clk->name, dd->last_rounded_m);

		if ((dd->last_rounded_m / OMAP4430_REGM4XEN_MULT) < 2)
			pr_warn("%s: %s's M (%u) is too low.  Try disabling REGM4XEN for this frequency\n",
					__func__, clk->name, dd->last_rounded_m);

		dd->last_rounded_m /= OMAP4430_REGM4XEN_MULT;
	}

	pr_debug("%s: last_rounded_m is %d, last_rounded_n is %d, last_rounded_rate is %lu\n",
			__func__, clk->dpll_data->last_rounded_m,
			clk->dpll_data->last_rounded_n,
			clk->dpll_data->last_rounded_rate);

	return clk->dpll_data->last_rounded_rate;
}

/**
 * omap4_dpll_low_power_cascade - configure system for low power DPLL cascade
 *
 * The low power DPLL cascading scheme is a way to have a mostly functional
 * system running with only one locked DPLL and all of the others in bypass.
 * While this might be useful for many use cases, the primary target is low
 * power audio playback.  The steps to enter this state are roughly:
 *
 * Reparent DPLL_ABE so that it is fed by SYS_32K_CK
 * Set magical REGM4XEN bit so DPLL_ABE MN dividers are multiplied by four
 * Lock DPLL_ABE at 196.608MHz and bypass DPLL_CORE, DPLL_MPU & DPLL_IVA
 * Reparent DPLL_CORE so that is fed by DPLL_ABE
 * Reparent DPLL_MPU & DPLL_IVA so that they are fed by DPLL_CORE
 */
int omap4_dpll_low_power_cascade_enter()
{
	u32 reg;
	int ret = 0;
	struct clk *sys_32k_ck;
	struct clk *dpll_abe_ck, *dpll_abe_m3x2_ck, *abe_dpll_refclk_mux_ck;
	struct clk *dpll_mpu_ck, *div_mpu_hs_clk;
	struct clk *dpll_iva_ck, *div_iva_hs_clk, *iva_hsd_byp_clk_mux_ck;
	struct clk *dpll_core_x2_ck;
	struct clk *dpll_core_m2_ck, *dpll_core_m5x2_ck, *dpll_core_m6x2_ck;
	struct clk *core_hsd_byp_clk_mux_ck, *div_core_ck;
	struct clk *l4_wkup_clk_mux_ck, *lp_clk_div_ck;
	struct clk *pmd_stm_clock_mux_ck, *pmd_trace_clk_mux_ck;
	struct clockdomain *emu_sys_44xx_clkdm;

	sys_32k_ck = clk_get(NULL, "sys_32k_ck");
	dpll_abe_ck = clk_get(NULL, "dpll_abe_ck");
	abe_dpll_refclk_mux_ck = clk_get(NULL, "abe_dpll_refclk_mux_ck");
	dpll_mpu_ck = clk_get(NULL, "dpll_mpu_ck");
	div_mpu_hs_clk = clk_get(NULL, "div_mpu_hs_clk");
	dpll_iva_ck = clk_get(NULL, "dpll_iva_ck");
	div_iva_hs_clk = clk_get(NULL, "div_iva_hs_clk");
	iva_hsd_byp_clk_mux_ck = clk_get(NULL, "iva_hsd_byp_clk_mux_ck");
	dpll_core_m2_ck = clk_get(NULL, "dpll_core_m2_ck");
	dpll_core_m5x2_ck = clk_get(NULL, "dpll_core_m5x2_ck");
	dpll_core_m6x2_ck = clk_get(NULL, "dpll_core_m6x2_ck");
	dpll_abe_m3x2_ck = clk_get(NULL, "dpll_abe_m3x2_ck");
	dpll_core_x2_ck = clk_get(NULL, "dpll_core_x2_ck");
	core_hsd_byp_clk_mux_ck = clk_get(NULL, "core_hsd_byp_clk_mux_ck");
	div_core_ck = clk_get(NULL, "div_core_ck");
	l4_wkup_clk_mux_ck = clk_get(NULL, "l4_wkup_clk_mux_ck");
	lp_clk_div_ck = clk_get(NULL, "lp_clk_div_ck");
	pmd_stm_clock_mux_ck = clk_get(NULL, "pmd_stm_clock_mux_ck");
	pmd_trace_clk_mux_ck = clk_get(NULL, "pmd_trace_clk_mux_ck");

	emu_sys_44xx_clkdm = clkdm_lookup("emu_sys_44xx_clkdm");

	if (!dpll_abe_ck || !dpll_mpu_ck || !div_mpu_hs_clk || !dpll_iva_ck ||
		!div_iva_hs_clk || !iva_hsd_byp_clk_mux_ck || !dpll_core_m2_ck
		|| !dpll_abe_m3x2_ck || !div_core_ck || !dpll_core_x2_ck ||
		!core_hsd_byp_clk_mux_ck || !dpll_core_m5x2_ck ||
		!l4_wkup_clk_mux_ck || !lp_clk_div_ck || !pmd_stm_clock_mux_ck
		|| !pmd_trace_clk_mux_ck || !dpll_core_m6x2_ck
		|| !abe_dpll_refclk_mux_ck || !sys_32k_ck) {
		pr_warn("%s: failed to get all necessary clocks\n", __func__);
		ret = -ENODEV;
		goto out;
	}

	/* enable DPLL_ABE and keep it on; usecount++ */
	clk_enable(dpll_abe_ck);
	omap3_dpll_deny_idle(dpll_abe_ck);

	/* check if DPLL_ABE is driven by 32KHz clock.  If not, reparent it */
	if (abe_dpll_refclk_mux_ck->parent != sys_32k_ck) {
		state.dpll_abe_ck_rate = dpll_abe_ck->rate;
		state.abe_dpll_refclk_mux_ck_parent =
			clk_get_parent(abe_dpll_refclk_mux_ck);

		ret = omap4_noncore_dpll_mn_bypass(dpll_abe_ck);

		if (ret) {
			pr_warn("%s: DPLL_ABE failed to enter MN Bypass\n",
					__func__);
			omap3_dpll_allow_idle(dpll_abe_ck);
			clk_disable(dpll_abe_ck);
			goto out;
		}

		ret = clk_set_parent(abe_dpll_refclk_mux_ck, sys_32k_ck);

		if (ret) {
			pr_warn("%s: failed to reparent DPLL_ABE to SYS_32K\n",
					__func__);
			clk_set_rate(dpll_abe_ck, state.dpll_abe_ck_rate);
			omap3_dpll_allow_idle(dpll_abe_ck);
			clk_disable(dpll_abe_ck);
			goto out;
		}

		/*
		 * Before re-locking DPLL_ABE at 196.608MHz
		 * CM_CLKMODE_DPLL_ABE needs to be configured specifically for
		 * DPLL cascading and for being fed from 32KHz timer.  First
		 * save the inital register contents for later on, then
		 * program the new values all at once.
		 */
		reg = __raw_readl(dpll_abe_ck->dpll_data->control_reg);
		reg &= cm_clkmode_dpll_abe_mask;
		state.cm_clkmode_dpll_abe = reg;

		mdelay(10);

		/*
		 * DPLL_ABE REGM4XEN Enable
		 * DPLL_ABE LP Mode Enable
		 * DPLL_ABE Relock Ramp Enable
		 * DPLL_ABE Ramp Rate
		 * DPLL_ABE Driftguard Enable
		 */
		reg = ((0x1 << OMAP4430_DPLL_REGM4XEN_SHIFT) |
				(0x1 << OMAP4430_DPLL_LPMODE_EN_SHIFT) |
				(0x1 << OMAP4430_DPLL_RELOCK_RAMP_EN_SHIFT) |
				(0x1 << OMAP4430_DPLL_RAMP_RATE_SHIFT) |
				(0x1 << OMAP4430_DPLL_DRIFTGUARD_EN_SHIFT));

		omap4_prm_rmw_reg_bits(cm_clkmode_dpll_abe_mask, reg,
				dpll_abe_ck->dpll_data->control_reg);

		mdelay(10);

		/*
		 * On OMAP4 the DPLL_n_X2 clocks are not twice the speed of
		 * the DPLL.  Instead they reflect the actual output of the
		 * DPLL and the non-X2 clocks are half of that output.  It
		 * would be preferable to set the rate of dpll_abe_x2_ck but
		 * that clock doesn't have any clock ops.  Program dpll_abe_ck
		 * for half of the desired rate instead.
		 */
		ret = clk_set_rate(dpll_abe_ck, 196608000 / 2);
		if (ret) {
			pr_warn("%s: failed to lock DPLL_ABE\n", __func__);
			omap4_prm_rmw_reg_bits(cm_clkmode_dpll_abe_mask,
					state.cm_clkmode_dpll_abe,
					dpll_abe_ck->dpll_data->control_reg);
			clk_set_parent(abe_dpll_refclk_mux_ck,
					state.abe_dpll_refclk_mux_ck_parent);
			clk_set_rate(dpll_abe_ck, state.dpll_abe_ck_rate);
			omap3_dpll_allow_idle(dpll_abe_ck);
			clk_disable(dpll_abe_ck);
			goto out;
		}
	} else
		/* paranoia */
		state.abe_dpll_refclk_mux_ck_parent = NULL;

	/* divide MPU/IVA bypass clocks by 2 (for when we bypass DPLL_CORE) */
	state.div_mpu_hs_clk_rate = div_mpu_hs_clk->rate;
	state.div_iva_hs_clk_rate = div_iva_hs_clk->rate;
	clk_set_rate(div_mpu_hs_clk, div_mpu_hs_clk->parent->rate / 2);
	clk_set_rate(div_iva_hs_clk, div_iva_hs_clk->parent->rate / 2);

	/* prevent DPLL_MPU & DPLL_IVA from idling */
	omap3_dpll_deny_idle(dpll_mpu_ck);
	omap3_dpll_deny_idle(dpll_iva_ck);

	/* select CLKINPULOW (div_iva_hs_clk) as DPLL_IVA bypass clock */
	state.iva_hsd_byp_clk_mux_ck_parent = iva_hsd_byp_clk_mux_ck->parent;
	ret = clk_set_parent(iva_hsd_byp_clk_mux_ck, div_iva_hs_clk);
	if (ret) {
		pr_debug("%s: failed reparenting DPLL_IVA bypass clock to CLKINPULOW\n",
				__func__);
		goto iva_hsd_byp_clk_mux_ck_parent;
	} else
		pr_debug("%s: reparented DPLL_IVA bypass clock to CLKINPULOW\n",
				__func__);

	/* bypass DPLL_MPU */
	state.dpll_mpu_ck_rate = dpll_mpu_ck->rate;
	ret = omap3_noncore_dpll_set_rate(dpll_mpu_ck,
			dpll_mpu_ck->dpll_data->clk_bypass->rate);
	if (ret) {
		pr_debug("%s: DPLL_MPU failed to enter Low Power bypass\n",
				__func__);
		goto dpll_mpu_bypass_fail;
	} else 
		pr_debug("%s: DPLL_MPU entered Low Power bypass\n",__func__);

	/* bypass DPLL_IVA */
	state.dpll_iva_ck_rate = dpll_iva_ck->rate;
	ret = omap3_noncore_dpll_set_rate(dpll_iva_ck,
			dpll_iva_ck->dpll_data->clk_bypass->rate);
	if (ret) {
		pr_debug("%s: DPLL_IVA failed to enter Low Power bypass\n",
				__func__);
		goto dpll_iva_bypass_fail;
	} else 
		pr_debug("%s: DPLL_IVA entered Low Power bypass\n",__func__);

	/* drive DPLL_CORE bypass clock from DPLL_ABE (CLKINPULOW) */
	state.core_hsd_byp_clk_mux_ck_parent = core_hsd_byp_clk_mux_ck->parent;
	ret = clk_set_parent(core_hsd_byp_clk_mux_ck, dpll_abe_m3x2_ck);
	if (ret) {
		pr_debug("%s: failed reparenting DPLL_CORE bypass clock to ABE_M3X2\n",
				__func__);
		goto core_bypass_clock_reparent_fail;
	} else
		pr_debug("%s: DPLL_CORE bypass clock reparented to ABE_M3X2\n",
				__func__);

	/*
	 * bypass DPLL_CORE, configure EMIF for the new rate
	 * CORE_CLK = CORE_X2_CLK
	 */
	state.div_core_ck_rate = div_core_ck->rate;
	state.dpll_core_m2_ck_rate = dpll_core_m2_ck->rate;
	state.dpll_core_m5x2_ck_rate = dpll_core_m5x2_ck->rate;

	ret =  clk_set_rate(div_core_ck, dpll_core_m5x2_ck->rate);
	ret |= clk_set_rate(dpll_core_m2_ck, 196608000);
	ret |= clk_set_rate(dpll_core_m5x2_ck, dpll_core_x2_ck->rate);
	if (ret) {
		pr_debug("%s: failed setting CORE clock rates\n", __func__);
		goto core_clock_set_rate_fail;
	} else
		pr_debug("%s: DPLL_CORE bypass clock reparented to ABE_M3X2\n",
				__func__);

	/* DPLLs are configured, so let DPLL_ABE idle again */
	omap3_dpll_allow_idle(dpll_abe_ck);

	/*
	 * use ABE_LP_CLK to drive L4WKUP_ICLK and use 32K_FCLK to drive
	 * ABE_DPLL_BYPASS_CLK
	 */
	state.l4_wkup_clk_mux_ck_parent = l4_wkup_clk_mux_ck->parent;
	ret = clk_set_parent(l4_wkup_clk_mux_ck, lp_clk_div_ck);
	if (ret)
		pr_debug("%s: failed reparenting L4WKUP_ICLK to ABE LP clock\n",
				__func__);
	else
		pr_debug("%s: reparented L4WKUP_ICLK to ABE LP clock\n",
				__func__);

	/* never de-assert CLKREQ while in DPLL cascading scheme */
	state.clkreqctrl = __raw_readl(OMAP4430_PRM_CLKREQCTRL);
	__raw_writel(0x0, OMAP4430_PRM_CLKREQCTRL);

	/* drive PM debug clocks from CORE_M6X2 and allow the clkdm to idle */
	state.pmd_stm_clock_mux_ck_parent = pmd_stm_clock_mux_ck->parent;
	state.pmd_trace_clk_mux_ck_parent = pmd_trace_clk_mux_ck->parent;
	ret =  clk_set_parent(pmd_stm_clock_mux_ck, dpll_core_m6x2_ck);
	ret |= clk_set_parent(pmd_trace_clk_mux_ck, dpll_core_m6x2_ck);
	if (ret)
		pr_debug("%s: failed reparenting PMD clocks to ABE LP clock\n",
				__func__);
	else
		pr_debug("%s: reparented PMD clocks to ABE LP clock\n",
				__func__);

	omap2_clkdm_allow_idle(emu_sys_44xx_clkdm);

	recalculate_root_clocks();

	goto out;

core_clock_set_rate_fail:
	clk_set_rate(dpll_core_m5x2_ck, state.dpll_core_m5x2_ck_rate);
	clk_set_rate(dpll_core_m2_ck, state.dpll_core_m2_ck_rate);
	clk_set_rate(div_core_ck, state.div_core_ck_rate);
core_bypass_clock_reparent_fail:
	clk_set_parent(iva_hsd_byp_clk_mux_ck,
			state.iva_hsd_byp_clk_mux_ck_parent);
dpll_iva_bypass_fail:
	clk_set_rate(div_iva_hs_clk, state.div_iva_hs_clk_rate);
	clk_set_rate(dpll_iva_ck, state.dpll_iva_ck_rate);
dpll_mpu_bypass_fail:
	clk_set_rate(div_mpu_hs_clk, state.div_mpu_hs_clk_rate);
	clk_set_rate(dpll_mpu_ck, state.dpll_mpu_ck_rate);
iva_hsd_byp_clk_mux_ck_parent:
	clk_set_parent(iva_hsd_byp_clk_mux_ck,
			state.iva_hsd_byp_clk_mux_ck_parent);
	omap3_dpll_allow_idle(dpll_iva_ck);
	omap3_dpll_allow_idle(dpll_mpu_ck);
out:
	return ret;
}

int omap4_dpll_low_power_cascade_exit()
{
	int ret = 0;
	struct clk *sys_clkin_ck, *sys_32k_ck;
	struct clk *dpll_abe_ck, *dpll_abe_m3x2_ck, *abe_dpll_refclk_mux_ck;
	struct clk *dpll_mpu_ck, *div_mpu_hs_clk;
	struct clk *dpll_iva_ck, *div_iva_hs_clk, *iva_hsd_byp_clk_mux_ck;
	struct clk *dpll_core_x2_ck;
	struct clk *dpll_core_m2_ck, *dpll_core_m5x2_ck, *dpll_core_m6x2_ck;
	struct clk *core_hsd_byp_clk_mux_ck, *div_core_ck;
	struct clk *l4_wkup_clk_mux_ck, *lp_clk_div_ck;
	struct clk *pmd_stm_clock_mux_ck, *pmd_trace_clk_mux_ck;
	struct clockdomain *emu_sys_44xx_clkdm;

	sys_clkin_ck = clk_get(NULL, "sys_clkin_ck");
	sys_32k_ck = clk_get(NULL, "sys_32k_ck");
	abe_dpll_refclk_mux_ck = clk_get(NULL, "abe_dpll_refclk_mux_ck");
	dpll_abe_ck = clk_get(NULL, "dpll_abe_ck");
	dpll_mpu_ck = clk_get(NULL, "dpll_mpu_ck");
	div_mpu_hs_clk = clk_get(NULL, "div_mpu_hs_clk");
	dpll_iva_ck = clk_get(NULL, "dpll_iva_ck");
	div_iva_hs_clk = clk_get(NULL, "div_iva_hs_clk");
	iva_hsd_byp_clk_mux_ck = clk_get(NULL, "iva_hsd_byp_clk_mux_ck");
	dpll_core_m2_ck = clk_get(NULL, "dpll_core_m2_ck");
	dpll_core_m5x2_ck = clk_get(NULL, "dpll_core_m5x2_ck");
	dpll_core_m6x2_ck = clk_get(NULL, "dpll_core_m6x2_ck");
	dpll_abe_m3x2_ck = clk_get(NULL, "dpll_abe_m3x2_ck");
	dpll_core_x2_ck = clk_get(NULL, "dpll_core_x2_ck");
	core_hsd_byp_clk_mux_ck = clk_get(NULL, "core_hsd_byp_clk_mux_ck");
	div_core_ck = clk_get(NULL, "div_core_ck");
	l4_wkup_clk_mux_ck = clk_get(NULL, "l4_wkup_clk_mux_ck");
	lp_clk_div_ck = clk_get(NULL, "lp_clk_div_ck");
	pmd_stm_clock_mux_ck = clk_get(NULL, "pmd_stm_clock_mux_ck");
	pmd_trace_clk_mux_ck = clk_get(NULL, "pmd_trace_clk_mux_ck");

	emu_sys_44xx_clkdm = clkdm_lookup("emu_sys_44xx_clkdm");

	if (!dpll_abe_ck || !dpll_mpu_ck || !div_mpu_hs_clk || !dpll_iva_ck ||
		!div_iva_hs_clk || !iva_hsd_byp_clk_mux_ck || !dpll_core_m2_ck
		|| !dpll_abe_m3x2_ck || !div_core_ck || !dpll_core_x2_ck ||
		!core_hsd_byp_clk_mux_ck || !dpll_core_m5x2_ck ||
		!l4_wkup_clk_mux_ck || !lp_clk_div_ck || !pmd_stm_clock_mux_ck
		|| !pmd_trace_clk_mux_ck || !dpll_core_m6x2_ck
		|| !sys_clkin_ck || !sys_32k_ck || !abe_dpll_refclk_mux_ck) {
		pr_warn("%s: failed to get all necessary clocks\n", __func__);
		ret = -ENODEV;
		goto out;
	}

	/* lock DPLL_MPU */
	ret = omap3_noncore_dpll_set_rate(dpll_mpu_ck, state.dpll_mpu_ck_rate);
	if (ret)
		pr_err("%s: DPLL_MPU failed to relock\n", __func__);

	/* lock DPLL_IVA */
	ret = omap3_noncore_dpll_set_rate(dpll_iva_ck, state.dpll_iva_ck_rate);
	if (ret)
		pr_err("%s: DPLL_IVA failed to relock\n", __func__);

	/* restore bypass clock rates */
	clk_set_rate(div_mpu_hs_clk, state.div_mpu_hs_clk_rate);
	clk_set_rate(div_iva_hs_clk, state.div_iva_hs_clk_rate);

	/* allow DPLL_MPU & DPLL_IVA to idle */
	omap3_dpll_allow_idle(dpll_mpu_ck);
	omap3_dpll_allow_idle(dpll_iva_ck);

	/* restore DPLL_IVA bypass clock */
	ret = clk_set_parent(iva_hsd_byp_clk_mux_ck,
			state.iva_hsd_byp_clk_mux_ck_parent);
	if (ret)
		pr_err("%s: failed to restore DPLL_IVA bypass clock\n",
				__func__);

	/* restore CORE clock rates */
	ret =  clk_set_rate(dpll_core_m5x2_ck, state.dpll_core_m5x2_ck_rate);
	ret |= clk_set_rate(dpll_core_m2_ck, state.dpll_core_m2_ck_rate);
	ret |= clk_set_rate(div_core_ck, state.div_core_ck_rate);
	if (ret)
		pr_debug("%s: failed to restore CORE clock rates\n", __func__);

	/* drive DPLL_CORE bypass clock from DPLL_ABE (CLKINPULOW) */
	ret = clk_set_parent(core_hsd_byp_clk_mux_ck,
			state.core_hsd_byp_clk_mux_ck_parent);
	if (ret)
		pr_debug("%s: failed restoring DPLL_CORE bypass clock parent\n",
				__func__);

	/* reconfigure DPLL_ABE if needed */
	if (state.abe_dpll_refclk_mux_ck_parent) {
		/* reprogram CM_CLKMODE_DPLL_ABE */
		omap4_prm_rmw_reg_bits(cm_clkmode_dpll_abe_mask,
				state.cm_clkmode_dpll_abe,
				dpll_abe_ck->dpll_data->control_reg);

		/* re-parent DPLL_ABE refclk */
		clk_set_parent(abe_dpll_refclk_mux_ck,
				state.abe_dpll_refclk_mux_ck_parent);

		/* relock DPLL_ABE */
		clk_set_rate(dpll_abe_ck, state.dpll_abe_ck_rate);

		omap3_dpll_allow_idle(dpll_abe_ck);
		clk_disable(dpll_abe_ck);
	}

	/*
	 * DPLLs are configured, so let SYSCK idle again
	 */

	/* restore parent to drive L4WKUP_ICLK and ABE_DPLL_BYPASS_CLK */
	clk_set_parent(l4_wkup_clk_mux_ck, state.l4_wkup_clk_mux_ck_parent);
	if (ret)
		pr_debug("%s: failed restoring L4WKUP_ICLK parent clock\n",
				__func__);

	/* restore CLKREQ behavior */
	__raw_writel(state.clkreqctrl, OMAP4430_PRM_CLKREQCTRL);

	/* drive PM debug clocks from CORE_M6X2 and allow the clkdm to idle */
	ret =  clk_set_parent(pmd_stm_clock_mux_ck,
			state.pmd_stm_clock_mux_ck_parent);
	ret |= clk_set_parent(pmd_trace_clk_mux_ck,
			state.pmd_trace_clk_mux_ck_parent);
	if (ret)
		pr_debug("%s: failed restoring parent to PMD clocks\n",
				__func__);

	recalculate_root_clocks();

out:
	return ret;
}
