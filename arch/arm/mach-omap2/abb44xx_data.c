/*
 * OMAP44xx Adaptive Body-Bias (ABB) data
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Mike Turquette <mturquette@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "abb.h"
#include "prm-regbits-44xx.h"

static const struct omap_abb_ops omap44xx_abb_ops = {
	.check_tranxdone	= omap44xx_prm_abb_check_tranxdone,
	.clear_tranxdone	= omap44xx_prm_abb_clear_tranxdone,
};

static const struct omap_abb_common omap44xx_abb_common = {
	.opp_sel_mask		= OMAP4430_OPP_SEL_MASK,
	.opp_sel_shift		= OMAP4430_OPP_SEL_SHIFT,
	.opp_change_mask	= OMAP4430_OPP_CHANGE_MASK,
	.ops			= &omap44xx_abb_opps,
};

struct omap_abb_instance omap44xx_abb_mpu = {
	.setup_offs		= OMAP4_PRM_LDO_ABB_MPU_SETUP_OFFSET,
	.ctrl_offs		= OMAP4_PRM_LDO_ABB_MPU_CTRL_OFFSET,
	.irqstatus_mpu_offs	= OMAP4_PRM_IRQSTATUS_MPU_2_OFFSET,
	.done_st_shift		= OMAP4430_ABB_MPU_DONE_ST_SHIFT,
	.done_st_mask		= OMAP4430_ABB_MPU_DONE_ST_MASK,
};

struct omap_abb_instance omap44xx_iva_mpu = {
	.setup_offs		= OMAP4_PRM_LDO_ABB_IVA_SETUP_OFFSET,
	.ctrl_offs		= OMAP4_PRM_LDO_ABB_IVA_CTRL_OFFSET,
	.irqstatus_mpu_offs	= OMAP4_PRM_IRQSTATUS_MPU_OFFSET,
	.done_st_shift		= OMAP4430_ABB_IVA_DONE_ST_SHIFT,
	.done_st_mask		= OMAP4430_ABB_IVA_DONE_ST_MASK,
};
