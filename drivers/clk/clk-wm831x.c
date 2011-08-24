/*
 * WM831x clock control
 *
 * Copyright 2011 Wolfson Microelectronics PLC.
 *
 * Author: Mark Brown <broonie@opensource.wolfsonmicro.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/mfd/wm831x/core.h>

struct wm831x_clk {
	struct wm831x *wm831x;
	struct clk_hw xtal_hw;
	struct clk_hw fll_hw;
	struct clk_hw clkout_hw;
	bool xtal_ena;
};

static int wm831x_xtal_enable(struct clk_hw *hw)
{
	struct wm831x_clk *clkdata = container_of(hw, struct wm831x_clk,
						  xtal_hw);

	if (clkdata->xtal_ena)
		return 0;
	else
		return -EPERM;
}

static unsigned long wm831x_xtal_recalc_rate(struct clk_hw *hw)
{
	struct wm831x_clk *clkdata = container_of(hw, struct wm831x_clk,
						  xtal_hw);

	if (clkdata->xtal_ena)
		return 32768;
	else
		return 0;
}

static long wm831x_xtal_round_rate(struct clk_hw *hw, unsigned long rate)
{
	return wm831x_xtal_recalc_rate(hw);
}

static const struct clk_hw_ops wm831x_xtal_ops = {
	.enable = wm831x_xtal_enable,
	.recalc_rate = wm831x_xtal_recalc_rate,
	.round_rate = wm831x_xtal_round_rate,
};

static const unsigned long wm831x_fll_auto_rates[] = {
	 2048000,
	11289600,
	12000000,
	12288000,
	19200000,
	22579600,
	24000000,
	24576000,
};

static bool wm831x_fll_enabled(struct wm831x *wm831x)
{
	int ret;

	ret = wm831x_reg_read(wm831x, WM831X_FLL_CONTROL_1);
	if (ret < 0) {
		dev_err(wm831x->dev, "Unable to read FLL_CONTROL_1: %d\n",
			ret);
		return true;
	}

	return ret & WM831X_FLL_ENA;
}

static int wm831x_fll_prepare(struct clk_hw *hw)
{
	struct wm831x_clk *clkdata = container_of(hw, struct wm831x_clk,
						  fll_hw);
	struct wm831x *wm831x = clkdata->wm831x;
	int ret;

	ret = wm831x_set_bits(wm831x, WM831X_FLL_CONTROL_2,
			      WM831X_FLL_ENA, WM831X_FLL_ENA);
	if (ret != 0)
		dev_crit(wm831x->dev, "Failed to enable FLL: %d\n", ret);

	return ret;
}

static void wm831x_fll_unprepare(struct clk_hw *hw)
{
	struct wm831x_clk *clkdata = container_of(hw, struct wm831x_clk,
						  fll_hw);
	struct wm831x *wm831x = clkdata->wm831x;
	int ret;

	ret = wm831x_set_bits(wm831x, WM831X_FLL_CONTROL_2, WM831X_FLL_ENA, 0);
	if (ret != 0)
		dev_crit(wm831x->dev, "Failed to disaable FLL: %d\n", ret);
}

static unsigned long wm831x_fll_recalc_rate(struct clk_hw *hw)
{
	struct wm831x_clk *clkdata = container_of(hw, struct wm831x_clk,
						  fll_hw);
	struct wm831x *wm831x = clkdata->wm831x;
	int ret;

	ret = wm831x_reg_read(wm831x, WM831X_CLOCK_CONTROL_2);
	if (ret < 0) {
		dev_err(wm831x->dev, "Unable to read CLOCK_CONTROL_2: %d\n",
			ret);
		return 0;
	}

	if (ret & WM831X_FLL_AUTO)
		return wm831x_fll_auto_rates[ret & WM831X_FLL_AUTO_FREQ_MASK];

	dev_err(wm831x->dev, "FLL only supported in AUTO mode\n");
	return 0;
}

static int wm831x_fll_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long *parent_rate)
{
	struct wm831x_clk *clkdata = container_of(hw, struct wm831x_clk,
						  fll_hw);
	struct wm831x *wm831x = clkdata->wm831x;
	int i;

	for (i = 0; i < ARRAY_SIZE(wm831x_fll_auto_rates); i++)
		if (wm831x_fll_auto_rates[i] == rate)
			break;
	if (i == ARRAY_SIZE(wm831x_fll_auto_rates))
		return -EINVAL;

	if (wm831x_fll_enabled(wm831x))
		return -EPERM;

	return wm831x_set_bits(wm831x, WM831X_CLOCK_CONTROL_2,
			       WM831X_FLL_AUTO_FREQ_MASK, i);
}

static struct clk *wm831x_fll_get_parent(struct clk_hw *hw)
{
	struct wm831x_clk *clkdata = container_of(hw, struct wm831x_clk,
						  fll_hw);
	struct wm831x *wm831x = clkdata->wm831x;
	int ret;

	/* AUTO mode is always clocked from the crystal */
	ret = wm831x_reg_read(wm831x, WM831X_CLOCK_CONTROL_2);
	if (ret < 0) {
		dev_err(wm831x->dev, "Unable to read CLOCK_CONTROL_2: %d\n",
			ret);
		return NULL;
	}

	if (ret & WM831X_FLL_AUTO)
		return clkdata->xtal_hw.clk;

	ret = wm831x_reg_read(wm831x, WM831X_FLL_CONTROL_5);
	if (ret < 0) {
		dev_err(wm831x->dev, "Unable to read FLL_CONTROL_5: %d\n",
			ret);
		return NULL;
	}

	switch (ret & WM831X_FLL_CLK_SRC_MASK) {
	case 0:
		return clkdata->xtal_hw.clk;
	case 1:
		dev_warn(wm831x->dev,
			 "FLL clocked from CLKIN not yet supported\n");
		return NULL;
	default:
		dev_err(wm831x->dev, "Unsupported FLL clock source %d\n",
			ret & WM831X_FLL_CLK_SRC_MASK);
		return NULL;
	}
}

