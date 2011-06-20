/* voltage transition notification handlers */

/**
 * omap_abb_notify_voltage - voltage change notifier handler for ABB
 * @nb         : notifier block
 * @val                : VOLTSCALE_PRECHANGE or VOLTSCALE_POSTCHANGE
 * @data       : struct omap_volt_change_info for a given voltage domain
 *
 * Sets ABB LDO to either bypass or Forward Body-Bias whenever a voltage
 * change notification is generated.  Voltages marked as FAST will result in
 * FBB operation of ABB LDO and voltages marked as NOMINAL will bypass the
 * LDO.  Does not handle Reverse Body-Bias since there is not benefit for it
 * on any existing silicon.  Returns 0 upon success, negative error code
 * otherwise.
 */
static int omap_abb_notify_voltage(struct notifier_block *nb,
		unsigned long val, void *data)
{
	struct omap_volt_change_info *v_info;
	struct omap_vdd_info *vdd;
	struct omap_volt_data *curr_volt_data, *target_volt_data;
	int ret = 0;

	if (!nb || IS_ERR(nb) || !data || IS_ERR(data)) {
		pr_warning("%s: invalid data specified\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	v_info = (struct omap_volt_change_info *)data;
	vdd = v_info->vdd;

	/* get the voltdata structures for the current & target voltage */
	target_volt_data = omap_voltage_get_voltdata(&vdd->voltdm,
			v_info->target_volt);
	curr_volt_data = omap_voltage_get_voltdata(&vdd->voltdm,
			v_info->curr_volt);

	/* nothing to do here */
	if (target_volt_data->abb_opp == curr_volt_data->abb_opp)
		goto out;

	/*
	 * When the VDD drops from a voltage requiring the ABB LDO to be in
	 * FBB mode to a voltage requiring bypass mode, we must bypass the LDO
	 * before the voltage transition.
	 */
	if (val == VOLTSCALE_PRECHANGE &&
			target_volt_data->abb_opp == NOMINAL_OPP) {
		ret = vdd->abb.set_opp(&vdd->abb, NOMINAL_OPP);

		/*
		 * When moving from a voltage requiring the ABB LDO to be bypassed to
		 * a voltage requiring FBB mode, we must change the LDO operation
		 * after the voltage transition.
		 */
	} else if (val == VOLTSCALE_POSTCHANGE &&
			target_volt_data->abb_opp == FAST_OPP) {
		ret = vdd->abb.set_opp(&vdd->abb, FAST_OPP);

		/* invalid combination, bail out */
	} else
		ret = -EINVAL;

out:
	return ret;
}

/* Voltage scale and accessory APIs */
static int _pre_volt_scale(struct omap_vdd_info *vdd,
		unsigned long target_volt, u8 *target_vsel, u8 *current_vsel)
@@ -717,6 +801,142 @@ static int vp_forceupdate_scale_voltage(struct omap_vdd_info *vdd,
		return 0;
		}

		/**
		 * omap3_abb_set_opp - program ABB LDO upon a voltage transition
		 *
		 * @abb                : ABB instance being programmed
		 * @opp_type   : flag for NOMINAL or FAST OPP
		 */
		static int omap3_abb_set_opp(struct omap_abb_info *abb, int opp_type)
		{
		int ret = 0;
		int timeout;

		/* program for NOMINAL OPP or FAST OPP */
		omap2_prm_rmw_mod_reg_bits(OMAP3630_OPP_SEL_MASK,
			(opp_type << OMAP3630_OPP_SEL_SHIFT),
			OMAP3430_GR_MOD, abb->setup_offs);

		/* clear ABB ldo interrupt status */
		omap2_prm_clear_mod_reg_bits(abb->done_st_mask, OCP_MOD,
				abb->irqstatus_mpu_offs);

		/* enable ABB LDO OPP change */
		omap2_prm_set_mod_reg_bits(OMAP3630_OPP_CHANGE_MASK, OMAP3430_GR_MOD,
				abb->setup_offs);

		timeout = 0;

		/* wait until OPP change completes */
		while ((timeout < ABB_TRANXDONE_TIMEOUT) &&
				(!(omap2_prm_read_mod_reg(OCP_MOD,
							  abb->irqstatus_mpu_offs) &
				   abb->done_st_mask))) {
			udelay(1);
			timeout++;
		}

		if (timeout == ABB_TRANXDONE_TIMEOUT)
			pr_warning("%s: TRANXDONE timed out waiting for OPP change\n",
					__func__);

		timeout = 0;

		/* Clear all pending TRANXDONE interrupts/status */
		while (timeout < ABB_TRANXDONE_TIMEOUT) {
			omap2_prm_write_mod_reg((1 << abb->done_st_shift), OCP_MOD,
					abb->irqstatus_mpu_offs);

			if (!(omap2_prm_read_mod_reg(OCP_MOD, abb->irqstatus_mpu_offs)
						& abb->done_st_mask))
				break;

			udelay(1);
			timeout++;
		}

		if (timeout == ABB_TRANXDONE_TIMEOUT) {
			pr_warning("%s: TRANXDONE timed out trying to clear status\n",
					__func__);
			ret = -EBUSY;
		}

		return ret;
		}

/**
 * omap4_abb_set_opp - program ABB LDO upon a voltage transition
 *
 * @abb                : ABB instance being programmed
 * @opp_type   : flag for NOMINAL or FAST OPP
 */
static int omap4_abb_set_opp(struct omap_abb_info *abb, int opp_type)
{
	int ret = 0;
	int timeout;

	/* program for NOMINAL OPP or FAST OPP */
	omap4_prminst_rmw_inst_reg_bits(OMAP4430_OPP_SEL_MASK,
			(opp_type << OMAP4430_OPP_SEL_SHIFT),
			OMAP4430_PRM_PARTITION, OMAP4430_PRM_DEVICE_INST,
			abb->ctrl_offs);

	/* clear ABB ldo interrupt status */
	omap4_prminst_rmw_inst_reg_bits(abb->done_st_mask,
			(0x0 << abb->done_st_shift), OMAP4430_PRM_PARTITION,
			OMAP4430_PRM_DEVICE_INST, abb->ctrl_offs);

	/* enable ABB LDO OPP change */
	omap4_prminst_rmw_inst_reg_bits(OMAP4430_OPP_CHANGE_MASK,
			(0x1 << OMAP4430_OPP_CHANGE_SHIFT),
			OMAP4430_PRM_PARTITION, OMAP4430_PRM_DEVICE_INST,
			abb->ctrl_offs);

	timeout = 0;

	/* wait until OPP change completes */
	while ((timeout < ABB_TRANXDONE_TIMEOUT) &&
			(!(omap4_prminst_read_inst_reg(OMAP4430_PRM_PARTITION,
						       OMAP4430_PRM_DEVICE_INST,
						       abb->irqstatus_mpu_offs)
			   & abb->done_st_mask))) {
		udelay(1);
		timeout++;
	}

	if (timeout == ABB_TRANXDONE_TIMEOUT)
		pr_warning("%s: TRANXDONE timed out waiting for OPP change\n",
				__func__);

	timeout = 0;

	/* Clear all pending TRANXDONE interrupts/status */
	while (timeout < ABB_TRANXDONE_TIMEOUT) {
		omap4_prminst_write_inst_reg((1 << abb->done_st_shift),
				OMAP4430_PRM_PARTITION,
				OMAP4430_PRM_DEVICE_INST,
				abb->irqstatus_mpu_offs);

		if (!(omap4_prminst_read_inst_reg(OMAP4430_PRM_PARTITION,
						OMAP4430_PRM_DEVICE_INST,
						abb->irqstatus_mpu_offs)
					& abb->done_st_mask)) {
			break;

			udelay(1);
			timeout++;
		}
	}

	if (timeout == ABB_TRANXDONE_TIMEOUT) {
		pr_warning("%s: TRANXDONE timed out trying to clear status\n",
				__func__);
		ret = -EBUSY;
	}

	return ret;
}

/* OMAP3 specific voltage init functions */

/*
   @@ -789,6 +1009,64 @@ static void __init omap3_vc_init(struct omap_vdd_info *vdd)
   is_initialized = true;
   }

/**
 * omap3_abb_configure - per-VDD configuration of ABB
 *
 * @abb                : abb instance being initialized
 */
static int omap3_abb_configure(struct omap_abb_info *abb)
{
	int ret = 0;
	u32 sr2_wt_cnt_val;
	struct clk *sys_ck;
	struct omap_vdd_info *vdd;

	if (!abb || IS_ERR(abb)) {
		pr_warning("%s: invalid abb\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	sys_ck = clk_get(NULL, "sys_ck");
	if (IS_ERR(sys_ck)) {
		pr_warning("%s: unable to fetch SYS_CK\n", __func__);
		ret = -ENODEV;
		goto out;
	}

	vdd = container_of(abb, struct omap_vdd_info, abb);

	/* LDO settling time */
	sr2_wt_cnt_val = clk_get_rate(sys_ck);
	sr2_wt_cnt_val = sr2_wt_cnt_val / 1000000 / 16;

	omap2_prm_rmw_mod_reg_bits(OMAP3630_SR2_WTCNT_VALUE_MASK,
			(sr2_wt_cnt_val << OMAP3630_SR2_WTCNT_VALUE_SHIFT),
			OMAP3430_GR_MOD, abb->setup_offs);

	/* allow FBB operation */
	omap2_prm_set_mod_reg_bits(OMAP3630_ACTIVE_FBB_SEL_MASK,
			OMAP3430_GR_MOD, abb->setup_offs);

	/* do not allow ACTIVE RBB operation */
	omap2_prm_set_mod_reg_bits(OMAP3630_ACTIVE_RBB_SEL_MASK,
			OMAP3430_GR_MOD, abb->setup_offs);

	/* do not allow SLEEP RBB operation */
	omap2_prm_set_mod_reg_bits(OMAP3630_SLEEP_RBB_SEL_MASK,
			OMAP3430_GR_MOD, abb->setup_offs);

	/* enable ABB LDO */
	omap2_prm_set_mod_reg_bits(OMAP3630_SR2EN_MASK,
			OMAP3430_GR_MOD, abb->ctrl_offs);

	/* register the notifier handler */
	omap_voltage_register_notifier(vdd, &abb->nb);

out:
	return ret;
}

/* Sets up all the VDD related info for OMAP3 */
static int __init omap3_vdd_data_configure(struct omap_vdd_info *vdd)
{
	@@ -824,6 +1102,9 @@ static int __init omap3_vdd_data_configure(struct omap_vdd_info *vdd)
		vdd->vc_reg.smps_volra_mask = OMAP3430_VOLRA0_MASK;
	vdd->vc_reg.voltsetup_shift = OMAP3430_SETUP_TIME1_SHIFT;
	vdd->vc_reg.voltsetup_mask = OMAP3430_SETUP_TIME1_MASK;

	/* configure ABB */
	vdd->abb.configure(&vdd->abb);
} else if (!strcmp(vdd->voltdm.name, "core")) {
	if (cpu_is_omap3630())
		vdd->volt_data = omap36xx_vddcore_volt_data;
	@@ -975,6 +1256,73 @@ static void __init omap4_vc_init(struct omap_vdd_info *vdd)
		is_initialized = true;
}

/**
 * omap4_abb_configure - per-VDD configuration of ABB
 *
 * @abb                : abb instance being initialized
 */
static int omap4_abb_configure(struct omap_abb_info *abb)
{
	int ret = 0;
	u32 sr2_wt_cnt_val;
	struct clk *sys_ck;
	struct omap_vdd_info *vdd;

	if (!abb || IS_ERR(abb)) {
		pr_warning("%s: invalid abb\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	sys_ck = clk_get(NULL, "sys_clkin_ck");
	if (IS_ERR(sys_ck)) {
		pr_warning("%s: unable to fetch SYS_CK", __func__);
		ret = -ENODEV;
		goto out;
	}

	vdd = container_of(abb, struct omap_vdd_info, abb);

	/* LDO settling time */
	sr2_wt_cnt_val = clk_get_rate(sys_ck);
	sr2_wt_cnt_val = sr2_wt_cnt_val / 1000000 / 16;

	omap4_prminst_rmw_inst_reg_bits(OMAP4430_SR2_WTCNT_VALUE_MASK,
			(sr2_wt_cnt_val << OMAP4430_SR2_WTCNT_VALUE_SHIFT),
			OMAP4430_PRM_PARTITION, OMAP4430_PRM_DEVICE_INST,
			abb->setup_offs);

	/* allow FBB operation */
	omap4_prminst_rmw_inst_reg_bits(OMAP4430_ACTIVE_FBB_SEL_MASK,
			(1 << OMAP4430_ACTIVE_FBB_SEL_SHIFT),
			OMAP4430_PRM_PARTITION, OMAP4430_PRM_DEVICE_INST,
			abb->setup_offs);

	/* do not allow ACTIVE RBB operation */
	omap4_prminst_rmw_inst_reg_bits(OMAP4430_ACTIVE_RBB_SEL_MASK,
			(0 << OMAP4430_ACTIVE_RBB_SEL_SHIFT),
			OMAP4430_PRM_PARTITION, OMAP4430_PRM_DEVICE_INST,
			abb->setup_offs);

	/* do not allow SLEEP RBB operation */
	omap4_prminst_rmw_inst_reg_bits(OMAP4430_SLEEP_RBB_SEL_MASK,
			(0 << OMAP4430_SLEEP_RBB_SEL_SHIFT),
			OMAP4430_PRM_PARTITION, OMAP4430_PRM_DEVICE_INST,
			abb->setup_offs);

	/* enable ABB LDO */
	omap4_prminst_rmw_inst_reg_bits(OMAP4430_SR2EN_MASK,
			(1 << OMAP4430_SR2EN_SHIFT),
			OMAP4430_PRM_PARTITION, OMAP4430_PRM_DEVICE_INST,
			abb->setup_offs);

	/* register the notifier handler */
	omap_voltage_register_notifier(vdd, &abb->nb);

out:
	return ret;
}


---

@@ -983,8 +1331,8 @@ static int __init omap4_vdd_data_configure(struct omap_vdd_info *vdd)

       if (!vdd->pmic_info) {
               pr_err("%s: PMIC info requried to configure vdd_%s not"
-                       "populated.Hence cannot initialize vdd_%s\n",
-                       __func__, vdd->voltdm.name, vdd->voltdm.name);
+                               "populated.Hence cannot initialize vdd_%s\n",
+                               __func__, vdd->voltdm.name, vdd->voltdm.name);
               return -EINVAL;
       }

@@ -1005,6 +1353,9 @@ static int __init omap4_vdd_data_configure(struct omap_vdd_info *vdd)
               vdd->vc_reg.voltsetup_reg =
                               OMAP4_PRM_VOLTSETUP_MPU_RET_SLEEP_OFFSET;
               vdd->prm_irqst_reg = OMAP4_PRM_IRQSTATUS_MPU_2_OFFSET;
+
+               /* configure ABB */
+               vdd->abb.configure(&vdd->abb);
       } else if (!strcmp(vdd->voltdm.name, "core")) {
               vdd->volt_data = omap44xx_vdd_core_volt_data;
               vdd->vp_reg.tranxdone_status =
@@ -1032,6 +1383,9 @@ static int __init omap4_vdd_data_configure(struct omap_vdd_info *vdd)
               vdd->vc_reg.voltsetup_reg =
                               OMAP4_PRM_VOLTSETUP_IVA_RET_SLEEP_OFFSET;
               vdd->prm_irqst_reg = OMAP4_PRM_IRQSTATUS_MPU_OFFSET;
+
+               /* configure ABB */
+               vdd->abb.configure(&vdd->abb);
       } else {
               pr_warning("%s: vdd_%s does not exisit in OMAP4\n",
                       __func__, vdd->voltdm.name);
@@ -1299,6 +1653,7 @@ int omap_voltage_scale_vdd(struct voltagedomain *voltdm,

       /* load notifier chain data */
       v_info.target_volt = target_volt;
+       v_info.curr_volt = vdd->curr_volt;
       v_info.vdd = vdd;

       srcu_notifier_call_chain(&vdd->volt_change_notify_chain,
diff --git a/arch/arm/plat-omap/include/plat/voltage.h b/arch/arm/plat-omap/include/plat/voltage.h
index af790bf..07997f7 100644
--- a/arch/arm/plat-omap/include/plat/voltage.h
+++ b/arch/arm/plat-omap/include/plat/voltage.h
@@ -235,8 +235,8 @@ struct omap_vdd_dep_info {
 * @irqstatus_mpu_offs : PRM_IRQSTATUS_MPU* register offset
 * @done_st_shift      : ABB_vdd_DONE_ST shift
 * @done_st_mask       : ABB_vdd_DONE_ST bit mask
+ * @nb                 : voltage transition notifier block
 * @configure          : boot-time configuration
- * @nb_handler         : voltage transition notification handler
 * @set_opp            : transition function called from nb_handler
 */
 struct omap_abb_info {
@@ -245,9 +245,8 @@ struct omap_abb_info {
       u8 irqstatus_mpu_offs;
       u8 done_st_shift;
       u32 done_st_mask;
+       struct notifier_block nb;
       int (*configure) (struct omap_abb_info *abb);
-       int (*nb_handler) (struct notifier_block *nb, unsigned long val,
-                       void *data);
       int (*set_opp) (struct omap_abb_info *abb, int opp_type);
 };

@@ -302,6 +301,7 @@ struct omap_vdd_info {
 */
 struct omap_volt_change_info {
       unsigned long target_volt;
+       unsigned long curr_volt;
       struct omap_vdd_info *vdd;
 };

