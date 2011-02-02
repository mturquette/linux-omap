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
#include <plat/omap_device.h>

#include <mach/emif.h>
#include <mach/omap4-common.h>

#include "clock.h"
#include "clock44xx.h"
#include "cm.h"
#include "cm-regbits-44xx.h"
#include "prm-regbits-44xx.h"

#define MAX_FREQ_UPDATE_TIMEOUT  100000
#define DPLL_REGM4XEN_ENABLE	0x1

bool omap4_lpmode = false;

u32 cm_clkmode_dpll_abe_mask =
		(OMAP4430_DPLL_REGM4XEN_MASK |
		 OMAP4430_DPLL_LPMODE_EN_MASK |
		 OMAP4430_DPLL_RELOCK_RAMP_EN_MASK |
		 OMAP4430_DPLL_RAMP_RATE_MASK |
		 OMAP4430_DPLL_DRIFTGUARD_EN_MASK);

static struct clockdomain *l3_emif_clkdm;
static struct clk *dpll_core_m2_ck;
static struct clk *emif1_fck, *emif2_fck;

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
	int i = 0;
	u32 validrate = 0, shadow_freq_cfg1 = 0, new_div = 0;
	struct clk *dpll_core_ck;
	struct dpll_data *dd;

	/* DPLL_CORE is parent of CORE_M2 */
	dpll_core_ck = clk_get(NULL, "dpll_core_ck");
	dd = dpll_core_ck->dpll_data;

	//pr_err("%s: here0\n", __func__);

	if (!clk || !rate)
		return -EINVAL;

	if (!dpll_core_ck || !dd)
		return -ENODEV;

	/* Just to avoid look-up on every call to speed up */
	if (!l3_emif_clkdm)
		l3_emif_clkdm = clkdm_lookup("l3_emif_clkdm");

	/* CM_MEMIF_CLKSTCTRL */
	/* Configures MEMIF domain in SW_WKUP */
	omap2_clkdm_wakeup(l3_emif_clkdm);

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
		shadow_freq_cfg1 =
			(0x2 << OMAP4430_DPLL_CORE_M2_DIV_SHIFT) |
			(DPLL_LOW_POWER_BYPASS <<
			 OMAP4430_DPLL_CORE_DPLL_EN_SHIFT) |
			(1 << OMAP4430_DLL_RESET_SHIFT);
		__raw_writel(shadow_freq_cfg1, OMAP4430_CM_SHADOW_FREQ_CONFIG1);
		mdelay(10);

		shadow_freq_cfg1 = __raw_readl(OMAP4430_CM_SHADOW_FREQ_CONFIG1);
		shadow_freq_cfg1 |= (1 << OMAP4430_FREQ_UPDATE_SHIFT);
		__raw_writel(shadow_freq_cfg1, OMAP4430_CM_SHADOW_FREQ_CONFIG1);
	} else {
		/* check for valid rate to lock DPLL_CORE */
#if 1
		/*if (omap4_lpmode) {
			validrate = 800000000;
			new_div = 1;
		} else {*/
			validrate = omap2_clksel_round_rate_div(clk, rate, &new_div);
			if (validrate != rate)
				return -EINVAL;
		//}
#else
		if (rate == 800000000)
			new_div = 2;
		else if (rate == 400000000)
			new_div = 1;
		else
			pr_err("%s: shouldn't be here!  rate is %lu\n",
					__func__, rate);
#endif

		/*
		 * DDR clock = DPLL_CORE_M2_CK / 2.  Program EMIF timing
		 * parameters in EMIF shadow registers for validrate divided
		 * by 2.
		 */
		omap_emif_setup_registers(rate / 2, LPDDR2_VOLTAGE_STABLE);

		//pr_err("%s: here1\n", __func__);

		/*
		 * program DPLL_CORE_M2_DIV with same value as the one already
		 * in direct register and lock DPLL_CORE
		 */
		shadow_freq_cfg1 =
			(new_div << OMAP4430_DPLL_CORE_M2_DIV_SHIFT) |
			(DPLL_LOCKED << OMAP4430_DPLL_CORE_DPLL_EN_SHIFT) |
			(1 << OMAP4430_DLL_RESET_SHIFT) |
			(1 << OMAP4430_FREQ_UPDATE_SHIFT);
		__raw_writel(shadow_freq_cfg1, OMAP4430_CM_SHADOW_FREQ_CONFIG1);
	}

	//pr_err("%s: here2\n", __func__);

	/* wait for the configuration to be applied */
	omap_test_timeout(((__raw_readl(OMAP4430_CM_SHADOW_FREQ_CONFIG1)
					& OMAP4430_FREQ_UPDATE_MASK) == 0),
			MAX_FREQ_UPDATE_TIMEOUT, i);

	/* CM_MEMIF_CLKSTCTRL */
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
 * omap4_core_dpll_set_rate - set the rate for the CORE DPLL
 * @clk: struct clk * of hte DPLL to set
 * @rate: rounded target rate
 *
 * Program the CORE DPLL, including handling of EMIF frequency changes on M2
 * divider.  Returns 0 on success, otherwise a negative error code.
 */
