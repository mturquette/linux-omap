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

long omap4_dpll_regm4xen_round_rate(struct clk *clk, unsigned long target_rate)
{
	long ret;
	u32 reg;
	struct dpll_data *dd;

	dd = clk->dpll_data;

	omap2_dpll_round_rate(clk, target_rate);

	/* regm4xen adds a multiplier of 4 to DPLL calculations */
	reg = cm_read_mod_reg(OMAP4430_CM1_CKGEN_MOD,
			OMAP4_CM_CLKMODE_DPLL_ABE_OFFSET);
	if (reg & (DPLL_REGM4XEN_ENABLE << OMAP4430_DPLL_REGM4XEN_SHIFT)) {
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

	/*
	 * Reparent DPLL_ABE so that it is fed by SYS_32K_CK.  Magical
	 * REGM4XEN registers allows us to multiply MN dividers by 4 so that
	 * we can get 196.608MHz out of DPLL_ABE (other DPLLs do not have this
	 * feature).  Divide the output of that clock by 4 so that AESS
	 * functional clock can be 48.152MHz.
	 */

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

	/* Device RET/OFF are not supported in DPLL cascading; gate them */
	omap3_dpll_deny_idle(dpll_abe_ck);

	/* enable DPLL_ABE if not done already */
	clk_enable(dpll_abe_ck);

	/* bypass DPLL_ABE */
	state.dpll_abe_rate = clk_get_rate(dpll_abe_ck);
	omap3_noncore_dpll_set_rate(dpll_abe_ck, dpll_abe_ck->parent->rate);

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
		pr_warn("%s: clk_get_parent returns %d\n", __func__, ret);
		goto dpll_abe_reparent_fail;
	}

	/* program DPLL_ABE for 196.608MHz */
	/* set DPLL_ABE REGM4XEN bit */
	omap4_dpll_regm4xen_enable(dpll_abe_ck);

	pr_err("%s: ATTN: CLKMODE_DPLL_ABE is 0x%x\n", __func__,
			cm_read_mod_reg(OMAP4430_CM1_CKGEN_MOD,
				OMAP4_CM_CLKMODE_DPLL_ABE_OFFSET));

	/* save CKGEN_CM1.CM_CLKMODE_DPLL_ABE */
	mask =	(OMAP4430_DPLL_LPMODE_EN_MASK |
		 OMAP4430_DPLL_RELOCK_RAMP_EN_MASK |
		 OMAP4430_DPLL_RAMP_RATE_MASK |
		 OMAP4430_DPLL_DRIFTGUARD_EN_MASK);

	pr_err("%s: ATT: mask is 0x%x\n", __func__, mask);

	reg = cm_read_mod_reg(OMAP4430_CM1_CKGEN_MOD,
			OMAP4_CM_CLKMODE_DPLL_ABE_OFFSET);
	pr_err("%s: ATTN: reg is 0x%x\n", __func__, reg);
	reg &= mask;
	pr_err("%s: ATTN: reg is 0x%x\n", __func__, reg);
	state.cm_clkmode_dpll_abe = reg;

	mdelay(10);

	/*
	 * DPLL_ABE LP Mode Enable
	 * DPLL_ABE Relock Ramp Enable
	 * DPLL_ABE Ramp Rate
	 * DPLL_ABE Driftguard Enable
	 */
	reg = ((0x1 << OMAP4430_DPLL_LPMODE_EN_SHIFT) |
			(0x1 << OMAP4430_DPLL_RELOCK_RAMP_EN_SHIFT) |
			(0x1 << OMAP4430_DPLL_RAMP_RATE_SHIFT) |
			(0x1 << OMAP4430_DPLL_DRIFTGUARD_EN_SHIFT));

	cm_rmw_mod_reg_bits(mask, reg, OMAP4430_CM1_CKGEN_MOD,
			OMAP4_CM_CLKMODE_DPLL_ABE_OFFSET);

	mdelay(10);
	/*
	 * XXX on OMAP4 the DPLL X2 clocks aren't really X2.  Instead they
	 * reflect the actual output DPLL and the non-X2 clocks are half of
	 * that output.  It would be preferable to set the rate of
	 * dpll_abe_x2_ck but that clock doesn't have any clock ops.  Program
	 * dpll_abe_ck for half of the desired rate instead.
	 */
	clk_set_rate(dpll_abe_ck, 196608000 / 2);

	pr_err("%s: clk_get_rate(dpll_abe_ck) is %lu\n",
			__func__, clk_get_rate(dpll_abe_ck));
	pr_err("%s: clk_get_rate(dpll_abe_x2_ck) is %lu\n",
			__func__, clk_get_rate(dpll_abe_ck));
	pr_err("%s: dpll_abe_ck->rate is %lu\n", __func__,
			dpll_abe_ck->rate);
	pr_err("%s: dpll_abe_x2_ck->rate is %lu\n", __func__,
			dpll_abe_x2_ck->rate);

#if 0
	pr_err("%s: CKGEN_CM1.CM_CLKSEL_ABB.CLKSEL_OPP is 0x%x\n",
			__func__, cm_read_mod_reg(OMAP4430_CM1_CKGEN_MOD,
				OMAP4_CM_CLKSEL_ABE_OFFSET));

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

	/* Program the MPU and IVA Bypass clock dividers for div by 2 */
	reg = 0x1;
	__raw_writel(reg, OMAP4430_CM_BYPCLK_DPLL_MPU);
	__raw_writel(reg, OMAP4430_CM_BYPCLK_DPLL_IVA);
	printk("cpufreq-omap: Successfully changed the MPU & IVA clock dividers\n");

	/* Configure EMIF Memory Interface */
	printk("cpufreq-omap: Now changing the EMIF clock rate setting for DPLL cascading...\n");
	//validrate = 196608000;
	/* validrate = 98304000; */
	l3_emif_clkdm = clkdm_lookup("l3_emif_clkdm");
	/* Configures MEMIF domain in SW_WKUP */
	omap2_clkdm_wakeup(l3_emif_clkdm);
	/*
	 * Program EMIF timing parameters in EMIF shadow registers
	 * for targetted DRR clock.
	 * DDR Clock = core_dpll_m2 / 2
	 */
	omap_emif_setup_registers(196608000 >> 1, LPDDR2_VOLTAGE_STABLE);

	/* Disable auto-control for MPU and IVA PLL's */
	reg = 0;
	__raw_writel(reg, OMAP4430_CM_AUTOIDLE_DPLL_MPU);
	__raw_writel(reg, OMAP4430_CM_AUTOIDLE_DPLL_IVA);

	/* Now Put MPU and IVA PLL's in Bypass and Use Core PLL Clock as Bypass source*/

	reg = __raw_readl(OMAP4430_CM_CLKMODE_DPLL_MPU);
	reg &= 0xFFFFFFF8;
	reg |= 0x5;
	__raw_writel(reg, OMAP4430_CM_CLKMODE_DPLL_MPU);
	printk("cpufreq-omap: Successfully put the MPU DPLL into Bypass mode\n");

	/* Change IVA DPLL bypass clock input to CLKINPULOW source */
	reg = __raw_readl(OMAP4430_CM_CLKSEL_DPLL_IVA);
	reg |= (0x1 << 23);
	__raw_writel(reg, OMAP4430_CM_CLKSEL_DPLL_IVA);

	reg = __raw_readl(OMAP4430_CM_CLKMODE_DPLL_IVA);
	reg &= 0xFFFFFFF8;
	reg |= 0x5;
	__raw_writel(reg, OMAP4430_CM_CLKMODE_DPLL_IVA);
	printk("cpufreq-omap: Successfully put the IVA DPLL into Bypass mode\n");

	/* Now put PER PLL in Bypass and Use Core PLL Clock */
	/* reg = __raw_readl(OMAP4430_CM_CLKSEL_DPLL_PER);
	   reg |= (0x1 << 23);
	   __raw_writel(reg, OMAP4430_CM_CLKSEL_DPLL_PER);
	   reg = __raw_readl(OMAP4430_CM_CLKMODE_DPLL_PER);
	   reg &= 0xFFFFFFF8;
	   reg |= 0x5;
	   __raw_writel(reg, OMAP4430_CM_CLKMODE_DPLL_PER);
	   printk("cpufreq-omap: Successfully put the PER DPLL into Bypass mode\n"); */

	/* reg = 0x1; */  /* For divide-by-2 on other functional clocks */
	reg = 0; /* Keep divide-by-1 for other functional clocks */
	__raw_writel(reg, OMAP4430_CM_SCALE_FCLK);

	/* Now Put CORE PLL In Bypass and Use the ABE o/p clock */

	reg = 0x2;
	__raw_writel(reg, OMAP4430_CM_MEMIF_CLKSTCTRL);

	/* Change bypass clock input to CLKINPULOW source */
	reg = __raw_readl(OMAP4430_CM_CLKSEL_DPLL_CORE);
	reg |= (0x1 << 23);
	__raw_writel(reg, OMAP4430_CM_CLKSEL_DPLL_CORE);

	/* Zero out CLKSEL_CORE divider to make CORE_CLK = CORE_X2_CLK */
	reg = __raw_readl(OMAP4430_CM_CLKSEL_CORE);
	reg &= 0xFFFFFFF0;
	__raw_writel(reg, OMAP4430_CM_CLKSEL_CORE);
	printk("cpufreq-omap: Successfully changed the CORE CLK divider setting\n");

	/* Update SHADOW register for proper CORE DPLL and EMIF config updates */
	reg = (0x2 << 11) | (0x5 << 8) | (0x1 << 3);
	__raw_writel(reg, OMAP4430_CM_SHADOW_FREQ_CONFIG1);
	reg |= 0x1;
	__raw_writel(reg, OMAP4430_CM_SHADOW_FREQ_CONFIG1);
	while (((reg = __raw_readl(OMAP4430_CM_SHADOW_FREQ_CONFIG1)) & 0x1) != 0x0)
		printk("cpufreq-omap: Waiting for CORE DPLL config to update...\n");
	printk("cpufreq-omap: Successfully updated the CORE DPLL Shadow Register\n");

	/* Update CORE DPLL divider value for M5 output */
	reg = 0x1;
	__raw_writel(reg, OMAP4430_CM_DIV_M5_DPLL_CORE);
	printk("cpufreq-omap: Successfully changed the CORE DIV M5 divider setting\n");

	/* Set .parent field of dpll_abe_ck to update (in case DPLL was in Bypass before) */
	omap2_init_dpll_parent(dpll_abe_ck);

	/* Set .parent field of core_hsd_byp_clk_mux_ck to update to the latest */
	core_hsd_byp_clk_mux_ck = clk_get(NULL, "core_hsd_byp_clk_mux_ck");
	if (!core_hsd_byp_clk_mux_ck) {
		printk("Could not get CORE HSD Bypass clock - core_hsd_byp_clk_mux_ck\n");
	} else {
		omap2_init_clksel_parent(core_hsd_byp_clk_mux_ck);
	}

	/* Set .parent field of dpll_core_ck to update to the latest */
	omap2_init_dpll_parent(dpll_core_ck);

	/* Set .parent field of iva_hsd_byp_clk_mux_ck to update to the latest */
	iva_hsd_byp_clk_mux_ck = clk_get(NULL, "iva_hsd_byp_clk_mux_ck");
	if (!iva_hsd_byp_clk_mux_ck) {
		printk("Could not get IVA HSD Bypass clock - iva_hsd_byp_clk_mux_ck\n");
	} else {
		omap2_init_clksel_parent(iva_hsd_byp_clk_mux_ck);
	}

	/* Set .parent field of dpll_iva_ck to update to the latest */
	omap2_init_dpll_parent(dpll_iva_ck);

	/* Set .parent field of dpll_mpu_ck to update to the latest */
	omap2_init_dpll_parent(dpll_mpu_ck);

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

out_clksel_opp:
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
