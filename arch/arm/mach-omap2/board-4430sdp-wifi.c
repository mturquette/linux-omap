/*
 * Board support file for containing WiFi specific details for OMAP4430 SDP.
 *
 * Copyright (C) 2009 Texas Instruments
 *
 * Author: Pradeep Gurumath <pradeepgurumath@ti.com>
 *
 * Based on mach-omap2/board-3430sdp.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/* linux/arch/arm/mach-omap2/board-4430sdp-wifi.c
*/

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/err.h>

#include <asm/gpio.h>
#include <asm/io.h>
#include <linux/wifi_tiwlan.h>

#include "mux.h"

#define SDP4430_WIFI_PMENA_GPIO	 54
#define SDP4430_WIFI_IRQ_GPIO	 53

static int sdp4430_wifi_cd;		/* WIFI virtual 'card detect' status */
static void (*wifi_status_cb)(int card_present, void *dev_id);
static void *wifi_status_cb_devid;

void omap_wifi_mux_init(void) {
	/* primary interrupt from WiLink chip to OMAP */
	omap_mux_init_gpio(53, (OMAP_PULL_ENA | OMAP_PULL_UP |
				OMAP_WAKEUP_EN | OMAP_INPUT_EN));
}

int omap_wifi_status_register(void (*callback)(int card_present,
						void *dev_id), void *dev_id)
{
	if (wifi_status_cb)
		return -EAGAIN;
	wifi_status_cb = callback;

	wifi_status_cb_devid = dev_id;

	return 0;
}

int omap_wifi_status(struct device *dev, int slot)
{
	return sdp4430_wifi_cd;
}

int sdp4430_wifi_set_carddetect(int val)
{
	printk(KERN_WARNING"%s: %d\n", __func__, val);
	sdp4430_wifi_cd = val;
	if (wifi_status_cb)
		wifi_status_cb(val, wifi_status_cb_devid);
	else
		printk(KERN_WARNING "%s: Nobody to notify\n", __func__);
	return 0;
}
#ifndef CONFIG_WIFI_CONTROL_FUNC
EXPORT_SYMBOL(sdp4430_wifi_set_carddetect);
#endif

static int sdp4430_wifi_power_state;

int sdp4430_wifi_power(int on)
{
	printk(KERN_WARNING"%s: %d\n", __func__, on);
	gpio_set_value(SDP4430_WIFI_PMENA_GPIO, on);
	sdp4430_wifi_power_state = on;
	return 0;
}
#ifndef CONFIG_WIFI_CONTROL_FUNC
EXPORT_SYMBOL(sdp4430_wifi_power);
#endif

static int sdp4430_wifi_reset_state;
int sdp4430_wifi_reset(int on)
{
	printk(KERN_WARNING"%s: %d\n", __func__, on);
	sdp4430_wifi_reset_state = on;
	return 0;
}
#ifndef CONFIG_WIFI_CONTROL_FUNC
EXPORT_SYMBOL(sdp4430_wifi_reset);
#endif

struct wifi_platform_data sdp4430_wifi_control = {
	.set_power	= sdp4430_wifi_power,
	.set_reset	= sdp4430_wifi_reset,
	.set_carddetect	= sdp4430_wifi_set_carddetect,
};

#ifdef CONFIG_WIFI_CONTROL_FUNC
static struct resource sdp4430_wifi_resources[] = {
	[0] = {
		.name		= "device_wifi_irq",
		.start		= OMAP_GPIO_IRQ(SDP4430_WIFI_IRQ_GPIO),
		.end		= OMAP_GPIO_IRQ(SDP4430_WIFI_IRQ_GPIO),
		.flags          = IORESOURCE_IRQ | IORESOURCE_IRQ_LOWEDGE,
	},
};

static struct platform_device sdp4430_wifi_device = {
	.name           = "device_wifi",
	.id             = 1,
	.num_resources  = ARRAY_SIZE(sdp4430_wifi_resources),
	.resource       = sdp4430_wifi_resources,
	.dev            = {
		.platform_data = &sdp4430_wifi_control,
	},
};
#endif

static int __init sdp4430_wifi_init(void)
{
	int ret;

	omap_wifi_mux_init();
	printk(KERN_WARNING"%s: start\n", __func__);
	ret = gpio_request(SDP4430_WIFI_IRQ_GPIO, "wifi_irq");
	if (ret < 0) {
		printk(KERN_ERR "%s: can't reserve GPIO: %d\n", __func__,
			SDP4430_WIFI_IRQ_GPIO);
		goto out;
	}
	gpio_direction_input(SDP4430_WIFI_IRQ_GPIO);
#ifdef CONFIG_WIFI_CONTROL_FUNC
	ret = platform_device_register(&sdp4430_wifi_device);
#endif
out:
	return ret;
}

device_initcall(sdp4430_wifi_init);