int omap4_core_dpll_set_rate(struct clk *clk, unsigned long rate)
{
	int i = 0, m2_div;
	u32 mask, reg;
	u32 shadow_freq_cfg1 = 0, new_div = 0;
	struct clk *dpll_core_ck, *new_parent;
	struct dpll_data *dd;

	pr_err("%s: here0\n", __func__);

	if (!clk  || !rate)
		return -EINVAL;

	dd = dpll_core_ck->dpll_data;
	if (!dd)
		return -EINVAL;

	if (rate == clk_get_rate(clk))
		return 0;

	/* enable reference and bypass clocks */
	clk_enable(dd->clk_bypass);
	clk_enable(dd->clk_ref);

	/* Just to avoid look-up on every call to speed up */
	if (!l3_emif_clkdm)
		l3_emif_clkdm = clkdm_lookup("l3_emif_clkdm");
	if (!emif1_fck)
		emif1_fck = clk_get(NULL, "emif1_fck");
	if (!emif2_fck)
		emif2_fck = clk_get(NULL, "emif2_fck");
	if (!dpll_core_m2_ck)
		dpll_core_m2_ck = clk_get(NULL, "dpll_core_m2_ck");

	/* CM_MEMIF_CLKSTCTRL */
	/* Make sure MEMIF clkdm is in SW_WKUP and EMIF modules are func */
	omap2_clkdm_wakeup(l3_emif_clkdm);
	clk_enable(emif1_fck);
	clk_enable(emif2_fck);

	/* check for bypass rate */
	if (rate == dd->clk_bypass->rate &&
			clk->dpll_data->modes & (1 << DPLL_LOW_POWER_BYPASS)) {
		pr_err("%s: here1\n", __func__);
		/*
		 * DDR clock = DPLL_CORE_M2_CK / 2.  Program EMIF timing
		 * parameters in EMIF shadow registers for bypass clock rate
		 * divided by 2
		 */
		omap_emif_setup_registers(rate / 2, LPDDR2_VOLTAGE_STABLE);
		pr_err("%s: here2\n", __func__);

		/*
		 * FIXME PRCM functional spec says we should program
		 * CM_SHADOW_FREQ_CONFIG2.CLKSEL_L3 to 0 (corresponds to
		 * CM_CLKSEL_CORE.CLKSEL_L3) for normal bypass operation.
		 * This means L3_CLK is CORE_CLK divided by 1.  Same spec says
		 * the value should be 1 when entering DPLL cascading.  All of
		 * this assumes GPMC can scale frequency on the fly.  Too many
		 * unknowns, skipping this for now...
		 */

		/*
		 * program CM_DIV_M2_DPLL_CORE.DPLL_CLKOUT_DIV for divide by
		 * two and put DPLL_CORE into LP Bypass
		 */
		m2_div = omap4_prm_read_bits_shift(dpll_core_m2_ck->clksel_reg,
				dpll_core_m2_ck->clksel_mask);

		shadow_freq_cfg1 =
			(m2_div << OMAP4430_DPLL_CORE_M2_DIV_SHIFT) |
			(DPLL_LOW_POWER_BYPASS <<
			 OMAP4430_DPLL_CORE_DPLL_EN_SHIFT) |
			(1 << OMAP4430_DLL_RESET_SHIFT) |
			(1 << OMAP4430_FREQ_UPDATE_SHIFT);
		__raw_writel(shadow_freq_cfg1, OMAP4430_CM_SHADOW_FREQ_CONFIG1);

		pr_err("%s: here3\n", __func__);
		new_parent = dd->clk_bypass;
	} else {
		pr_err("%s WRONG\n", __func__);
		if (dd->last_rounded_rate != rate)
			rate = clk->round_rate(clk, rate);

		if (dd->last_rounded_rate == 0) {
			pr_err("%s: WHAT THE FUCK, MAN?\n", __func__);
			return -EINVAL;
		}

		/*
		 * DDR clock = DPLL_CORE_M2_CK / 2.  Program EMIF timing
		 * parameters in EMIF shadow registers for validrate divided
		 * by 2.
		 */
		omap_emif_setup_registers(rate / 2, LPDDR2_VOLTAGE_STABLE);

		//pr_err("%s: here1\n", __func__);

		/*
		 * FIXME skipping  bypass part of omap3_noncore_dpll_program.
		 * also x-loader's configure_core_dpll_no_lock bypasses
		 * DPLL_CORE directly through CM_CLKMODE_DPLL_CORE via MN
		 * bypass; no shadow register necessary!
		 */

		pr_err("%s: locking DPLL_CORE.  m is %d, n is %d\n", __func__,
				dd->last_rounded_m, dd->last_rounded_n);

		mask = (dd->mult_mask | dd->div1_mask);
		reg  = (dd->last_rounded_m << __ffs(dd->mult_mask)) |
			((dd->last_rounded_n - 1) << __ffs(dd->div1_mask));

		/* program mn divider values */
		omap4_prm_rmw_reg_bits(mask, reg, dd->mult_div1_reg);

		/*
		 * FIXME PRCM functional spec says we should program
		 * CM_SHADOW_FREQ_CONFIG2.CLKSEL_L3 to 1 (corresponds to
		 * CM_CLKSEL_CORE.CLKSEL_L3) for normal bypass operation.
		 * This means L3_CLK is CORE_CLK divided by 2.  Same spec says
		 * the value should be 0 when exiting DPLL cascading.  All of
		 * this assumes GPMC can scale frequency on the fly.  Too many
		 * unknowns, skipping this for now...
		 */

		/*
		 * program DPLL_CORE_M2_DIV with same value as the one already
		 * in direct register and lock DPLL_CORE
		 */
		m2_div = omap4_prm_read_bits_shift(dpll_core_m2_ck->clksel_reg,
				dpll_core_m2_ck->clksel_mask);

		shadow_freq_cfg1 =
			(m2_div << OMAP4430_DPLL_CORE_M2_DIV_SHIFT) |
			(DPLL_LOCKED << OMAP4430_DPLL_CORE_DPLL_EN_SHIFT) |
			(1 << OMAP4430_DLL_RESET_SHIFT) |
			(1 << OMAP4430_FREQ_UPDATE_SHIFT);
		__raw_writel(shadow_freq_cfg1, OMAP4430_CM_SHADOW_FREQ_CONFIG1);

		new_parent = dd->clk_ref;
	}

	pr_err("%s: here2\n", __func__);

	/* wait for the configuration to be applied */
	omap_test_timeout(((__raw_readl(OMAP4430_CM_SHADOW_FREQ_CONFIG1)
					& OMAP4430_FREQ_UPDATE_MASK) == 0),
			MAX_FREQ_UPDATE_TIMEOUT, i);
	/*
	 * Switch the parent clock in the heirarchy, and make sure that the
	 * new parent's usecount is correct.  Note: we enable the new parent
	 * before disabling the old to avoid any unnecessary hardware
	 * disable->enable transitions.
	 */
	if (clk->usecount) {
		omap2_clk_enable(new_parent);
		omap2_clk_disable(clk->parent);
	}
	clk_reparent(clk, new_parent);
	clk->rate = rate;

	/* CM_MEMIF_CLKSTCTRL */
	/* Configures MEMIF domain back to HW_WKUP */
	omap2_clkdm_allow_idle(l3_emif_clkdm);
	clk_disable(emif1_fck);
	clk_disable(emif2_fck);

	/*
	 * FIXME PRCM functional spec says we should set GPMC_FREQ_UPDATE bit
	 * here, but we're not even handling CM_SHADOW_FREQ_CONFIG2 at all.
	 */

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
	struct clk *dpll_core_ck, *dpll_core_x2_ck;
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
	dpll_core_ck = clk_get(NULL, "dpll_core_ck");
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
		|| !abe_dpll_refclk_mux_ck || !sys_32k_ck || !dpll_core_ck) {
		pr_warn("%s: failed to get all necessary clocks\n", __func__);
		ret = -ENODEV;
		goto out;
	}

	omap4_lpmode = true;

	/* enable DPLL_ABE and keep it on; usecount++ */
	clk_enable(dpll_abe_ck);
	omap3_dpll_deny_idle(dpll_abe_ck);

