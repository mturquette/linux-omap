/*
 * ipu_pm.c
 *
 * IPU Power Management support functions for TI OMAP processors.
 *
 * Copyright (C) 2009-2010 Texas Instruments, Inc.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <generated/autoconf.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>

/* PwrMgmt Initialization */
#include <syslink/notify.h>
#include <syslink/notify_driver.h>
#include <syslink/notifydefs.h>
#include <syslink/notify_driverdefs.h>
#include <syslink/notify_ducatidriver.h>

/* Module headers */
#include "ipu_pm.h"

/* Power Management headers */
#include <linux/gpio.h>
#include <linux/semaphore.h>
#include <linux/jiffies.h>
#include <plat/omap_hwmod.h>
#include <plat/omap_device.h>
#include <plat/dma.h>
#include <plat/dmtimer.h>

/** ============================================================================
 *  Macros and types
 *  ============================================================================
 */

#define SYS_M3 2
#define APP_M3 3

union message_slicer pm_msg;

int timeout = 1000;

int pm_action_type;
int pm_resource_type;
int pm_notify_response;

int pm_gptimer_num;
int pm_gptimer_counter;

int pm_gpio_num;
int pm_gpio_counter;

int pm_sdmachan_num;
int pm_sdmachan_counter;
int pm_sdmachan_dummy;

int ch, ch_aux;
int return_val;
int ipu_timer_list[NUM_IPU_TIMERS] = {
	GP_TIMER_3,
	GP_TIMER_4,
	GP_TIMER_9,
	GP_TIMER_11};

struct omap_dm_timer *p_gpt;

/** ============================================================================
 *  Forward declarations of internal functions
 *  ============================================================================
 */

/* Function for get sdma channels from PRCM */
inline int ipu_pm_get_sdma_chan(int proc_id, unsigned rcb_num);

/* Function for get gptimers from PRCM */
inline int ipu_pm_get_gptimer(unsigned rcb_num);

/* Function for release sdma channels to PRCM */
inline void ipu_pm_rel_sdma_chan(unsigned rcb_num);

/* Function for release gptimer to PRCM */
inline void ipu_pm_rel_gptimer(unsigned rcb_num);

/** ============================================================================
 *  Globals
 *  ============================================================================
 */

/*
  Function for PM resources Callback
 *
 */
void ipu_pm_callback(short int procId,
							int eventNo,
							unsigned long args,
							int payload)
{
	struct rcb_block *rcb_p;
	/* Get the payload */
	pm_msg.whole = payload;
	/* Get pointer to the proper RCB */
	rcb_p = (struct rcb_block *)&rcb_table->rcb[pm_msg.fields.rcb_num];

	/* Get the type of resource and the actions required */
	pm_action_type = rcb_p->msg_type;
	pm_resource_type = rcb_p->sub_type;

	/* Request the resource to PRCM */
	switch (pm_resource_type) {
	case SDMA:
		if (pm_action_type == PM_REQUEST_RESOURCE) {
			return_val =
				ipu_pm_get_sdma_chan(procId,
					pm_msg.fields.rcb_num);
			if (return_val < 0) {
				printk(KERN_INFO "Error Requesting SDMA\n");
				/* Update payload */
				pm_msg.fields.msg_type = PM_REQUEST_FAIL;
				pm_msg.fields.parm = PM_INSUFFICIENT_CHANNELS;
				break;
			}
		}
		if (pm_action_type == PM_RELEASE_RESOURCE) {
			pm_sdmachan_num = rcb_p->num_chan;
			ipu_pm_rel_sdma_chan(pm_msg.fields.rcb_num);
		}
		break;
	case GP_TIMER:
		if (pm_action_type == PM_REQUEST_RESOURCE) {
			/* GP Timers 3,4,9 or 11 for Ducati M3 */
			pm_gptimer_num =
				ipu_pm_get_gptimer
				(pm_msg.fields.rcb_num);
			if (pm_gptimer_num < 0) {
				/* GPtimer for Ducati unavailable */
				printk(KERN_ERR "GPTIMER error allocating\n");
				/* Update the payload with the failure msg */
				pm_msg.fields.msg_type = PM_REQUEST_FAIL;
				pm_msg.fields.parm = PM_NO_GPTIMER;
				break;
			}
		}
		if (pm_action_type == PM_RELEASE_RESOURCE)
			ipu_pm_rel_gptimer(pm_msg.fields.rcb_num);
		break;
	case GP_IO:
		if (pm_action_type == PM_REQUEST_RESOURCE) {
			pm_gpio_num = rcb_p->fill9;
			if (!gpio_request(pm_gpio_num , "ducati-ss"))
				pm_gpio_counter++;
			else {
				printk(KERN_ERR " GPIO request error\n");
				/* Update the payload with the failure msg */
				pm_msg.fields.msg_type = PM_REQUEST_FAIL;
				pm_msg.fields.parm = PM_NO_GPIO;
			}
		}
		if (pm_action_type == PM_RELEASE_RESOURCE) {
			pm_gpio_num = rcb_p->fill9;
			gpio_free(pm_gpio_num);
			pm_gpio_counter--;
		}
		break;
	case I2C:
	case DUCATI:
	case IVA_HD:
	case ISS:
	default:
		printk(KERN_INFO "Unsupported resource\n");
		break;
	}

	/* Update the payload with the reply msg */
	pm_msg.fields.reply_flag = true;

	/* Update the payload before send */
	payload = pm_msg.whole;

	/* send the ACK to DUCATI*/
	return_val = notify_sendevent(
				platform_notifydrv_handle,
				SYS_M3,/*DUCATI_PROC*/
				PM_RESOURCE,/*PWR_MGMT_EVENT*/
				payload,
				false);
	if (return_val < 0)
		printk(KERN_INFO "ERROR SENDING PM EVENT\n");
}
EXPORT_SYMBOL(ipu_pm_callback);

