/*
 * OMAP Adaptive Body-Bias structure and macro definitions
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Mike Turquette <mturquette@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __ARCH_ARM_MACH_OMAP2_ABB_H
#define __ARCH_ARM_MACH_OMAP2_ABB_H

//#include <linux/kernel.h>

#define NOMINAL_OPP	0
#define FAST_OPP	1

struct omap_abb_ops {
	u32 (*check_tranxdone)(u8 abb_id);
	void (*clear_tranxdone)(u8 abb_id);
};

struct omap_abb_common {
	u32 opp_sel_mask;
	u32 opp_change_mask;
	u32 sr2_wtcnt_value_mask;
	u8 opp_sel_shift;
	u8 sr2en_shift;
	u8 active_fbb_sel_shift;
	u8 sr2_wtcnt_value_shift;
	const struct omap_abb_ops *ops;
};

struct omap_abb_instance {
	u8 setup_offs;
	u8 ctrl_offs;
	u16 irqstatus_mpu_offs;
	u8 done_st_shift;
	u8 done_st_mask;
	u8 id;
	bool enabled;
	const struct omap_abb_common *common;
};

extern struct omap_abb_instance omap36xx_abb_mpu;

extern struct omap_abb_instance omap4_abb_mpu;
extern struct omap_abb_instance omap4_abb_iva;

void omap_abb_init(struct voltagedomain *voltdm);
int omap_abb_enable(struct voltagedomain *voltdm);
int omap_abb_disble(struct voltagedomain *voltdm);
int omap_abb_set_opp(struct voltagedomain *voltdm, u8 abb_type);

#endif