#if 0
	/* check if DPLL_ABE is driven by 32KHz clock.  If not, reparent it */
	if (clk_get_parent(abe_dpll_refclk_mux_ck) != sys_32k_ck) {
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
#endif

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
	pr_err("%s: ret is %d, CM_CLKSEL_DPLL_CORE is 0x%x\n", __func__, ret,
			__raw_readl(core_hsd_byp_clk_mux_ck->clksel_reg));
	if (ret) {
		pr_debug("%s: failed reparenting DPLL_CORE bypass clock to ABE_M3X2\n",
				__func__);
		goto core_bypass_clock_reparent_fail;
	} else
		pr_debug("%s: DPLL_CORE bypass clock reparented to ABE_M3X2\n",
				__func__);

	/*
	 * bypass PER here.  Be sure parent is DPLL_CORE.
	 * Also make sure that UARTs behave correctly.  Seems like UART1/2/3/4
	 * all seem to be enabled during DPLL cascading...
	 */

	/*
	 * bypass DPLL_CORE, configure EMIF for the new rate
	 * CORE_CLK = CORE_X2_CLK
	 */
	state.div_core_ck_rate = div_core_ck->rate;
	state.dpll_core_m2_ck_rate = dpll_core_m2_ck->rate;
	state.dpll_core_m5x2_ck_rate = dpll_core_m5x2_ck->rate;

	ret =  clk_set_rate(div_core_ck, (dpll_core_m5x2_ck->rate / 2));
	pr_err("%s: ret is %d, CM_CLKSEL_CORE is 0x%x\n", __func__, ret,
			__raw_readl(div_core_ck->clksel_reg));

#if 0
	ret = clk_set_rate(dpll_core_m2_ck, 196608000);
	pr_err("%s: ret is %d, CM_DIV_M2_DPLL_CORE is 0x%x\n", __func__, ret,
			__raw_readl(dpll_core_m2_ck->clksel_reg));
#else
	ret = clk_set_rate(dpll_core_ck, 196608000);
	pr_err("%s: ret is %d, CM_DIV_M2_DPLL_CORE is 0x%x\n", __func__, ret,
			__raw_readl(dpll_core_m2_ck->clksel_reg));
#endif

	/* at this point MPU and IVA should not be bypassed... */
	ret = clk_set_rate(dpll_core_m5x2_ck, dpll_core_x2_ck->rate);
	pr_err("%s: ret is %d, CM_DIV_M5_DPLL_CORE is 0x%x\n", __func__, ret,
			__raw_readl(dpll_core_m5x2_ck->clksel_reg));
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

	/* modify OPP tables for OPP_LP entries only */

	/*
	 * calling the following would be perfect for getting the clock tree
	 * in shape.  Just need to make sure that they result in DPLL's
	 * getting relocked!
	 *
	 * omap_device_get_rate(mpu)
	 * omap_device_get_rate(l3)
	 * omap_device_get_rate(aess)
	 * omap_device_get_rate(iva)
	 *
	 * omap_device_set_rate(mpu, 98MHz)
	 * omap_device_set_rate(l3, 98MHz)
	 * omap_device_set_rate(aess, 98MHz)
	 * omap_device_set_rate(iva, 98MHz)
	 */
	static struct device dummy_lp_dev;
	struct device *dev;

	dev = omap2_get_mpuss_device();
	if (!dev)
		pr_err("%s: didn't get mpuss\n", __func__);
	pr_err("%s: mpuss device rate is %lu\n", __func__, omap_device_get_rate(dev));
	//omap_device_set_rate(&dummy_lp_dev, dev, 98304000);

	dev = omap2_get_iva_device();
	if (!dev)
		pr_err("%s: didn't get iva\n", __func__);
	pr_err("%s: iva device rate is %lu\n", __func__, omap_device_get_rate(dev));
	//omap_device_set_rate(&dummy_lp_dev, dev, 98304000);

	dev = omap4_get_dsp_device();
	if (!dev)
		pr_err("%s: didn't get dsp\n", __func__);
	pr_err("%s: dsp device rate is %lu\n", __func__, omap_device_get_rate(dev));
	//omap_device_set_rate(&dummy_lp_dev, dev, 98304000);

	dev = omap2_get_l3_device();
	if (!dev)
		pr_err("%s: didn't get dsp\n", __func__);
	pr_err("%s: l3 device rate is %lu\n", __func__, omap_device_get_rate(dev));
	//omap_device_set_rate(&dummy_lp_dev, dev, 98304000);

	//omap4_lpmode = true;

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
	struct clk *dpll_core_ck;
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
	dpll_core_ck = clk_get(NULL, "dpll_core_ck");
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
		|| !sys_clkin_ck || !sys_32k_ck || !abe_dpll_refclk_mux_ck
		|| !dpll_core_ck) {
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

	mdelay(10);
	pr_err("%s: here0\n", __func__);
	/* restore bypass clock rates */
	clk_set_rate(div_mpu_hs_clk, state.div_mpu_hs_clk_rate);
	clk_set_rate(div_iva_hs_clk, state.div_iva_hs_clk_rate);

	/* restore DPLL_IVA bypass clock */
	ret = clk_set_parent(iva_hsd_byp_clk_mux_ck,
			state.iva_hsd_byp_clk_mux_ck_parent);
	if (ret)
		pr_err("%s: failed to restore DPLL_IVA bypass clock\n",
				__func__);

	/* Just to avoid look-up on every call to speed up */
	if (!l3_emif_clkdm)
		l3_emif_clkdm = clkdm_lookup("l3_emif_clkdm");

	/* CM_MEMIF_CLKSTCTRL */
	/* Configures MEMIF domain in SW_WKUP */
	omap2_clkdm_wakeup(l3_emif_clkdm);

	/* drive DPLL_CORE bypass clock from DPLL_ABE (CLKINPULOW) */
	ret = clk_set_parent(core_hsd_byp_clk_mux_ck, dpll_abe_m3x2_ck);
	pr_err("%s: ret is %d, CM_CLKSEL_DPLL_CORE is 0x%x\n", __func__, ret,
			__raw_readl(core_hsd_byp_clk_mux_ck->clksel_reg));
	if (ret)
		pr_debug("%s: failed restoring DPLL_CORE bypass clock parent\n",
				__func__);

	/*mdelay(10);
	pr_err("%s: here1, rate is %lu\n", __func__,
			state.dpll_core_m2_ck_rate);*/

	/* restore CORE clock rates */
	ret = clk_set_rate(div_core_ck, (dpll_core_m5x2_ck->rate / 2));
	pr_err("%s: ret is %d, CM_CLKSEL_CORE is 0x%x\n", __func__, ret,
			__raw_readl(div_core_ck->clksel_reg));

	/*mdelay(10);
	pr_err("%s: here2 ret is %d, rate is %lu\n", __func__, ret, state.dpll_core_m5x2_ck_rate);*/

	ret =  clk_set_rate(dpll_core_m5x2_ck, dpll_core_x2_ck->rate);
	pr_err("%s: ret is %d, CM_DIV_M5_DPLL_CORE is 0x%x\n", __func__, ret,
			__raw_readl(dpll_core_m5x2_ck->clksel_reg));

	/*mdelay(10);
	pr_err("%s: here3 ret is %d\n", __func__, ret);*/

	/* do the weird non-shadow register programming here, a la x-loader */

#if 0
	ret = clk_set_rate(dpll_core_m2_ck, 400000000);
	pr_err("%s: ret is %d, CM_DIV_M2_DPLL_CORE is 0x%x\n", __func__, ret,
			__raw_readl(dpll_core_m2_ck->clksel_reg));
#else
	ret = clk_set_rate(dpll_core_ck, 400000000);
	pr_err("%s: ret is %d, CM_DIV_M2_DPLL_CORE is 0x%x\n", __func__, ret,
			__raw_readl(dpll_core_m2_ck->clksel_reg));
#endif

	/*mdelay(10);
	pr_err("%s: here4 ret is %d\n", __func__, ret);*/

	if (ret)
		pr_err("%s: failed to restore CORE clock rates\n", __func__);

	/* CM_MEMIF_CLKSTCTRL */
	/* Configures MEMIF domain back to HW_WKUP */
	omap2_clkdm_allow_idle(l3_emif_clkdm);

	/* allow DPLL_MPU & DPLL_IVA to idle */
	omap3_dpll_allow_idle(dpll_mpu_ck);
	omap3_dpll_allow_idle(dpll_iva_ck);

	mdelay(10);
	pr_err("%s: here5\n", __func__);
	/* reconfigure DPLL_ABE if needed */
	if (state.abe_dpll_refclk_mux_ck_parent) {
		pr_err("%s: reconfiguring DPLL_ABE\n", __func__);
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
