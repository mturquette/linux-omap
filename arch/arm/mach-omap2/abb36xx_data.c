/*
 * OMAP36xx Adaptive Body-Bias (ABB) data
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Mike Turquette <mturquette@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "abb.h"
#include "prm-regbits-34xx.h"

/*
 * so far all "ops" identified can be abstracted out of abbXXXX_data.c
 * and put into abb.c/abb.h.  This includes:
 * 	set_opp / transition function (one function to rule them all
 * 	maybe enable / disable functions (again, reuse across omap3 & omap4)
 * 
 * so question remains, should common structs, instance structs or no structs
 * point towards these functions?  at this point I feel ABB is starting to
 * look more like VC code than VP code, since functions are identical across
 * different OMAPs: we only need to track register offsets and shift/mask data
 */

#if 0
/* ABB data common to all VDDs */
static struct omap_abb_common omap36xx_abb_common = {
	.done_st_shift	= OMAP3630_ABB_LDO_TRANXDONE_ST_SHIFT,
	.done_st_mask	= OMAP3630_ABB_LDO_TRANXDONE_ST_MASK,
};
#endif

/*
 * ok looks like there is no common data across VDDs anyways...
 *
 * WAIT!  yes there is.  look at the old crappy patches I did and look at the
 * configure and set_opp functions... lots of #define macros used explicitly
 * there that should be abstracted into common structs.
 */

/*
   OMAP3630_OPP_SEL_MASK
   OMAP3630_OPP_SEL_SHIFT
   OMAP3430_GR_MOD
   OMAP3630_OPP_CHANGE_MASK
   ABB_TRANXDONE_TIMEOUT
*/

static const struct omap_abb_ops omap36xx_abb_ops = {
	.check_tranxdone	= omap36xx_prm_abb_check_tranxdone,
	.clear_tranxdone	= omap36xx_prm_abb_clear_tranxdone,
};

static const struct omap_abb_common omap36xx_abb_common = {
	.opp_sel_mask		= OMAP3630_OPP_SEL_MASK,
	.opp_sel_shift		= OMAP3630_OPP_SEL_SHIFT,
	.opp_change_mask	= OMAP3630_OPP_CHANGE_MASK,
	.settling_time		= 30,
	.cycl_rate		= 8,
	.ops			= &omap36xx_abb_opps,
};

struct omap_abb_instance omap36xx_abb_mpu = {
	.setup_offs		= OMAP3_PRM_LDO_ABB_SETUP_OFFSET,
	.ctrl_offs		= OMAP3_PRM_LDO_ABB_CTRL_OFFSET,
	.irqstatus_mpu_offs	= OMAP3_PRM_IRQSTATUS_MPU_OFFSET,
	.done_st_shift		= OMAP3630_ABB_LDO_TRANXDONE_ST_SHIFT,
	.done_st_mask		= OMAP3630_ABB_LDO_TRANXDONE_ST_MASK,
};