static const struct clk_hw_ops wm831x_fll_ops = {
	.prepare = wm831x_fll_prepare,
	.unprepare = wm831x_fll_unprepare,
	.recalc_rate = wm831x_fll_recalc_rate,
	.set_rate = wm831x_fll_set_rate,
	.get_parent = wm831x_fll_get_parent,
};

static int wm831x_clkout_prepare(struct clk_hw *hw)
{
	struct wm831x_clk *clkdata = container_of(hw, struct wm831x_clk,
						  clkout_hw);
	struct wm831x *wm831x = clkdata->wm831x;
	int ret;

	ret = wm831x_reg_unlock(wm831x);
	if (ret != 0) {
		dev_crit(wm831x->dev, "Failed to lock registers: %d\n", ret);
		return ret;
	}

	ret = wm831x_set_bits(wm831x, WM831X_CLOCK_CONTROL_1,
			      WM831X_CLKOUT_ENA, WM831X_CLKOUT_ENA);
	if (ret != 0)
		dev_crit(wm831x->dev, "Failed to enable CLKOUT: %d\n", ret);

	wm831x_reg_lock(wm831x);

	return ret;
}

static void wm831x_clkout_unprepare(struct clk_hw *hw)
{
	struct wm831x_clk *clkdata = container_of(hw, struct wm831x_clk,
						  clkout_hw);
	struct wm831x *wm831x = clkdata->wm831x;
	int ret;

	ret = wm831x_reg_unlock(wm831x);
	if (ret != 0) {
		dev_crit(wm831x->dev, "Failed to lock registers: %d\n", ret);
		return;
	}

	ret = wm831x_set_bits(wm831x, WM831X_CLOCK_CONTROL_1,
			      WM831X_CLKOUT_ENA, 0);
	if (ret != 0)
		dev_crit(wm831x->dev, "Failed to disable CLKOUT: %d\n", ret);

	wm831x_reg_lock(wm831x);
}

static unsigned long wm831x_clkout_recalc_rate(struct clk_hw *hw)
{
	return clk_get_rate(clk_get_parent(hw->clk));
}

static long wm831x_clkout_round_rate(struct clk_hw *hw, unsigned long rate)
{
	return clk_round_rate(clk_get_parent(hw->clk), rate);
}

