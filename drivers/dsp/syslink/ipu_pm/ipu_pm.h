/*
* ipu_pm.h
*
* Syslink IPU Power Managament support functions for TI OMAP processors.
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
*
*  --------------------------------------------------------------------
* | rcb_num                             |      |        |       |      |
* | msg_type                            |      |        |       |      |
* | sub_type                            |      |        |       |      |
* | rqst_cpu                            |    1-|word    |       |      |
* | extd_mem_flag                       |      |        |       |      |
* | num_chan                            |      |        |       |      |
* | fill9                               |      |        |       |      |
* |-------------------------------------|    ------  4-words 4-words   |
* | process_id                          |    1-word     |       |      |
* |-------------------------------------|    ------     |       |      |
* | sem_hnd                             |    1-word     |       |      |
* |-------------------------------------|    ------     |       |      |
* | mod_base_addr                       |    1-word     |       |      |
* |-------------------------------------|    ------   -----   -----    |
* | channels[0]  | data[0] | datax[0]   |      |        |       |      |
* | channels[1]  |         |            |    1-word     |       |   RCB_SIZE
* | channels[2]  |         |            |      |        |       |      =
* | channels[3]  |         |            |      |        |       |    8WORDS
* |--------------|---------|------------|    ------     |       |      |
* | channels[4]  | data[0] | datax[1]   |      |        |       |      |
* | channels[5]  |         |            |    1-word     |  RCB_SIZE-5  |
* | channels[6]  |         |            |      |        |       |      |
* | channels[7]  |         |            |      |     RCB_SIZE-4 |      |
* |--------------|---------|------------|    ------     |       |      |
* | channels[8]  | data[0] | datax[2]   |      |        |       |      |
* | channels[9]  |         |            |    1-word     |       |      |
* | channels[10] |         |            |      |        |       |      |
* | channels[11] |         |            |      |        |       |      |
* |--------------|---------|------------|    ------     |     -----    |
* | channels[12] | data[0] |extd_mem_hnd|      |        |       |      |
* | channels[13] |         |            |    1-word     |     1-word   |
* | channels[14] |         |            |      |        |       |      |
* | channels[15] |         |            |      |        |       |      |
*  --------------------------------------------------------------------
*
*The Ducati Power Management sub-system uses a structure called RCB_struct or
*just RCB to share information with the MPU about a particular resource involved
*in the communication. The information stored in this structure is needed to get
*attributes and other useful data about the resource.
*The fisrt fields of the RCB resemble the Rcb message sent across the NotifyDver
*It retains the rcb_num, msg_type and msg_subtype from the rcb message as its
*first 3 fields. The rqst_cpu fields indicates which remote processor originates
*the request/release petition. When a particular resource is requested, some of
*its parameters should be specify.
*For devices like Gptimer and GPIO, the most significant attribute its itemID.
*This value should be placed in the "fill9" field of the Rcb sruct. This field
*should be fill by the requester if asking for a particular resource or by the
*receiver if the resource granted is other than the one asked.
*
*Other variables related with the resource are:
*"sem_hnd" which storage the semaphore handle associated in the ducati side.
*We are pending on this semaphore when asked for the resource and
*posted when granted.
*"mod_base_addr". It is the virtual base addres for the resource.
*"process_id". It is the Task Id where the petition for the resource was called.
*
*The last 16 bytes of the structure could be interpreted in 3 different ways
*according to the context.
*1) For the case of the Rcb is for SDMA. The last 16 bytes correspond to a array
*  of 16 channels[ ]. Each entry has the number of the SDMA channel granted.
*  As many number of channels indicated in num_chan as many are meaningful
*  in the channels[] array.
*2) If the extd_mem_flag bit is NOT set the 16 last bytes are used as a data[]
*  array. Each entry is 4bytes long so the maximum number of entries is 4.
*3) If the extd_mem_flag bit is NOT set the 16 last bytes are used as an array
*  datax[ ]  3 members Each entry  4bytes long  and one additional field of
*  "extd_mem_hnd" which is a pointer to the continuation of this datax array
*/

#ifndef _IPU_PM_H_
#define _IPU_PM_H_

#include <linux/types.h>

/* Pm notify ducati driver */
/* Suspend/resume/other... */
#define NUMBER_PM_EVENTS 3

#define RCB_SIZE 8

#define DATA_MAX (RCB_SIZE - 4)
#define DATAX_MAX (RCB_SIZE - 5)
#define SDMA_CHANNELS_MAX 16

#define GP_TIMER_3 3
#define GP_TIMER_4 4
#define GP_TIMER_9 9
#define GP_TIMER_11 11
#define NUM_IPU_TIMERS 4

#define RCB_MIN 1
#define RCB_MAX 33

#define PM_RESOURCE 19
#define PM_NOTIFICATION 20
#define PM_SUCCESS 0
#define PM_FAILURE -1
#define PM_SHM_BASE_ADDR 0x9cff0000

enum pm_failure_codes{
	PM_INSUFFICIENT_CHANNELS = 1,
	PM_NO_GPTIMER,
	PM_NO_GPIO
};

enum pm_msgtype_codes{PM_NULLMSG,
	PM_ACKNOWLEDGEMENT,
	PM_REQUEST_RESOURCE,
	PM_RELEASE_RESOURCE,
	PM_REQUEST_FAIL,
	PM_RELEASE_FAIL,
	PM_NOTIFICATIONS
};

enum res_type{
	DUCATI = 0,
	IVA_HD,
	ISS,
	SDMA,
	GP_TIMER,
	GP_IO,
	I2C
};

enum pm_event_type{PM_SUSPEND,
	PM_RESUME,
	PM_OTHER
};

struct rcb_message {
	unsigned rcb_flag:1;
	unsigned rcb_num:6;
	unsigned reply_flag:1;
	unsigned msg_type:4;
	unsigned msg_subtype:4;
	unsigned parm:16;
};

union message_slicer {
	struct rcb_message fields;
	int whole;
};


struct rcb_block {
	unsigned rcb_num:6;
	unsigned msg_type:4;
	unsigned sub_type:4;
	unsigned rqst_cpu:4;
	unsigned extd_mem_flag:1;
	unsigned num_chan:4;
	unsigned fill9:9;

	unsigned process_id;
	unsigned *sem_hnd;
	unsigned mod_base_addr;
	union {
		unsigned int data[DATA_MAX];
		struct {
			unsigned datax[DATAX_MAX];
			unsigned extd_mem_hnd;
		};
		unsigned char channels[SDMA_CHANNELS_MAX];
	};

};

extern struct sms *rcb_table;
extern void *platform_notifydrv_handle;
extern struct pm_event *pm_event;

struct sms {
	unsigned rat;
	struct rcb_block rcb[RCB_MAX];
};

struct pm_event {
	enum pm_event_type event_type;
	struct semaphore *sem_handle;
};

/* Function for PM resources Callback */
void ipu_pm_callback(short int procId,
			int eventNo,
			unsigned long args,
			int payload);

/* Function for PM notifications Callback */
void ipu_pm_notify_callback(short int procId,
			int eventNo,
			unsigned long args,
			int payload);

/* Function for send PM Notifications */
int ipu_pm_notifications(enum pm_event_type event_type);

#endif