/*
  Function for PM notifications Callback
 *
 */
void ipu_pm_notify_callback(short int procId,
							int eventNo,
							unsigned long args,
							int payload)
{
	/**
	 * Post semaphore based in eventType (payload);
	 */
	/* Get the payload */
	pm_msg.whole = payload;
	switch (pm_msg.fields.msg_subtype) {
	case PM_SUSPEND:
		up(pm_event[PM_SUSPEND].sem_handle);
		break;
	case PM_RESUME:
		up(pm_event[PM_RESUME].sem_handle);
		break;
	case PM_OTHER:
		up(pm_event[PM_OTHER].sem_handle);
		break;
	}
}
EXPORT_SYMBOL(ipu_pm_notify_callback);

/*
  Function for send PM Notifications
 *
 */
int ipu_pm_notifications(enum pm_event_type event_type)
{
	/**
	 * Function called by linux driver
	 * Recieves evenType: Suspend, Resume, others...
	 * Send event to Ducati;
	 * Pend semaphore based in event_type (payload);
	 * Return ACK to caller;
	 */
	int pm_ack = true;
	switch (event_type) {
	case PM_SUSPEND:
		pm_msg.fields.msg_type = PM_NOTIFICATIONS;
		pm_msg.fields.msg_subtype = PM_SUSPEND;
		pm_msg.fields.parm = PM_SUCCESS;
		/* send the ACK to DUCATI*/
		return_val = notify_sendevent(
				platform_notifydrv_handle,
				SYS_M3,/*DUCATI_PROC*/
				PM_NOTIFICATION,/*PWR_MGMT_EVENT*/
				(unsigned int)pm_msg.whole,
				false);
		if (return_val < 0)
			printk(KERN_INFO "ERROR SENDING PM EVENT\n");
		return_val = down_timeout(pm_event[PM_SUSPEND].sem_handle,
			msecs_to_jiffies(timeout));
		if (return_val < 0) {
			printk(KERN_INFO "Error Suspend\n");
			pm_ack = PM_FAILURE;
		} else
			pm_ack = pm_msg.fields.parm;
		break;
	case PM_RESUME:
		pm_msg.fields.msg_type = PM_NOTIFICATIONS;
		pm_msg.fields.msg_subtype = PM_RESUME;
		pm_msg.fields.parm = PM_SUCCESS;
		/* send the ACK to DUCATI*/
		return_val = notify_sendevent(
				platform_notifydrv_handle,
				SYS_M3,/*DUCATI_PROC*/
				PM_NOTIFICATION,/*PWR_MGMT_EVENT*/
				(unsigned int)pm_msg.whole,
				false);
		if (return_val < 0)
			printk(KERN_INFO "ERROR SENDING PM EVENT\n");
		return_val = down_timeout(pm_event[PM_RESUME].sem_handle,
			msecs_to_jiffies(timeout));
		if (return_val < 0) {
			printk(KERN_INFO "Error Suspend\n");
			pm_ack = PM_FAILURE;
		} else
			pm_ack = pm_msg.fields.parm;
		break;
	case PM_OTHER:
		pm_msg.fields.msg_type = PM_NOTIFICATIONS;
		pm_msg.fields.msg_subtype = PM_OTHER;
		pm_msg.fields.parm = PM_SUCCESS;
		/* send the ACK to DUCATI*/
		return_val = notify_sendevent(
				platform_notifydrv_handle,
				SYS_M3,/*DUCATI_PROC*/
				PM_NOTIFICATION,/*PWR_MGMT_EVENT*/
				(unsigned int)pm_msg.whole,
				false);
		if (return_val < 0)
			printk(KERN_INFO "ERROR SENDING PM EVENT\n");
		return_val = down_timeout(pm_event[PM_OTHER].sem_handle,
			msecs_to_jiffies(timeout));
		if (return_val < 0) {
			printk(KERN_INFO "Error Suspend\n");
			pm_ack = PM_FAILURE;
		} else
			pm_ack = pm_msg.fields.parm;
		break;
	}

	return pm_ack;
}
EXPORT_SYMBOL(ipu_pm_notifications);