static int wm831x_clkout_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long *parent_rate)
{
	*parent_rate = rate;
	return CLK_SET_RATE_PROPAGATE;
}

static struct clk *wm831x_clkout_get_parent(struct clk_hw *hw)
{
	struct wm831x_clk *clkdata = container_of(hw, struct wm831x_clk,
						  clkout_hw);
	struct wm831x *wm831x = clkdata->wm831x;
	int ret;

	ret = wm831x_reg_read(wm831x, WM831X_CLOCK_CONTROL_1);
	if (ret < 0) {
		dev_err(wm831x->dev, "Unable to read CLOCK_CONTROL_1: %d\n",
			ret);
		return NULL;
	}

	if (ret & WM831X_CLKOUT_SRC)
		return clkdata->xtal_hw.clk;
	else
		return clkdata->fll_hw.clk;
}

static const struct clk_hw_ops wm831x_clkout_ops = {
	.prepare = wm831x_clkout_prepare,
	.unprepare = wm831x_clkout_unprepare,
	.recalc_rate = wm831x_clkout_recalc_rate,
	.round_rate = wm831x_clkout_round_rate,
	.set_rate = wm831x_clkout_set_rate,
	.get_parent = wm831x_clkout_get_parent,
};

static __devinit int wm831x_clk_probe(struct platform_device *pdev)
{
	struct wm831x *wm831x = dev_get_drvdata(pdev->dev.parent);
	struct wm831x_clk *clkdata;
	int ret;

	clkdata = kzalloc(sizeof(*clkdata), GFP_KERNEL);
	if (!clkdata)
		return -ENOMEM;

	/* XTAL_ENA can only be set via OTP/InstantConfig so just read once */
	ret = wm831x_reg_read(wm831x, WM831X_CLOCK_CONTROL_2);
	if (ret < 0) {
		dev_err(wm831x->dev, "Unable to read CLOCK_CONTROL_2: %d\n",
			ret);
		goto err_alloc;
	}
	clkdata->xtal_ena = ret & WM831X_XTAL_ENA;

	if (!clk_register(wm831x->dev, &wm831x_xtal_ops, &clkdata->xtal_hw,
			  "xtal")) {
		ret = -EINVAL;
		goto err_alloc;
	}

	if (!clk_register(wm831x->dev, &wm831x_fll_ops, &clkdata->fll_hw,
			  "fll")) {
		ret = -EINVAL;
		goto err_xtal;
	}

	if (!clk_register(wm831x->dev, &wm831x_clkout_ops, &clkdata->clkout_hw,
			  "clkout")) {
		ret = -EINVAL;
		goto err_fll;
	}

	dev_set_drvdata(&pdev->dev, clkdata);

	return 0;

err_fll:
	clk_unregister(clkdata->fll_hw.clk);
err_xtal:
	clk_unregister(clkdata->xtal_hw.clk);
err_alloc:
	kfree(clkdata);
	return ret;
}

static __devexit int wm831x_clk_remove(struct platform_device *pdev)
{
	struct wm831x_clk *clkdata = dev_get_drvdata(&pdev->dev);

	clk_unregister(clkdata->clkout_hw.clk);
	clk_unregister(clkdata->fll_hw.clk);
	clk_unregister(clkdata->xtal_hw.clk);
	kfree(clkdata);

	return 0;
}

static struct platform_driver wm831x_clk_driver = {
	.probe = wm831x_clk_probe,
	.remove = __devexit_p(wm831x_clk_remove),
	.driver		= {
		.name	= "wm831x-clk",
		.owner	= THIS_MODULE,
	},
};

static int __init wm831x_clk_init(void)
{
	int ret;

	ret = platform_driver_register(&wm831x_clk_driver);
	if (ret != 0)
		pr_err("Failed to register WM831x clock driver: %d\n", ret);

	return ret;
}
module_init(wm831x_clk_init);

static void __exit wm831x_clk_exit(void)
{
	platform_driver_unregister(&wm831x_clk_driver);
}
module_exit(wm831x_clk_exit);

/* Module information */
MODULE_AUTHOR("Mark Brown <broonie@opensource.wolfsonmicro.com>");
MODULE_DESCRIPTION("WM831x clock driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:wm831x-clk");
