/*
 * OMAP Adaptive Body-Bias core
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Mike Turquette <mturquette@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>

#include "abb.h"
#include "voltage.h"

int omap_abb_set_opp(struct voltagedomain *voltdm, u8 abb_type)
{
	struct omap_abb_instance *abb = voltdm->abb;
	int ret, timeout;

	/* clear interrupt status */
	timeout = 0;
	while (timeout++ < ABB_TRANXDONE_TIMEOUT) {
		abb->common->ops->clear_tranxdone(abb->done_st_mask,
				abb->irqstatus_mpu_offs);

		ret = abb->common->ops->check_tranxdone(abb->done_st_mask,
				abb->irqstatus_mpu_offs);
		if (!ret)
			break;

		udelay(1);
	}

	if (timeout>= ABB_TRANXDONE_TIMEOUT) {
		pr_warning("%s: vdd_%s ABB TRANXDONE timeout\n",
				__func__, voltdm->name);
		return -ETIMEDOUT;
	}

	/* program next state of ABB ldo */
	voltdm->rmw(abb->common->opp_sel_mask,
			abb_type << abb->common->opp_sel_shift,
			abb->ctrl_offs);

	/* initiate ABB ldo change */
	voltdm->rmw(abb->common->opp_change_mask,
			abb->common->opp_change_mask,
			abb->ctrl_offs);

	/* clear interrupt status */
	timeout = 0;
	while (timeout++ < ABB_TRANXDONE_TIMEOUT) {
		abb->common->ops->clear_tranxdone(abb->done_st_mask,
				abb->irqstatus_mpu_offs);

		ret = abb->common->ops->check_tranxdone(abb->done_st_mask,
				abb->irqstatus_mpu_offs);
		if (!ret)
			break;

		udelay(1);
	}

	if (timeout>= ABB_TRANXDONE_TIMEOUT) {
		pr_warning("%s: vdd_%s ABB TRANXDONE timeout\n",
				__func__, voltdm->name);
		return -ETIMEDOUT;
	}

	return 0;
}

void omap_abb_enable(struct voltagedomain *voltdm)
{
	struct omap_abb_instance *abb = voltdm->abb;

	voltdm->rmw(abb->common->sr2en_mask, abb->common->sr2en_mask,
			abb->setup_offs);
}

/* not used, but will be needed if made into a loadable module */
void omap_abb_disable(struct voltagedomain *voltdm)
{
	struct omap_abb_instance *abb = voltdm->abb;

	voltdm->rmw(abb->common->sr2en_mask, (0 << abb->common->sr2en_shift),
			abb->setup_offs);
}

/* initialize an ABB instance for Forward Body-Bias */
void __init omap_abb_init(struct voltagedomain *voltdm)
{
	struct omap_abb_instance *abb = voltdm->abb;
	unsigned long sys_clk_rate;
	u32 sr2_wt_cnt_val;

	if(IS_ERR_OR_NULL(abb))
		return -EINVAL;

	sys_clk_rate = voltdm->sys_clk.rate / 1000;
	pr_err("%s: sys_clk_rate is %lu\n", __func__, sys_clk_rate);

	sr2_wt_cnt_val = DIV_ROUND_UP(sys_clk_rate, 16000000);
	pr_err("%s: sr2_wt_cnt_val is %lu\n", __func__, sr2_wt_cnt_val);

	voltdm->rmw(abb->common->sr2_wtcnt_value_mask,
			(sr2_wt_cnt_val << abb->common->sr2_wtcnt_value_shift),
			abb->setup_offs);

	/* allow Forward Body-Bias */
	voltdm->rmw(abb->common->active_fbb_sel_mask,
			abb->common->active_fbb_sel_mask,
			abb->setup_offs);

	/* enable the ldo */
	omap_abb_enable(voltdm);

	return 0;
}