/*
  Function for get sdma channels from PRCM
 *
 */
inline int ipu_pm_get_sdma_chan(int proc_id, unsigned rcb_num)
{
	struct rcb_block *rcb_p;

	if (WARN_ON((rcb_num < RCB_MIN) || (rcb_num > RCB_MAX)))
		return PM_FAILURE;
	/* Get pointer to the proper RCB */
	rcb_p = (struct rcb_block *)&rcb_table->rcb[rcb_num];

	/* Get numchannels from RCB */
	pm_sdmachan_num = rcb_p->num_chan;
	if (WARN_ON((pm_sdmachan_num <= 0) ||
			(pm_sdmachan_num > SDMA_CHANNELS_MAX)))
		return PM_FAILURE;

	for (ch = 0; ch < pm_sdmachan_num; ch++) {
		return_val = omap_request_dma(proc_id,
			"ducati-ss",
			NULL,
			NULL,
			&pm_sdmachan_dummy);
		if (return_val == 0) {
			pm_sdmachan_counter++;
			rcb_p->channels[ch] = (unsigned char)pm_sdmachan_dummy;
		} else
			goto clean_sdma;
	}
	return PM_SUCCESS;
clean_sdma:
	printk(KERN_INFO "Error Requesting SDMA\n");
	/*failure, need to free the chanels*/
	for (ch_aux = 0; ch_aux < ch; ch_aux++) {
		pm_sdmachan_dummy = (int)rcb_p->channels[ch_aux];
		omap_free_dma(pm_sdmachan_dummy);
		pm_sdmachan_counter--;
	}
	return PM_FAILURE;
}

/*
  Function for get gptimers from PRCM
 *
 */
inline int ipu_pm_get_gptimer(unsigned rcb_num)
{
	int pm_gp_num;
	struct rcb_block *rcb_p;

	if (WARN_ON((rcb_num < RCB_MIN) || (rcb_num > RCB_MAX)))
		return PM_FAILURE;
	/* Get pointer to the proper RCB */
	rcb_p = (struct rcb_block *)&rcb_table->rcb[rcb_num];

	for (pm_gp_num = 0; pm_gp_num < NUM_IPU_TIMERS; pm_gp_num++) {
		p_gpt = omap_dm_timer_request_specific
			(ipu_timer_list[pm_gp_num]);
		if (p_gpt != NULL)
			break;
	}
	if (p_gpt == NULL)
		return PM_FAILURE;
	else {
		/* Store the gptimer number and base address */
		rcb_p->fill9 = ipu_timer_list[pm_gp_num];
		rcb_p->mod_base_addr = (unsigned)p_gpt;
		pm_gptimer_counter++;
		return PM_SUCCESS;
	}
}

/*
  Function for release sdma channels to PRCM
 *
 */
inline void ipu_pm_rel_sdma_chan(unsigned rcb_num)
{
	struct rcb_block *rcb_p;

	if (WARN_ON((rcb_num < RCB_MIN) || (rcb_num > RCB_MAX))) {
		printk(KERN_INFO "Invalid RCB number\n");
		return;
	}
	/* Get pointer to the proper RCB */
	rcb_p = (struct rcb_block *)&rcb_table->rcb[rcb_num];

	pm_sdmachan_num = rcb_p->num_chan;
	for (ch = 0; ch < pm_sdmachan_num; ch++) {
		pm_sdmachan_dummy = (int)rcb_p->channels[ch];
		omap_free_dma(pm_sdmachan_dummy);
		pm_sdmachan_counter--;
	}
}

/*
  Function for release gptimer to PRCM
 *
 */
inline void ipu_pm_rel_gptimer(unsigned rcb_num)
{
	struct rcb_block *rcb_p;

	if (WARN_ON((rcb_num < RCB_MIN) || (rcb_num > RCB_MAX))) {
		printk(KERN_INFO "Invalid RCB number\n");
		return;
	}
	/* Get pointer to the proper RCB */
	rcb_p = (struct rcb_block *)&rcb_table->rcb[rcb_num];

	p_gpt = (struct omap_dm_timer *)rcb_p->mod_base_addr;
	if (p_gpt != NULL)
		omap_dm_timer_free(p_gpt);
	rcb_p->mod_base_addr = 0;
	pm_gptimer_counter--;
}

