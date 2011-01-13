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
#define DPLL_ABE_CLKSEL_SYS_32K	0x1
#define DPLL_REGM4XEN_ENABLE	0x1
#define DPLL_REGM4XEN_MULT	0x4

static struct clockdomain *l3_emif_clkdm;

static struct dpll_cascade_saved_state {
	struct clk *dpll_abe_parent;
	u32 dpll_abe_rate;
	struct clk *dpll_core_parent;
	u32 dpll_core_rate;
	struct clk *dpll_mpu_parent;
	u32 dpll_mpu_rate;
	struct clk *dpll_iva_parent;
	u32 dpll_iva_rate;
	struct clk *abe_dpll_refclk_mux_ck_parent;
	u32 cm_clkmode_dpll_abe;
} state;

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

int omap4_noncore_dpll_mn_bypass(struct clk *clk)
{
	int i, ret = 0;
	u32 reg, v;
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
	long ret;
	u32 reg;
	struct dpll_data *dd;

	dd = clk->dpll_data;

	/* CM_CLKMODE_DPLL_n.REGM4XEN add 4x multiplier to MN dividers */
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

out:
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
	int ret = 0;
	u32 reg, mask;
	int i;
	struct clk *sys_32k_ck, *sys_clkin_ck;
	struct clk *dpll_abe_ck, *dpll_abe_x2_ck;
	struct clk *abe_clk, *abe_dpll_refclk_mux_ck;
	struct clk *dpll_mpu_ck, *div_mpu_hs_clk;
	struct clk *dpll_iva_ck, *div_iva_hs_clk, *iva_hsd_byp_clk_mux_ck;
	struct clk *dpll_core_ck, *dpll_core_m2_ck, *core_hsd_byp_clk_mux_ck;
	unsigned long clk_rate;

	dpll_abe_ck = clk_get(NULL, "dpll_abe_ck");
	dpll_abe_x2_ck = clk_get(NULL, "dpll_abe_x2_ck");
	sys_32k_ck = clk_get(NULL, "sys_32k_ck");
	sys_clkin_ck = clk_get(NULL, "sys_clkin_ck");
	abe_clk = clk_get(NULL, "abe_clk");
	abe_dpll_refclk_mux_ck = clk_get(NULL, "abe_dpll_refclk_mux_ck");
	dpll_mpu_ck = clk_get(NULL, "dpll_mpu_ck");
	div_mpu_hs_clk = clk_get(NULL, "div_mpu_hs_clk");
	dpll_iva_ck = clk_get(NULL, "dpll_iva_ck");
	div_iva_hs_clk = clk_get(NULL, "div_iva_hs_clk");
	iva_hsd_byp_clk_mux_ck = clk_get(NULL, "iva_hsd_byp_clk_mux_ck");
	dpll_core_ck = clk_get(NULL, "dpll_core_ck");
	dpll_core_m2_ck = clk_get(NULL, "dpll_core_ck");

	if (!dpll_abe_ck || !dpll_abe_x2_ck || !sys_32k_ck || !sys_clkin_ck ||
			!abe_clk || !abe_dpll_refclk_mux_ck || !dpll_mpu_ck ||
			!div_mpu_hs_clk || !dpll_iva_ck || !div_iva_hs_clk ||
			!iva_hsd_byp_clk_mux_ck || !dpll_core_ck ||
			!dpll_core_m2_ck) {
		pr_warn("%s: failed to get all necessary clocks\n", __func__);
		ret = -ENODEV;
		goto out;
	}

	/* enable DPLL_ABE if not done already; usecount++ */
	clk_enable(dpll_abe_ck);

	/* bypass DPLL_ABE */
	state.dpll_abe_rate = clk_get_rate(dpll_abe_ck);
	omap3_dpll_deny_idle(dpll_abe_ck);
	ret = omap4_noncore_dpll_mn_bypass(dpll_abe_ck);

	if (ret) {
		pr_debug("%s: DPLL_ABE failed to enter bypass\n", __func__);
		goto dpll_abe_bypass_fail;
	} else
		pr_debug("%s: DPLL_ABE bypassed successfully\n", __func__);

	/* set SYS_32K_CK as input to DPLL_ABE */
	state.abe_dpll_refclk_mux_ck_parent =
		clk_get_parent(abe_dpll_refclk_mux_ck);
	ret = clk_set_parent(abe_dpll_refclk_mux_ck, sys_32k_ck);

	if (ret) {
		pr_warn("%s: clk_set_parent returns %d\n", __func__, ret);
		goto dpll_abe_reparent_fail;
	}

	/*
	 * Before re-locking DPLL_ABE at 196.608MHz CM_CLKMODE_DPLL_ABE needs
	 * to be configured specifically for DPLL cascading and for being fed
	 * from 32KHz timer.  First save the inital register contents for
	 * later on, then program the new values all at once.
	 */
	mask =	(OMAP4430_DPLL_REGM4XEN_MASK |
		 OMAP4430_DPLL_LPMODE_EN_MASK |
		 OMAP4430_DPLL_RELOCK_RAMP_EN_MASK |
		 OMAP4430_DPLL_RAMP_RATE_MASK |
		 OMAP4430_DPLL_DRIFTGUARD_EN_MASK);

	reg = __raw_readl(dpll_abe_ck->dpll_data->control_reg);
	reg &= mask;
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

	omap4_prm_rmw_reg_bits(mask, reg, dpll_abe_ck->dpll_data->control_reg);

	mdelay(10);

	/*
	 * XXX on OMAP4 the DPLL_n_X2 clocks are not twice the speed of the
	 * DPLL.  Instead they reflect the actual output of the DPLL and the
	 * non-X2 clocks are half of that output.  It would be preferable to
	 * set the rate of dpll_abe_x2_ck but that clock doesn't have any
	 * clock ops.  Program dpll_abe_ck for half of the desired rate
	 * instead.
	 */
	ret = clk_set_rate(dpll_abe_ck, 196608000 / 2);
	if (ret) {
		pr_warn("%s: failed to lock DPLL_ABE\n", __func__);
		goto dpll_abe_relock_fail;
	}

	/* divide MPU/IVA bypass clocks by 2 (for when we bypass DPLL_CORE) */
	clk_set_rate(div_mpu_hs_clk, div_mpu_hs_clk->parent->rate / 2);
	clk_set_rate(div_iva_hs_clk, div_iva_hs_clk->parent->rate / 2);

	/* prevent DPLL_MPU & DPLL_IVA from idling */
	omap3_dpll_deny_idle(dpll_mpu_ck);
	omap3_dpll_deny_idle(dpll_iva_ck);

	/* select CLKINPULOW (div_iva_hs_clk) as DPLL_IVA bypass clock */
	clk_set_parent(iva_hsd_byp_clk_mux_ck, div_iva_hs_clk);

	/* bypass DPLL_MPU */
	ret = omap3_noncore_dpll_set_rate(dpll_mpu_ck,
			dpll_mpu_ck->dpll_data->clk_bypass->rate);
	if (ret) {
		pr_debug("%s: DPLL_MPU failed to enter Low Power bypass\n",
				__func__);
		goto dpll_mpu_bypass_fail;
	} else 
		pr_debug("%s: DPLL_MPU entered Low Power bypass\n",__func__);

	/* bypass DPLL_IVA */
	ret = omap3_noncore_dpll_set_rate(dpll_iva_ck,
			dpll_iva_ck->dpll_data->clk_bypass->rate);
	if (ret) {
		pr_debug("%s: DPLL_IVA failed to enter Low Power bypass\n",
				__func__);
		goto dpll_iva_bypass_fail;
	} else 
		pr_debug("%s: DPLL_IVA entered Low Power bypass\n",__func__);

	/* Now put PER PLL in Bypass and Use Core PLL Clock */
	/* reg = __raw_readl(OMAP4430_CM_CLKSEL_DPLL_PER);
	   reg |= (0x1 << 23);
	   __raw_writel(reg, OMAP4430_CM_CLKSEL_DPLL_PER);
	   reg = __raw_readl(OMAP4430_CM_CLKMODE_DPLL_PER);
	   reg &= 0xFFFFFFF8;
	   reg |= 0x5;
	   __raw_writel(reg, OMAP4430_CM_CLKMODE_DPLL_PER);
	   printk("cpufreq-omap: Successfully put the PER DPLL into Bypass mode\n"); */

	/* Change bypass clock input to CLKINPULOW source */
	reg = __raw_readl(OMAP4430_CM_CLKSEL_DPLL_CORE);
	reg |= (0x1 << 23);
	__raw_writel(reg, OMAP4430_CM_CLKSEL_DPLL_CORE);

	/* Zero out CLKSEL_CORE divider to make CORE_CLK = CORE_X2_CLK */
	reg = __raw_readl(OMAP4430_CM_CLKSEL_CORE);
	reg &= 0xFFFFFFF0;
	__raw_writel(reg, OMAP4430_CM_CLKSEL_CORE);
	printk("cpufreq-omap: Successfully changed the CORE CLK divider setting\n");

	/* Configure EMIF Memory Interface */
	l3_emif_clkdm = clkdm_lookup("l3_emif_clkdm");
	/* Configures MEMIF domain in SW_WKUP */
	omap2_clkdm_wakeup(l3_emif_clkdm);
	/*
	 * Program EMIF timing parameters in EMIF shadow registers
	 * for targetted DRR clock.
	 * DDR Clock = core_dpll_m2 / 2
	 */
	omap_emif_setup_registers(196608000 >> 1, LPDDR2_VOLTAGE_STABLE);

	mdelay(10);

	/* reg = 0x1; */  /* For divide-by-2 on other functional clocks */
	reg = 0; /* Keep divide-by-1 for other functional clocks */
	__raw_writel(reg, OMAP4430_CM_SCALE_FCLK);

	/* Now Put CORE PLL In Bypass and Use the ABE o/p clock */

	reg = 0x2;
	__raw_writel(reg, OMAP4430_CM_MEMIF_CLKSTCTRL);

#if 0
	/* Update SHADOW register for proper CORE DPLL and EMIF config updates */
	reg = (0x2 << 11) | (0x5 << 8) | (0x1 << 3);
	__raw_writel(reg, OMAP4430_CM_SHADOW_FREQ_CONFIG1);
	reg |= 0x1;
	__raw_writel(reg, OMAP4430_CM_SHADOW_FREQ_CONFIG1);
	while (((reg = __raw_readl(OMAP4430_CM_SHADOW_FREQ_CONFIG1)) & 0x1) != 0x0)
		printk("cpufreq-omap: Waiting for CORE DPLL config to update...\n");
	printk("cpufreq-omap: Successfully updated the CORE DPLL Shadow Register\n");
#endif

	/*
	 * XXX should really omap4_core_dpll_m2_set_rate here...
	 * should really be clk_set_rate(core_m2); here...
	 */
	reg = (0x2 << 11) | (0x5 << 8);
	__raw_writel(reg, OMAP4430_CM_SHADOW_FREQ_CONFIG1);
	omap4_set_freq_update();

	/* Update CORE DPLL divider value for M5 output */
	reg = 0x1;
	__raw_writel(reg, OMAP4430_CM_DIV_M5_DPLL_CORE);
	printk("cpufreq-omap: Successfully changed the CORE DIV M5 divider setting\n");

	/* Set .parent field of core_hsd_byp_clk_mux_ck to update to the latest */
	core_hsd_byp_clk_mux_ck = clk_get(NULL, "core_hsd_byp_clk_mux_ck");
	if (!core_hsd_byp_clk_mux_ck) {
		printk("Could not get CORE HSD Bypass clock - core_hsd_byp_clk_mux_ck\n");
	} else {
		omap2_init_clksel_parent(core_hsd_byp_clk_mux_ck);
	}

	/* Set .parent field of dpll_core_ck to update to the latest */
	omap2_init_dpll_parent(dpll_core_ck);

	recalculate_root_clocks();

	/* DDR clock rate */
	clk_rate = (unsigned long) clk_get_rate(dpll_core_m2_ck);
	printk("Latest DPLL_CORE_M2_CK (EMIF source) is %ld Hz\n", clk_rate);

	/* Configures MEMIF domain back to HW_WKUP */
	omap2_clkdm_allow_idle(l3_emif_clkdm);

	/* Let HW control ABE DPLL now, since we have the DPLL's chained */
	reg = 0x1;
	__raw_writel(reg, OMAP4430_CM_AUTOIDLE_DPLL_ABE);

	/* Move PRM from SYS Clock to ABE LP Clock and ABE Bypass clock to 32kHz */
	reg = 0x1;
	__raw_writel(reg, OMAP4430_CM_L4_WKUP_CLKSEL);

	/* Program the CLKREQCTRL in PRM */
	reg = 0;
	__raw_writel(reg, OMAP4430_PRM_CLKREQCTRL);

	/* Program emu cd to HW-AUTO mode and change clock source */
	reg = __raw_readl(OMAP4430_CM_EMU_DEBUGSS_CLKCTRL);
	reg |= (0x1 << 22) | (0x1 << 20);
	__raw_writel(reg, OMAP4430_CM_EMU_DEBUGSS_CLKCTRL);

	reg = 0x3;
	__raw_writel(reg, OMAP4430_CM_EMU_CLKSTCTRL);

	printk("cpufreq-omap: Done forcing DPLL Cascading\n");

	goto out;

dpll_mpu_bypass_fail:
dpll_iva_bypass_fail:
dpll_abe_relock_fail:
dpll_abe_reparent_fail:
dpll_abe_bypass_fail:
	clk_set_rate(dpll_abe_ck, state.dpll_abe_rate);
	omap3_dpll_allow_idle(dpll_abe_ck);
out:
	return ret;
}

int omap4_dpll_low_power_cascade_exit()
{
	pr_err("%s\n", __func__);
	return 0;
}
