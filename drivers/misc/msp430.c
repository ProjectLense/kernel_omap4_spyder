/*
 * Copyright (C) 2010 Motorola, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

#include <linux/earlysuspend.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input-polldev.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>

#include <linux/msp430.h>
#include <linux/wakelock.h>

#ifdef  CONFIG_QUICK_WAKEUP
#include <linux/quickwakeup.h>
#include <linux/wakeup_timer_kernel.h>
#endif

#define NAME			     "msp430"

#define I2C_RETRY_DELAY			5
#define I2C_RETRIES			5
#define I2C_RESPONSE_LENGTH		8
#define MSP430_MAXDATA_LENGTH		250
#define MSP430_DELAY_USEC		10
#define MSP430_RESPONSE_MSG		0x3b
#define MSP430_RESPONSE_MSG_SUCCESS	0x00
#define MSP430_CRC_SEED			0xffff
#define MSP430_COMMAND_HEADER		0x80
#define MSP430_CRC_LENGTH		2
#define MSP430_OPCODE_LENGTH		1
#define MSP430_ADDRESS_LENGTH		3
#define MSP430_CORECOMMAND_LENGTH	(MSP430_OPCODE_LENGTH +\
					MSP430_MAXDATA_LENGTH +\
					MSP430_ADDRESS_LENGTH)
#define MSP430_HEADER_LENGTH		1
#define MSP430_CMDLENGTH_BYTES		2
#define G_MAX				0x7FFF
#define MSP430_CLIENT_MASK		0xF0

/* MSP430 memory map */
#define ID				0x00
#define REV_ID				0x01
#define ERROR_STATUS			0x02
#define STEP_LENGTH_WALK		0x03
#define STEP_LENGTH_RUN			0x04
#define HEIGHT				0x05
#define WEIGHT				0x06
#define GENDER_AGE			0x07
#define DISTANCE_THR			0x08
#define TOTAL_DISTANCE			0x09

#define TOTAL_STEP_COUNT		0x0A
#define STEP_COUNT_LAST_ACT		0x0B
#define STEP_COUNT_CURENT_ACT		0x0C
#define TIME_ACT_CHANGE			0x0D
#define SPEED				0x0E

#define AP_POSIX_TIME			0x10

#define PRESSURE_SEA_LEVEL		0x14

#define ACCEL_UPDATE_RATE		0x16
#define MAG_UPDATE_RATE			0x17
#define PRESSURE_UPDATE_RATE		0x18
#define GYRO_UPDATE_RATE		0x19
#define MODULE_CONFIG			0x1A 
#define GYRO_CONFIG			0x1B
#define ACCEL_CONFIG			0x1C
#define FF_THR				0x1D
#define FF_DUR				0x1E
#define MOTION_THR			0x1F
#define MOTION_DUR			0x20
#define ZRMOTION_THR			0x21
#define ZRMOTION_DUR			0x22
#define RINGBUFFER_EN			0x23
#define BYPASS_MODE			0x24
#define SLAVE_ADDRESS			0x25

#define ALGO_CONFIG			0x26
#define ALGO_INT_STATUS			0x27
#define RADIAL_THR			0x28
#define RADIAL_DUR			0x2A
#define RADIAL_NORTHING			0x2C
#define RADIAL_EASTING			0x2E

#define INTERRUPT_MASK			0x37
#define INTERRUPT_STATUS		0x3A

#define ACCEL_X				0x3B
#define ACCEL_Y				0x3D
#define ACCEL_Z				0x3F

#define TEMPERATURE_DATA		0x41

#define GYRO_X				0x43
#define GYRO_Y				0x45
#define GYRO_Z				0x47

#define MAG_HX				0x49
#define MAG_HY				0x4B
#define MAG_HZ				0x4D

#define DEVICE_ORIEN_HEAD		0x4F
#define DEVICE_ORIEN_PITCH		0x51
#define DEVICE_ORIEN_ROLL		0x53
#define CAL_STATUS        		0x55

#define CURRENT_PRESSURE		0x56

#define CURRENT_ALTITUDE		0x58

#define ACTIVITY_DETECTION		0x6A

#define POWER_MODE			0x6B

#define MOTION_DUR_1			0x71
#define ZRMOTION_DUR_1			0x72
#define MOTION_DUR_2			0x73
#define ZRMOTION_DUR_2			0x74
#define MOTION_DUR_3		       	0x75
#define ZRMOTION_DUR_3			0x76
#define MOTION_THR_1			0x77
#define ZRMOTION_THR_1			0x78
#define MOTION_THR_2			0x79
#define ZRMOTION_THR_2			0x7A
#define MOTION_THR_3			0x7B
#define ZRMOTION_THR_3			0x7C

#define RESET				0x7F

#define KDEBUG(format, s...)	if (g_debug)\
		pr_info(format, ##s)
static char g_debug;
/* to hold sensor state so that it can be restored on resume */
static unsigned char g_sensor_state;

static unsigned char msp_cmdbuff[MSP430_HEADER_LENGTH + MSP430_CMDLENGTH_BYTES +
			MSP430_CORECOMMAND_LENGTH + MSP430_CRC_LENGTH];

enum msp_mode {
	UNINITIALIZED,
	BOOTMODE,
	NORMALMODE,
	FACTORYMODE
};

struct msp430_pedometer_data {
	unsigned char  activity;
	unsigned short distance;
	unsigned short stepcount;
	unsigned short speed;
};

struct msp430_data {
	struct i2c_client *client;
	struct msp430_platform_data *pdata;
	/* to avoid two i2c communications at the same time */
	struct mutex lock;
	struct input_dev *input_dev;
	struct input_dev *input_dev_b;
	struct work_struct irq_work;
	struct workqueue_struct *irq_work_queue;
	struct work_struct passive_work;
	struct wake_lock wakelock;
	struct timer_cascade_root *waketimer;

	int hw_initialized;
	int acc_poll_interval;
	int motion_poll_interval;
	int env_poll_interval;
	int mag_poll_interval;
	int gyro_poll_interval;
	int monitor_poll_interval;
	int orin_poll_interval;
	atomic_t enabled;
	int irq;
	unsigned int current_addr;
	enum msp_mode mode;
	int in_activity ;
	struct msp430_pedometer_data prev_data;
	unsigned char intp_mask;
	struct early_suspend early_suspend;
};

enum msp_commands {
	PASSWORD_RESET,
	MASS_ERASE,
	PROGRAM_CODE,
	END_FIRMWARE,
	PASSWORD_RESET_DEFAULT
};

enum msp_opcode {
	PASSWORD_OPCODE = 0x11,
	MASSERASE_OPCODE = 0x15,
	RXDATA_OPCODE = 0x10,
};

enum msp_powermodes {
	POWER_NORMAL_MODE = 0x01,
	POWER_ANY_MOTION_MODE = 0x02,
	POWER_SLEEP_MODE = 0x03
};

/* Different activities detected */
enum msp_activities {
	STILL = 0x00,
	WALK = 0x01,
	RUN = 0x03
};

struct msp_response {

	/* 0x0080 */
	unsigned short header;
	unsigned char len_lsb;
	unsigned char len_msb;
	unsigned char cmd;
	unsigned char data;
	unsigned char crc_lsb;
	unsigned char crc_msb;
};

static const unsigned char msp_motion_thr_a[] = {
	MOTION_THR,
	MOTION_THR_1,
	MOTION_THR_2,
	MOTION_THR_3
};

static const unsigned char msp_motion_dur_a[] = {
	MOTION_DUR,
	MOTION_DUR_1,
	MOTION_DUR_2,
	MOTION_DUR_3
};

static const unsigned char msp_zrmotion_thr_a[] = {
	ZRMOTION_THR,
	ZRMOTION_THR_1,
	ZRMOTION_THR_2,
	ZRMOTION_THR_3
};

static const unsigned char msp_zrmotion_dur_a[] = {
	ZRMOTION_DUR,
	ZRMOTION_DUR_1,
	ZRMOTION_DUR_2,
	ZRMOTION_DUR_3
};

static const unsigned short crc_table[256] = {
  0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5,
  0x60c6, 0x70e7, 0x8108, 0x9129, 0xa14a, 0xb16b,
  0xc18c, 0xd1ad, 0xe1ce, 0xf1ef, 0x1231, 0x0210,
  0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
  0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c,
  0xf3ff, 0xe3de, 0x2462, 0x3443, 0x0420, 0x1401,
  0x64e6, 0x74c7, 0x44a4, 0x5485, 0xa56a, 0xb54b,
  0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
  0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6,
  0x5695, 0x46b4, 0xb75b, 0xa77a, 0x9719, 0x8738,
  0xf7df, 0xe7fe, 0xd79d, 0xc7bc, 0x48c4, 0x58e5,
  0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
  0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969,
  0xa90a, 0xb92b, 0x5af5, 0x4ad4, 0x7ab7, 0x6a96,
  0x1a71, 0x0a50, 0x3a33, 0x2a12, 0xdbfd, 0xcbdc,
  0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
  0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03,
  0x0c60, 0x1c41, 0xedae, 0xfd8f, 0xcdec, 0xddcd,
  0xad2a, 0xbd0b, 0x8d68, 0x9d49, 0x7e97, 0x6eb6,
  0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
  0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a,
  0x9f59, 0x8f78, 0x9188, 0x81a9, 0xb1ca, 0xa1eb,
  0xd10c, 0xc12d, 0xf14e, 0xe16f, 0x1080, 0x00a1,
  0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
  0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c,
  0xe37f, 0xf35e, 0x02b1, 0x1290, 0x22f3, 0x32d2,
  0x4235, 0x5214, 0x6277, 0x7256, 0xb5ea, 0xa5cb,
  0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
  0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447,
  0x5424, 0x4405, 0xa7db, 0xb7fa, 0x8799, 0x97b8,
  0xe75f, 0xf77e, 0xc71d, 0xd73c, 0x26d3, 0x36f2,
  0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
  0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9,
  0xb98a, 0xa9ab, 0x5844, 0x4865, 0x7806, 0x6827,
  0x18c0, 0x08e1, 0x3882, 0x28a3, 0xcb7d, 0xdb5c,
  0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
  0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0,
  0x2ab3, 0x3a92, 0xfd2e, 0xed0f, 0xdd6c, 0xcd4d,
  0xbdaa, 0xad8b, 0x9de8, 0x8dc9, 0x7c26, 0x6c07,
  0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
  0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba,
  0x8fd9, 0x9ff8, 0x6e17, 0x7e36, 0x4e55, 0x5e74,
  0x2e93, 0x3eb2, 0x0ed1, 0x1ef0
};

struct msp430_data *msp430_misc_data;

#ifdef CONFIG_HAS_EARLYSUSPEND
static void msp430_early_suspend(struct early_suspend *handler);
static void msp430_late_resume(struct early_suspend *handler);
#endif

static int msp430_i2c_write_read(struct msp430_data *ps_msp430, u8 *buf,
			int writelen, int readlen)
{
	int tries, err = 0;
	struct msp_response *response;
	struct i2c_msg msgs[] = {
		{
			.addr = ps_msp430->client->addr,
			.flags = ps_msp430->client->flags,
			.len = writelen,
			.buf = buf,
		},
		{
			.addr = ps_msp430->client->addr,
			.flags = ps_msp430->client->flags | I2C_M_RD,
			.len = readlen,
			.buf = buf,
		},
	};
	if (ps_msp430->mode == FACTORYMODE)
		return err;
	if (buf == NULL || writelen == 0 || readlen == 0)
		return -EFAULT;

	if (ps_msp430->mode == BOOTMODE) {
		KDEBUG("MSP430 In msp430_i2c_write_read\n");
		KDEBUG("MSP430 sending: ");
		for (tries = 0; tries < writelen; tries++)
			KDEBUG("MSP430 %02x", buf[tries]);
	}
	tries = 0;
	do {
		err = i2c_transfer(ps_msp430->client->adapter, msgs, 2);
		if (err != 2)
			msleep_interruptible(I2C_RETRY_DELAY);
	} while ((err != 2) && (++tries < I2C_RETRIES));
	if (err != 2) {
		dev_err(&ps_msp430->client->dev, "read transfer error\n");
		err = -EIO;
	} else {
		err = 0;
		KDEBUG("MSP430 Read from MSP: ");
		for (tries = 0; tries < readlen; tries++)
			KDEBUG("MSP430 %02x", buf[tries]);

		if (ps_msp430->mode == BOOTMODE) {
			response = (struct msp_response *) buf;
			if ((response->cmd == MSP430_RESPONSE_MSG &&
				response->data != MSP430_RESPONSE_MSG_SUCCESS)
				|| (response->cmd != MSP430_RESPONSE_MSG)) {
					pr_err("i2c cmd returned failure\n");
					err = -EIO;
			}
		}
	}
	return err;
}


static int msp430_i2c_read(struct msp430_data *ps_msp430, u8 *buf, int len)
{
	int tries, err = 0;

	if (ps_msp430->mode == FACTORYMODE)
		return err;
	if (buf == NULL || len == 0)
		return -EFAULT;
	tries = 0;
	do {
		err = i2c_master_recv(ps_msp430->client, buf, len);
		if (err < 0)
			msleep_interruptible(I2C_RETRY_DELAY);
	} while ((err < 0) && (++tries < I2C_RETRIES));
	if (err < 0) {
		dev_err(&ps_msp430->client->dev, "read transfer error\n");
		err = -EIO;
	} else {
		KDEBUG("Read was successsful: \n");
		for (tries = 0; tries < err ; tries++)
			KDEBUG("MSP430 %02x", buf[tries]);
	}
	return err;
}

static int msp430_i2c_write(struct msp430_data *ps_msp430, u8 * buf, int len)
{
	int err = 0;
	int tries = 0;

	if (ps_msp430->mode == FACTORYMODE)
		return err;

	tries = 0;
	do {
		err = i2c_master_send(ps_msp430->client, buf, len);
		if (err < 0)
			msleep_interruptible(I2C_RETRY_DELAY);
	} while ((err < 0) && (++tries < I2C_RETRIES));

	if (err < 0) {
		dev_err(&ps_msp430->client->dev, "msp430: write error\n");
		err = -EIO;
	} else {
		KDEBUG("MSP430 msp430 i2c write successful \n");
		err = 0;
	}
	return err;
}

static int msp430_hw_init(struct msp430_data *ps_msp430)
{
	int err = 0;
	KDEBUG("MSP430 in  msp430_hw_init\n");
	ps_msp430->hw_initialized = 1;
	return err;
}

static void msp430_device_power_off(struct msp430_data *ps_msp430)
{
	KDEBUG("MSP430  in msp430_device_power_off\n");
	if (ps_msp430->hw_initialized == 1) {
		if (ps_msp430->pdata->power_off)
			ps_msp430->pdata->power_off();
		ps_msp430->hw_initialized = 0;
	}
}

static int msp430_device_power_on(struct msp430_data *ps_msp430)
{
	int err = 0;
	KDEBUG("In msp430_device_power_on\n");
	if (ps_msp430->pdata->power_on) {
		err = ps_msp430->pdata->power_on();
		if (err < 0) {
			dev_err(&ps_msp430->client->dev,
				"power_on failed: %d\n", err);
			return err;
		}
	}
	if (!ps_msp430->hw_initialized) {
		err = msp430_hw_init(ps_msp430);
		if (err < 0) {
			msp430_device_power_off(ps_msp430);
			return err;
		}
	}
	return err;
}

static void msp430_report_pedometer_values(struct msp430_data *ps_msp430,
				struct msp430_pedometer_data *curr_data)
{
	unsigned int delta_stepcount;
	unsigned int delta_distance;

	delta_stepcount = curr_data->stepcount - ps_msp430->prev_data.stepcount;
	delta_distance = curr_data->distance - ps_msp430->prev_data.distance;

	ps_msp430->prev_data.stepcount = curr_data->stepcount;
	ps_msp430->prev_data.distance = curr_data->distance;
	ps_msp430->prev_data.activity = curr_data->activity;
	/* report stepcount */
	input_report_rel(ps_msp430->input_dev_b, REL_HWHEEL,
			delta_stepcount);
	/* report speed */
	input_report_rel(ps_msp430->input_dev_b, REL_WHEEL,
			curr_data->speed);
	/* report distance */
	input_report_rel(ps_msp430->input_dev_b, REL_DIAL,
			delta_distance);
	/* report activity */
	input_report_rel(ps_msp430->input_dev_b, REL_MISC,
			curr_data->activity);
	input_sync(ps_msp430->input_dev_b);
}

static void msp430_read_motion_data(struct msp430_data *ps_msp430,
		struct msp430_pedometer_data *pedo)
{

	int err;
	msp_cmdbuff[0] = ACTIVITY_DETECTION;
	err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 1);
	if (err < 0) {
		pr_err("MSP430 Reading from msp failed\n");
		return;
	}
	pedo->activity = msp_cmdbuff[0];
	msp_cmdbuff[0] = TOTAL_STEP_COUNT;
	err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 2);
	if (err < 0) {
		pr_err("MSP430 Reading from msp failed\n");
		return;
	}
	pedo->stepcount = (msp_cmdbuff[0] << 8) | (msp_cmdbuff[1]);
	msp_cmdbuff[0] = SPEED;
	err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 2);
	if (err < 0) {
		pr_err("MSP430 Reading from msp failed\n");
		return;
	}
	pedo->speed = (msp_cmdbuff[0] << 8) | (msp_cmdbuff[1]);

	msp_cmdbuff[0] = TOTAL_DISTANCE;
	err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 2);
	if (err < 0) {
		pr_err("MSP430 Reading from msp failed\n");
		return;
	}
	pedo->distance = (msp_cmdbuff[0] << 8) | msp_cmdbuff[1];
	msp430_report_pedometer_values(ps_msp430, pedo);

}

static irqreturn_t msp430_isr(int irq, void *dev)
{
	struct msp430_data *ps_msp430 = dev;
	queue_work(ps_msp430->irq_work_queue, &ps_msp430->irq_work);
	return IRQ_HANDLED;
}

static void msp430_irq_work_func(struct work_struct *work)
{
	int err;
	unsigned char irq_status, irq2_status;
	signed short xyz[7];
	struct msp430_data *ps_msp430 = container_of(work,
			struct msp430_data, irq_work);
	struct msp430_pedometer_data pedo;

	if (ps_msp430->mode == BOOTMODE)
		return;

	KDEBUG("MSP430 In msp430_irq_work_func\n");
	mutex_lock(&ps_msp430->lock);

	/* read interrupt mask register */
	msp_cmdbuff[0] = INTERRUPT_STATUS;
	err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 1);
	if (err < 0) {
		pr_err("MSP430 Reading from msp failed\n");
		goto EXIT;
	}
	irq_status = msp_cmdbuff[0];
	/* read algorithm interrupt status register */
	msp_cmdbuff[0] = ALGO_INT_STATUS;
	err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 1);
	if (err < 0) {
		pr_err("MSP430 Reading from msp failed\n");
		goto EXIT;
	}
	irq2_status = msp_cmdbuff[0];

	if (irq_status & M_ACCEL) {
		/* read accelerometer values from MSP */
		msp_cmdbuff[0] = ACCEL_X;
		err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 6);
		if (err < 0) {
			pr_err("MSP430 Reading from msp failed\n");
			goto EXIT;
		}
		xyz[0] =  (msp_cmdbuff[0] << 8) | msp_cmdbuff[1];
		xyz[1] =  (msp_cmdbuff[2] << 8) | msp_cmdbuff[3];
		xyz[2] =  (msp_cmdbuff[4] << 8) | msp_cmdbuff[5];
		input_report_abs(ps_msp430->input_dev, ABS_X, xyz[0]);
		input_report_abs(ps_msp430->input_dev, ABS_Y, xyz[1]);
		input_report_abs(ps_msp430->input_dev, ABS_Z, xyz[2]);
		input_sync(ps_msp430->input_dev);
		KDEBUG("Sending acc(x,y,z)values:x=%d,y=%d,z=%d\n",
			xyz[0], xyz[1], xyz[2]);
	}
	if (irq_status & M_ECOMPASS) {
		/*Read orientation values */
		msp_cmdbuff[0] = MAG_HX;
		err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 13);
		if (err < 0) {
			pr_err("MSP430 Reading from msp failed\n");
			goto EXIT;
		}
		xyz[0] =  (msp_cmdbuff[0] << 8) | msp_cmdbuff[1];
		xyz[1] =  (msp_cmdbuff[2] << 8) | msp_cmdbuff[3];
		xyz[2] =  (msp_cmdbuff[4] << 8) | msp_cmdbuff[5];
		xyz[3] =  (msp_cmdbuff[6] << 8) | msp_cmdbuff[7];
		xyz[4] =  (msp_cmdbuff[8] << 8) | msp_cmdbuff[9];
		xyz[5] =  (msp_cmdbuff[10] << 8) | msp_cmdbuff[11];
		xyz[6] =  msp_cmdbuff[12];
		input_report_abs(ps_msp430->input_dev, ABS_HAT0X, xyz[0]);
		input_report_abs(ps_msp430->input_dev, ABS_HAT0Y, xyz[1]);
		input_report_abs(ps_msp430->input_dev, ABS_BRAKE, xyz[2]);
		input_report_abs(ps_msp430->input_dev, ABS_RX, xyz[3]);
		input_report_abs(ps_msp430->input_dev, ABS_RY, xyz[4]);
		input_report_abs(ps_msp430->input_dev, ABS_RZ, xyz[5]);
		input_report_abs(ps_msp430->input_dev, ABS_RUDDER, xyz[6]);
		input_sync(ps_msp430->input_dev);
		KDEBUG("Sending mag(x,y,z)values:x=%d,y=%d,z=%d\n",
			xyz[0], xyz[1], xyz[2]);
		KDEBUG("Sending orient(x,y,z)values:x=%d,y=%d,z=%d,stat=%d\n",
		       xyz[3], xyz[4], xyz[5], xyz[6]);
	}
	if (irq_status & M_GYRO) {
		msp_cmdbuff[0] = GYRO_X;
		err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 6);
		if (err < 0) {
			pr_err("MSP430 Reading from msp failed\n");
			goto EXIT;
		}
		xyz[0] = (msp_cmdbuff[0] << 8) | msp_cmdbuff[1];
		xyz[1] = (msp_cmdbuff[2] << 8) | msp_cmdbuff[3];
		xyz[2] = (msp_cmdbuff[4] << 8) | msp_cmdbuff[5];

		input_report_rel(ps_msp430->input_dev, REL_RX, xyz[0]);
		input_report_rel(ps_msp430->input_dev, REL_RY, xyz[1]);
		input_report_rel(ps_msp430->input_dev, REL_RZ, xyz[2]);
		input_sync(ps_msp430->input_dev);
		KDEBUG("Sending gyro(x,y,z)values:x = %d,y =%d,z=%d\n",
			xyz[0], xyz[1], xyz[2]);
	}
	if (irq_status & M_ACTIVITY_CHANGE) {
		msp430_read_motion_data(ps_msp430, &pedo);
#ifdef  CONFIG_QUICK_WAKEUP
		if (ps_msp430->prev_data.activity == STILL) {
			KDEBUG("MSP430 detected STILL stop wakeup timer\n");
			wakeup_stop_status_timer(ps_msp430->waketimer);
		} else {
			KDEBUG("MSP430 Restart wakeup timer\n");
			wakeup_start_status_timer(ps_msp430->waketimer,
			ps_msp430->motion_poll_interval);
		}
#endif
	}
	if (irq_status & M_TEMPERATURE) {
		/*Read temperature value */
		msp_cmdbuff[0] = TEMPERATURE_DATA;
		err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 2);
		if (err < 0) {
			pr_err("MSP430 Reading from msp failed\n");
			goto EXIT;
		}
		xyz[0] = (msp_cmdbuff[0] << 8) | msp_cmdbuff[1];

		input_report_abs(ps_msp430->input_dev, ABS_THROTTLE, xyz[0]);
		input_sync(ps_msp430->input_dev);
		KDEBUG("MSP430 Sending temp(x)value: %d\n", xyz[0]);
	}
	if (irq2_status & M_MMOVEME) {
		/* Client recieving action will be upper 2 MSB of status */
		xyz[0] = (irq2_status & MSP430_CLIENT_MASK) | M_MMOVEME;
		input_report_rel(ps_msp430->input_dev_b, REL_RZ, xyz[0]);
		input_sync(ps_msp430->input_dev_b);
		KDEBUG("MSP430 Sending meaningful movement event\n");
	}
	if (irq2_status & M_NOMMOVE) {
		/* Client recieving action will be upper 2 MSB of status */
		xyz[0] = (irq2_status & MSP430_CLIENT_MASK) | M_NOMMOVE;
		input_report_rel(ps_msp430->input_dev_b, REL_RZ, xyz[0]);
		input_sync(ps_msp430->input_dev_b);
		KDEBUG("MSP430 Sending no meaningful movement event\n");
	}
	if (irq2_status & M_RADIAL) {
		msp_cmdbuff[0] = RADIAL_NORTHING;
		err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 4);
		if (err < 0) {
			pr_err("MSP430 Reading from msp failed\n");
			goto EXIT;
		}
		xyz[0] = (msp_cmdbuff[0] << 8) | msp_cmdbuff[1];
		xyz[1] = (msp_cmdbuff[2] << 8) | msp_cmdbuff[3];

		input_report_rel(ps_msp430->input_dev_b, REL_RX, xyz[0]);
		input_report_rel(ps_msp430->input_dev_b, REL_RY, xyz[1]);
		input_sync(ps_msp430->input_dev_b);
		KDEBUG("MSP430 Radial north:%d,east:%d", xyz[0], xyz[1]);
	}

EXIT:
	/* For now HAE needs events even if the activity is still */
	mutex_unlock(&ps_msp430->lock);
}

static void msp430_passive_work_func(struct work_struct *work)
{
	struct msp430_data *ps_msp430 = container_of(work,
		struct msp430_data, passive_work);
	struct msp430_pedometer_data curr;
	if (ps_msp430->mode == BOOTMODE)
		return;

	mutex_lock(&ps_msp430->lock);
	/* read and report current motion data */
	msp430_read_motion_data(ps_msp430, &curr);
	mutex_unlock(&ps_msp430->lock);
}

static int msp430_set_powermode(enum msp_powermodes mode,
			struct msp430_data *ps_msp430)
{
	int err = 0;
	msp_cmdbuff[0] = POWER_MODE;
	msp_cmdbuff[1] = mode;
	err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
	if (err < 0)
		pr_err("Unable to switch msp power mode\n");
	return err;
}

static int msp430_enable(struct msp430_data *ps_msp430)
{
	int err = 0;

	KDEBUG("MSP430 msp430_enable\n");
	if (!atomic_cmpxchg(&ps_msp430->enabled, 0, 1)) {
		err = msp430_device_power_on(ps_msp430);
		if (err < 0) {
			atomic_set(&ps_msp430->enabled, 0);
			pr_err("msp430_enable returned with %d\n" ,err);
			return err;
		}

	}

	return err;
}

static int msp430_misc_open(struct inode *inode, struct file *file)
{
	int err = 0;
	KDEBUG("MSP430 msp430_misc_open\n");

	err = nonseekable_open(inode, file);
	if (err < 0)
		return err;
	file->private_data = msp430_misc_data;

	err = msp430_enable(msp430_misc_data);

	return err;
}

unsigned short msp430_calculate_crc(unsigned char *data, size_t length)
{
	size_t count;
	unsigned int crc = MSP430_CRC_SEED;
	unsigned int temp;
	for (count = 0; count < length; ++count) {
		temp = (*data++ ^ (crc >> 8)) & 0xff;
		crc = crc_table[temp] ^ (crc << 8);
	}
	return (unsigned short)(crc);
}

/* the caller function is resposible to free mem allocated in this function. */
void msp430_build_command(enum msp_commands cmd,
		const char *inbuff, unsigned int *length)
{
	unsigned int index = 0, i, len = *length;
	unsigned short corecmdlen;
	unsigned short crc;
	struct msp430_data *ps_msp = msp430_misc_data;

	/*allocate for the msp command */
	msp_cmdbuff[index++] = MSP430_COMMAND_HEADER; /* header */
	switch (cmd) {
	case PASSWORD_RESET:
		msp_cmdbuff[index++] = 0x21; /* len LSB */
		msp_cmdbuff[index++] = 0x00; /* len MSB */
		msp_cmdbuff[index++] = PASSWORD_OPCODE; /* opcode */
		for (i = 4; i < 36; i++) /* followed by FFs */
			msp_cmdbuff[index++] = 0xff;
		msp_cmdbuff[index++] = 0x9E; /* CRC LSB */
		msp_cmdbuff[index++] = 0xE6; /* CRC MSB */
		break;
	case MASS_ERASE:
		msp_cmdbuff[index++] = 0x01; /* len LSB */
		msp_cmdbuff[index++] = 0x00; /* len MSB */
		msp_cmdbuff[index++] = MASSERASE_OPCODE; /* opcode */
		msp_cmdbuff[index++] = 0x64; /* crc LSB */
		msp_cmdbuff[index++] = 0xa3; /* crc MSB */
		break;
	case PROGRAM_CODE:
		/*code length */
		KDEBUG("MSP430 No of bytes got from user = %d", len);
		corecmdlen = len + MSP430_OPCODE_LENGTH + MSP430_ADDRESS_LENGTH;
		msp_cmdbuff[index++] =
			(unsigned char)(corecmdlen & 0xff); /* LSB len */
		msp_cmdbuff[index++] =
			(unsigned char)((corecmdlen >> 8) & 0xff);/*MSB len*/
		msp_cmdbuff[index++] = RXDATA_OPCODE; /* opcode */
		/* LSB of write addr on MSP */
		msp_cmdbuff[index++] =
			(unsigned char)(ps_msp->current_addr & 0xff);
		/* middle byte of write addr */
		msp_cmdbuff[index++] =
			(unsigned char)((ps_msp->current_addr >> 8) & 0xff);
		/* MSB of write addr on MSP */
		msp_cmdbuff[index++] =
			(unsigned char)((ps_msp->current_addr >> 16) & 0xff);
		/* copy data from user to kernel space */
		if (copy_from_user(msp_cmdbuff+index, inbuff, len)) {
			pr_err("MSP430 copy from user returned error\n");
			index = 0;
		} else {
			index += len; /*increment index with data len*/
			crc = msp430_calculate_crc(msp_cmdbuff+3, len+1+3);
			/* crc LSB */
			msp_cmdbuff[index++] = (unsigned char)(crc & 0xff);
			/* crc MSB */
			msp_cmdbuff[index++] = (unsigned char)((crc >> 8)&0xff);
		}
		break;
	case END_FIRMWARE:
		msp_cmdbuff[index++] = 0x06; /* len LSB */
		msp_cmdbuff[index++] = 0x00; /* len MSB */
		msp_cmdbuff[index++] = RXDATA_OPCODE; /* opcode */
		msp_cmdbuff[index++] = 0xfe;
		msp_cmdbuff[index++] = 0xff;
		msp_cmdbuff[index++] = 0x00;
		msp_cmdbuff[index++] = 0x00;
		msp_cmdbuff[index++] = 0x44;
		msp_cmdbuff[index++] = 0x89; /* crc LSB */
		msp_cmdbuff[index++] = 0xa7; /* crc MSB */
		break;
	case PASSWORD_RESET_DEFAULT:
		msp_cmdbuff[index++] = 0x21; /* len LSB */
		msp_cmdbuff[index++] = 0x00; /* len MSB */
		msp_cmdbuff[index++] = PASSWORD_OPCODE; /* opcode */
		for (i = 0; i < 32; i++) /* followed by 30 FFs */
			msp_cmdbuff[index++] = 0xff;
		msp_cmdbuff[index++] = 0x9e; /* CRC LSB */
		msp_cmdbuff[index++] = 0xE6; /* CRC MSB */
		break;
	default:
		pr_info("Invalid msp430 cmd \n");
		index = 0;
		break;
	}
	/*command length */
	*length = index;
}

static ssize_t msp430_misc_write(struct file *file, const char __user *buff,
				 size_t count, loff_t *ppos)
{
	int err = 0;
	struct msp430_data *ps_msp430;
	unsigned int len = (unsigned int)count;

	KDEBUG("MSP430 msp430_misc_write\n");
	ps_msp430 = msp430_misc_data;
	if (len > MSP430_MAXDATA_LENGTH || len == 0) {
		pr_err("Error packet size is more \
			than MSP430_MAXDATA_LENGTH or 0\n");
		err = -EINVAL;
		return err;
	}
	KDEBUG("MSP430 Leng = %d", len); /* debug */

	if (ps_msp430->mode == BOOTMODE) {
		KDEBUG("MSP430  msp430_misc_write: boot mode\n");
		/* build the msp430 command to program code */
		msp430_build_command(PROGRAM_CODE, buff, &len);
		err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff,
				 len, I2C_RESPONSE_LENGTH);
		/* increment the current MSP write addr by count */
		if (err == 0) {
			msp430_misc_data->current_addr += count;
			/* return the number of bytes successfully written */
			err = len;
		}
	} else {
		KDEBUG("MSP430 msp430_misc_write: normal mode\n");
		if (copy_from_user(msp_cmdbuff, buff, count)) {
			pr_err("MSP430 copy from user returned error\n");
			err = -EINVAL;
		}
		err = msp430_i2c_write(ps_msp430, msp_cmdbuff, count);
		if (err == 0)
			err = len;
	}
	return err;
}

/* gpio toggling to switch modes(boot mode,normal mode)on MSP430 */
void switch_msp430_mode(enum msp_mode mode)

{
        unsigned int test_pin_active_value =
		msp430_misc_data->pdata->test_pin_active_value;

	/* bootloader mode */
	if (mode == BOOTMODE) {
		KDEBUG("MSP430 toggling to switch to boot mode\n");
		gpio_set_value(msp430_misc_data->pdata->gpio_test,
				(test_pin_active_value));
		msleep_interruptible(I2C_RETRY_DELAY);
		gpio_set_value(msp430_misc_data->pdata->gpio_reset, 0);
		msleep_interruptible(I2C_RETRY_DELAY);
		gpio_set_value(msp430_misc_data->pdata->gpio_reset, 1);

	} else{
		/*normal mode */
		KDEBUG("MSP430 toggling to normal or factory mode\n");
		gpio_set_value(msp430_misc_data->pdata->gpio_test,
				!(test_pin_active_value));
		msleep_interruptible(I2C_RETRY_DELAY);
		gpio_set_value(msp430_misc_data->pdata->gpio_reset, 0);
		msleep_interruptible(I2C_RETRY_DELAY);
		gpio_set_value(msp430_misc_data->pdata->gpio_reset, 1);
	}
	msp430_misc_data->mode = mode;

}

static int msp430_get_version(struct msp430_data *ps_msp430)
{
	int err = 0;
	KDEBUG("MSP430 Switch to normal to get version\n");
	switch_msp430_mode(NORMALMODE);
	msleep_interruptible(I2C_RETRY_DELAY);
	KDEBUG("MSP430 MSP software version: ");
	msp_cmdbuff[0] = REV_ID;
	err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 1);
	if (err >= 0) {
		err = (int)msp_cmdbuff[0];
		pr_err("MSP430 %02x", msp_cmdbuff[0]);
	}
	return err;
}

static int msp430_bootloadermode(struct msp430_data *ps_msp430)
{
	int err = 0;
	unsigned int cmdlen = 0;
	/* switch MSP to bootloader mode */
	KDEBUG("MSP430 Switching to bootloader mode\n");
	switch_msp430_mode(BOOTMODE);
	/* send password reset command to unlock MSP	 */
	KDEBUG("MSP430 Password reset for reset vector\n");
	msp430_build_command(PASSWORD_RESET, NULL, &cmdlen);
	err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff,
				cmdlen, I2C_RESPONSE_LENGTH);
	/* password reset for reset vector failed */
	if (err < 0) {
		/* password for blank reset vector */
		msp430_build_command(PASSWORD_RESET_DEFAULT, NULL, &cmdlen);
		err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff,
			cmdlen, I2C_RESPONSE_LENGTH);
	}
	return err;
}

static int msp430_test_write_read(struct msp430_data *ps_msp430,
				void __user *argp)
{
	int err = 0;
	unsigned short readwritebyte;
	unsigned char reg;
	unsigned int bytecount;
	int i;
	if (copy_from_user(&readwritebyte, argp, sizeof(unsigned short))) {
		pr_err("copy from user returned error\n");
		return -EFAULT;
	}
	bytecount = (readwritebyte >> 8) & 0xff;
	reg = readwritebyte & 0xff;
	msp_cmdbuff[0] = reg;
	err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, bytecount);
	if (err < -1)
		pr_err("MSP430 Failed to write read\n");
	if (copy_to_user(argp, &bytecount, sizeof(unsigned short)))
		return -EFAULT;
	for (i = 0; i < bytecount; i++)
		KDEBUG("%02x ", msp_cmdbuff[i]);
	return err;
}

static int msp430_set_user_profile(struct msp430_data *ps_msp430,
				void __user *argp)
{
	int err = 0;
	struct msp430_user_profile user;
	KDEBUG("MSP430 Set user profile \n");
	if (copy_from_user(&user, argp, sizeof(user))) {
		pr_err("copy from user returned error\n");
		return -EFAULT;
	}
	msp_cmdbuff[0] = GENDER_AGE;
	msp_cmdbuff[1] = ((user.sex << 7) | user.age);
	err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
	if (err < -1)
		pr_err("MSP430 Failed write gender age\n");
	msp_cmdbuff[0] = HEIGHT;
	msp_cmdbuff[1] = user.height;
	err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
	if (err < -1)
		pr_err("MSP430 Failed write height\n");
	msp_cmdbuff[0] = WEIGHT;
	msp_cmdbuff[1] = user.weight;
	err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
	if (err < -1)
		pr_err("MSP430 Failed write weight\n");
	return err;
}

static int msp430_set_sea_level_pressure(struct msp430_data *ps_msp430,
				void __user *argp)
{
	int err = 0;
	short seaLevelPressure = 0;
	KDEBUG("MSP430 Setting sea level pressure: ");
	if (copy_from_user(&seaLevelPressure, argp, sizeof(seaLevelPressure))) {
		pr_err("copy from user returned error\n");
		return -EFAULT;
	}
	KDEBUG("MSP430 %d\n", seaLevelPressure);
	msp_cmdbuff[0] = PRESSURE_SEA_LEVEL;
	msp_cmdbuff[1] = (unsigned char)((seaLevelPressure >> 8) & 0xff);
	msp_cmdbuff[2] = (unsigned char)((seaLevelPressure) & 0xff);
	err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 3);
	if (err < -1)
		pr_err("MSP430 Failed to write sea level pressure\n");
	return err;
}

static int msp430_set_ref_altitude(struct msp430_data *ps_msp430,
				void __user *argp)
{
	int err = 0;
	short ref_altitude;
	KDEBUG("MSP430 Setting reference altitude: ");
	if (copy_from_user(&ref_altitude, argp, sizeof(ref_altitude))) {
		pr_err("copy from user returned error\n");
		return -EFAULT;
	}
	KDEBUG("MSP430 %d\n", ref_altitude);
	msp_cmdbuff[0] = CURRENT_ALTITUDE;
	msp_cmdbuff[1] = (unsigned char)((ref_altitude >> 8) & 0xff);
	msp_cmdbuff[2] = (unsigned char)((ref_altitude) & 0xff);
	err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 3);
	if (err < -1)
		pr_err("MSP430 Failed to write reference altitude\n");
	return err;
}

static long msp430_misc_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	int err = 0;
	unsigned int addr = 0;
	struct msp430_data *ps_msp430 = file->private_data;
	unsigned int cmdlen = 0;
	unsigned char byte;
	unsigned short delay;

	mutex_lock(&ps_msp430->lock);

	KDEBUG("MSP430 msp430_misc_ioctl = %d\n", cmd);
	switch (cmd) {
	case MSP430_IOCTL_GET_VERSION:
		err = msp430_get_version(ps_msp430);
		break;
	case MSP430_IOCTL_BOOTLOADERMODE:
		err = msp430_bootloadermode(ps_msp430);
		break;
	case MSP430_IOCTL_NORMALMODE:
		switch_msp430_mode(NORMALMODE);
		break;
	case MSP430_IOCTL_MASSERASE:
		msp430_build_command(MASS_ERASE, NULL, &cmdlen);
		err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff,
					cmdlen, I2C_RESPONSE_LENGTH);
		break;
	case MSP430_IOCTL_SETSTARTADDR:
		if (copy_from_user(&addr, argp, sizeof(addr))) {
			pr_err("copy from user returned error\n");
			return -EFAULT;
		}
		/* store the start addr */
		msp430_misc_data->current_addr = addr;
		break;
	case MSP430_IOCTL_TEST_READ:
		err = msp430_i2c_read(ps_msp430, &byte, 1);
		/* msp430 will return num of bytes read or error */
		if (err > 0)
			err = byte;
		break;
	case MSP430_IOCTL_TEST_WRITE:
		if (copy_from_user(&byte, argp, sizeof(unsigned char))) {
			pr_err("copy from user returned error\n");
			return -EFAULT;
		}
		err = msp430_i2c_write(ps_msp430, &byte, 1);
		break;
	case MSP430_IOCTL_TEST_WRITE_READ:
		err = msp430_test_write_read(ps_msp430, argp);
		break;

	case MSP430_IOCTL_SET_ACC_DELAY:
		delay = 0;
		if (copy_from_user(&delay, argp, sizeof(delay))) {
			KDEBUG("copy from user returned error\n");
			return -EFAULT;
		}
		ps_msp430->acc_poll_interval = (int)delay;
		msp_cmdbuff[0] = ACCEL_UPDATE_RATE;
		msp_cmdbuff[1] = ps_msp430->acc_poll_interval;
		err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
		break;

	case MSP430_IOCTL_SET_MAG_DELAY:
		delay = 0;
		if (copy_from_user(&delay, argp, sizeof(delay))) {
			KDEBUG("copy from user returned error\n");
			return -EFAULT;
		}
		ps_msp430->mag_poll_interval = (int)delay;
		msp_cmdbuff[0] = MAG_UPDATE_RATE;
		msp_cmdbuff[1] = ps_msp430->mag_poll_interval;
		err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
		break;
	case MSP430_IOCTL_SET_SENSORS:
		delay = 0;
		if (copy_from_user(&delay, argp, sizeof(delay))) {
			KDEBUG("copy from user returned error\n");
			return -EFAULT;
		}
		msp_cmdbuff[0] = MODULE_CONFIG;
		msp_cmdbuff[1] = delay;
		err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
		/* save sensor state any time this changes */
		g_sensor_state =  delay;
		break;
	case MSP430_IOCTL_GET_SENSORS:
		msp_cmdbuff[0] = MODULE_CONFIG;
		err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 1);
		if (err < 0) {
			pr_err("MSP430 Reading from msp failed\n");
			break;
		}
		delay = msp_cmdbuff[0];
		if (copy_to_user(argp, &delay, sizeof(delay)))
			return -EFAULT;
		break;
	case MSP430_IOCTL_SET_ALGOS:
		byte = 0;
		if (copy_from_user(&byte, argp, sizeof(byte))) {
			KDEBUG("copy from user returned error\n");
			return -EFAULT;
		}
		msp_cmdbuff[0] = ALGO_CONFIG;
		msp_cmdbuff[1] = byte;
		err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
		break;
	case MSP430_IOCTL_GET_ALGOS:
		msp_cmdbuff[0] = ALGO_CONFIG;
		err = msp430_i2c_write_read(ps_msp430, msp_cmdbuff, 1, 1);
		if (err < 0) {
			pr_err("MSP430 Reading from msp failed\n");
			break;
		}
		byte = msp_cmdbuff[0];
		if (copy_to_user(argp, &byte, sizeof(byte)))
			return -EFAULT;
		break;
	case MSP430_IOCTL_SET_RADIAL_THR:
		byte = 0;
		if (copy_from_user(&addr, argp, sizeof(addr))) {
			KDEBUG("copy from user returned error\n");
			return -EFAULT;
		}
		msp_cmdbuff[0] = RADIAL_THR;
		msp_cmdbuff[1] = addr >> 8;
		msp_cmdbuff[2] = addr & 0xFF;
		err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 3);
		break;
	case MSP430_IOCTL_SET_RADIAL_DUR:
		byte = 0;
		if (copy_from_user(&addr, argp, sizeof(addr))) {
			KDEBUG("copy from user returned error\n");
			return -EFAULT;
		}
		msp_cmdbuff[0] = RADIAL_DUR;
		msp_cmdbuff[1] = addr >> 8;
		msp_cmdbuff[2] = addr & 0xFF;
		err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 3);
		break;
	case MSP430_IOCTL_SET_MOTION_THR:
		if (copy_from_user(&addr, argp, sizeof(addr))) {
			KDEBUG("copy from user returned error\n");
			return -EFAULT;
		}
		byte = addr >> 8;
		if (byte < sizeof(msp_motion_thr_a))
			msp_cmdbuff[0] = msp_motion_thr_a[byte];
		else {
			KDEBUG("invalid arg passed in\n");
			return -EFAULT;
		}
		msp_cmdbuff[1] = addr & 0xFF;
		err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
		break;
	case MSP430_IOCTL_SET_MOTION_DUR:
		if (copy_from_user(&addr, argp, sizeof(addr))) {
			KDEBUG("copy from user returned error\n");
			return -EFAULT;
		}
		byte = addr >> 8;
		if (byte < sizeof(msp_motion_dur_a))
			msp_cmdbuff[0] = msp_motion_dur_a[byte];
		else {
			KDEBUG("invalid arg passed in\n");
			return -EFAULT;
		}
		msp_cmdbuff[1] = addr & 0xFF;
		err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
		break;
	case MSP430_IOCTL_SET_ZRMOTION_THR:
		if (copy_from_user(&addr, argp, sizeof(addr))) {
			KDEBUG("copy from user returned error\n");
			return -EFAULT;
		}
		byte = addr >> 8;
		if (byte < sizeof(msp_zrmotion_thr_a))
			msp_cmdbuff[0] = msp_zrmotion_thr_a[byte];
		else {
			KDEBUG("invalid arg passed in\n");
			return -EFAULT;
		}
		msp_cmdbuff[1] = addr & 0xFF;
		err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
		break;
	case MSP430_IOCTL_SET_ZRMOTION_DUR:
		if (copy_from_user(&addr, argp, sizeof(addr))) {
			KDEBUG("copy from user returned error\n");
			return -EFAULT;
		}
		byte = addr >> 8;
		if (byte < sizeof(msp_zrmotion_dur_a))
			msp_cmdbuff[0] = msp_zrmotion_dur_a[byte];
		else {
			KDEBUG("invalid arg passed in\n");
			return -EFAULT;
		}
		msp_cmdbuff[1] = addr & 0xFF;
		err = msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
		break;
	case MSP430_IOCTL_SET_FACTORY_MODE:
		KDEBUG("MSP430 set factory mode\n");
		switch_msp430_mode(FACTORYMODE);
		break;
	case MSP430_IOCTL_SET_PASSIVE_MODE:
		KDEBUG("MSP430 set passive mode\n");
		ps_msp430->in_activity = 0;
		break;
	case MSP430_IOCTL_SET_ACTIVE_MODE:
		KDEBUG("MSP430 set active mode\n");
		ps_msp430->in_activity = 1;
		break;
	case MSP430_IOCTL_TEST_BOOTMODE:
		/* switch MSP to bootloader mode */
		switch_msp430_mode(BOOTMODE);
		break;
	case MSP430_IOCTL_SET_POWER_MODE:
	  	err = msp430_set_powermode(POWER_NORMAL_MODE,ps_msp430);
	  	break;

	case MSP430_IOCTL_SET_DEBUG:
		/* enable or disble msp driver debug messages */
		if (copy_from_user(&g_debug, argp, sizeof(g_debug))) {
			KDEBUG("copy from user returned error\n");
			return -EFAULT;
		}
		break;

	case MSP430_IOCTL_SET_USER_PROFILE:
		err = msp430_set_user_profile(ps_msp430, argp);
		break;

	case MSP430_IOCTL_SET_SEA_LEVEL_PRESSURE:
		err = msp430_set_sea_level_pressure(ps_msp430, argp);
		break;

	case MSP430_IOCTL_SET_REF_ALTITUDE:
		err = msp430_set_ref_altitude(ps_msp430, argp);
		break;

	case MSP430_IOCTL_SET_DOCK_STATUS:
		if (copy_from_user(&byte, argp, sizeof(byte))) {
			pr_err("copy from user returned error doc status\n");
			return -EFAULT;
		}
		break;
	default:
		pr_err("MSP430 MSP430:Invalid ioctl command %d\n", cmd);
		err = -EINVAL;
	}

	mutex_unlock(&ps_msp430->lock);
	return err;
}

static const struct file_operations msp430_misc_fops = {
	.owner = THIS_MODULE,
	.open = msp430_misc_open,
	.unlocked_ioctl = msp430_misc_ioctl,
	.write = msp430_misc_write,
};

static struct miscdevice msp430_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = NAME,
	.fops = &msp430_misc_fops,
};


static int msp430_input_init(struct msp430_data *ps_msp430)
{
	int err;
	pr_info("MSP430 msp430_input_init\n");
	ps_msp430->input_dev = input_allocate_device();
	if (!ps_msp430->input_dev) {
		err = -ENOMEM;
		dev_err(&ps_msp430->client->dev,
			"input device allocate failed: %d\n", err);
		goto err0;
	}
	ps_msp430->input_dev_b = input_allocate_device();
	if (!ps_msp430->input_dev_b) {
		err = -ENOMEM;
		dev_err(&ps_msp430->client->dev,
			"input device allocate failed: %d\n", err);
		goto err1;
	}

	input_set_drvdata(ps_msp430->input_dev, ps_msp430);
	input_set_drvdata(ps_msp430->input_dev_b, ps_msp430);

	set_bit(EV_ABS, ps_msp430->input_dev->evbit);
	set_bit(EV_REL, ps_msp430->input_dev->evbit);

	set_bit(ABS_THROTTLE, ps_msp430->input_dev->absbit);

	set_bit(ABS_PRESSURE, ps_msp430->input_dev->absbit);
	set_bit(ABS_X, ps_msp430->input_dev->absbit);
	set_bit(ABS_Y, ps_msp430->input_dev->absbit);
	set_bit(ABS_Z, ps_msp430->input_dev->absbit);
	set_bit(ABS_HAT0X, ps_msp430->input_dev->absbit);
	set_bit(ABS_HAT0Y, ps_msp430->input_dev->absbit);
	set_bit(ABS_BRAKE, ps_msp430->input_dev->absbit);
	set_bit(ABS_RX, ps_msp430->input_dev->absbit);
	set_bit(ABS_RY, ps_msp430->input_dev->absbit);
	set_bit(ABS_RZ, ps_msp430->input_dev->absbit);
	set_bit(ABS_RUDDER, ps_msp430->input_dev->absbit);

	set_bit(REL_RX, ps_msp430->input_dev->relbit);
	set_bit(REL_RY, ps_msp430->input_dev->relbit);
	set_bit(REL_RZ, ps_msp430->input_dev->relbit);
	/* temp sensor can measure btw -40 degree C to +125 degree C */
	input_set_abs_params(ps_msp430->input_dev,
			ABS_THROTTLE, -12000, 395000, 0, 0);
	input_set_abs_params(ps_msp430->input_dev,
			ABS_PRESSURE, 0, 1000000, 0, 0);


	/* fuzz and flat */
	input_set_abs_params(ps_msp430->input_dev, ABS_X, -G_MAX, G_MAX, 0, 0);
	input_set_abs_params(ps_msp430->input_dev, ABS_Y, -G_MAX, G_MAX, 0, 0);
	input_set_abs_params(ps_msp430->input_dev, ABS_Z, -G_MAX, G_MAX, 0, 0);
	input_set_abs_params(ps_msp430->input_dev, ABS_HAT0X,
			-4096 , 4096, 0, 0);
	input_set_abs_params(ps_msp430->input_dev, ABS_HAT0Y,
			-4096, 4096, 0, 0);
	input_set_abs_params(ps_msp430->input_dev, ABS_BRAKE,
			-4096, 4096, 0, 0);
	/* Max range = 360 * 64 */
	input_set_abs_params(ps_msp430->input_dev, ABS_RX, 0, 23040, 0, 0);
	input_set_abs_params(ps_msp430->input_dev, ABS_RY, 0, 23040, 0, 0);
	input_set_abs_params(ps_msp430->input_dev, ABS_RZ, 0, 23040, 0, 0);
	input_set_abs_params(ps_msp430->input_dev, ABS_RUDDER, 0, 4, 0, 0);

	/* for dev B */
	set_bit(EV_ABS, ps_msp430->input_dev_b->evbit);
	set_bit(EV_REL, ps_msp430->input_dev_b->evbit);
	set_bit(REL_RZ, ps_msp430->input_dev_b->relbit);
	set_bit(REL_RX, ps_msp430->input_dev_b->relbit);
	set_bit(REL_RY, ps_msp430->input_dev_b->relbit);
	set_bit(ABS_VOLUME, ps_msp430->input_dev_b->absbit);
	set_bit(REL_HWHEEL, ps_msp430->input_dev_b->relbit);
	set_bit(REL_DIAL, ps_msp430->input_dev_b->relbit);
	set_bit(REL_WHEEL, ps_msp430->input_dev_b->relbit);
	set_bit(REL_MISC, ps_msp430->input_dev_b->relbit);
	set_bit(ABS_HAT3X, ps_msp430->input_dev_b->absbit);
	set_bit(ABS_HAT3Y, ps_msp430->input_dev_b->absbit);
	set_bit(ABS_HAT2X, ps_msp430->input_dev_b->absbit);
	set_bit(ABS_HAT2Y, ps_msp430->input_dev_b->absbit);

	input_set_abs_params(ps_msp430->input_dev_b,
			ABS_HAT3X, 0, 1000000, 0, 0);
	input_set_abs_params(ps_msp430->input_dev_b,
			ABS_HAT3Y, 0, 1000000, 0, 0);
	input_set_abs_params(ps_msp430->input_dev_b,
			ABS_HAT2X, 0, 1000000, 0, 0);
	input_set_abs_params(ps_msp430->input_dev_b,
			ABS_HAT2Y, 0, 1000000, 0, 0);
	input_set_abs_params(ps_msp430->input_dev_b,
			ABS_VOLUME, 0, 1000000, 0, 0);


	ps_msp430->input_dev->name = "msp430sensorprocessor";
	ps_msp430->input_dev_b->name = "msp430smartfusion";
	err = input_register_device(ps_msp430->input_dev);
	if (err) {
		dev_err(&ps_msp430->client->dev,
			"unable to register input polled device %s: %d\n",
			ps_msp430->input_dev->name, err);
		goto err2;
	}
	err = input_register_device(ps_msp430->input_dev_b);
	if (err) {
		dev_err(&ps_msp430->client->dev,
			"unable to register input polled device %s: %d\n",
			ps_msp430->input_dev_b->name, err);
		goto err3;
	}

	return 0;
err3:

	input_unregister_device(ps_msp430->input_dev);
	ps_msp430->input_dev = NULL;

err2:

	input_free_device(ps_msp430->input_dev_b);

err1:

	input_free_device(ps_msp430->input_dev);

err0:

	return err;

}

static void msp430_input_cleanup(struct msp430_data *ps_msp430)
{
	pr_err("MSP430 msp430_input_cleanup\n");
	input_unregister_device(ps_msp430->input_dev);
	input_unregister_device(ps_msp430->input_dev_b);
}

int waketimercallback(void)
{
	struct msp430_data *ps_msp430 = msp430_misc_data;
	KDEBUG("wake up msp430\n");
	queue_work(ps_msp430->irq_work_queue, &ps_msp430->passive_work);
#ifdef  CONFIG_QUICK_WAKEUP
	if (ps_msp430->prev_data.activity != STILL) {
		wakeup_start_status_timer(ps_msp430->waketimer,
			ps_msp430->motion_poll_interval);
	}
#endif
	return 0;
}

static int msp430_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct msp430_data *ps_msp430;
	int err = -1;
	dev_info(&client->dev, "msp430 probe begun\n");

	if (client->dev.platform_data == NULL) {
		dev_err(&client->dev, "platform data is NULL, exiting\n");
		err = -ENODEV;
		goto err0;
	}
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "client not i2c capable\n");
		err = -ENODEV;
		goto err0;
	}
	ps_msp430 = kzalloc(sizeof(*ps_msp430), GFP_KERNEL);
	if (ps_msp430 == NULL) {
		err = -ENOMEM;
		dev_err(&client->dev,
			"failed to allocate memory for module data: %d\n", err);
		goto err0;
	}

	mutex_init(&ps_msp430->lock);
	mutex_lock(&ps_msp430->lock);
	wake_lock_init(&ps_msp430->wakelock, WAKE_LOCK_SUSPEND, "msp430");
#ifdef  CONFIG_QUICK_WAKEUP
	ps_msp430->waketimer = wakeup_create_status_timer(waketimercallback);
#endif
	ps_msp430->client = client;
	ps_msp430->mode = UNINITIALIZED;
	ps_msp430->acc_poll_interval = 0;
	ps_msp430->mag_poll_interval = 0;
	ps_msp430->gyro_poll_interval = 0;
	ps_msp430->orin_poll_interval = 0;
	ps_msp430->env_poll_interval = 0;
	ps_msp430->motion_poll_interval = 0;
	ps_msp430->monitor_poll_interval = 0;

	/* initialize pedometer data */
	ps_msp430->prev_data.activity = -1;
	ps_msp430->prev_data.distance = 0;
	ps_msp430->prev_data.stepcount = 0;
	ps_msp430->prev_data.speed = 0;

	/* Set to passive mode by default */
	ps_msp430->in_activity = 0;
	g_debug = 0;
	g_sensor_state = 0;
	/* clear the interrupt mask */
	ps_msp430->intp_mask = 0x00;

	INIT_WORK(&ps_msp430->irq_work, msp430_irq_work_func);
	INIT_WORK(&ps_msp430->passive_work, msp430_passive_work_func);
	ps_msp430->irq_work_queue = create_singlethread_workqueue("msp430_wq");
	if (!ps_msp430->irq_work_queue) {
		err = -ENOMEM;
		dev_err(&client->dev, "cannot create work queue: %d\n", err);
		goto err1;
	}
	ps_msp430->pdata = kmalloc(sizeof(*ps_msp430->pdata), GFP_KERNEL);
	if (ps_msp430->pdata == NULL) {
		err = -ENOMEM;
		dev_err(&client->dev,
			"failed to allocate memory for pdata: %d\n", err);
		goto err2;
	}
	memcpy(ps_msp430->pdata, client->dev.platform_data,
		sizeof(*ps_msp430->pdata));
	i2c_set_clientdata(client, ps_msp430);
	ps_msp430->client->flags &= 0x00;

	if (ps_msp430->pdata->init) {
		err = ps_msp430->pdata->init();
		if (err < 0) {
			dev_err(&client->dev, "init failed: %d\n", err);
			goto err3;
		}
	}

	/*configure interrupt*/
	ps_msp430->irq = gpio_to_irq(ps_msp430->pdata->gpio_int);

	err = msp430_device_power_on(ps_msp430);
	if (err < 0) {
		dev_err(&client->dev, "power on failed: %d\n", err);
		goto err4;
	}
	enable_irq_wake(ps_msp430->irq);
	atomic_set(&ps_msp430->enabled, 1);

	err = msp430_input_init(ps_msp430);
	if (err < 0)
		goto err5;

	msp430_misc_data = ps_msp430;
	err = misc_register(&msp430_misc_device);
	if (err < 0) {
		dev_err(&client->dev, "misc register failed: %d\n", err);
		goto err6;
	}

	msp430_device_power_off(ps_msp430);

	atomic_set(&ps_msp430->enabled, 0);

	err = request_irq(ps_msp430->irq, msp430_isr, IRQF_TRIGGER_RISING,
		"msp430_irq", ps_msp430);
	if (err < 0) {
		dev_err(&client->dev, "request irq failed: %d\n", err);
		goto err7;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	ps_msp430->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	ps_msp430->early_suspend.suspend = msp430_early_suspend;
	ps_msp430->early_suspend.resume = msp430_late_resume;
	register_early_suspend(&ps_msp430->early_suspend);
#endif

	mutex_unlock(&ps_msp430->lock);

	dev_info(&client->dev, "msp430 probed\n");

	return 0;

err7:
	misc_deregister(&msp430_misc_device);
err6:
	msp430_input_cleanup(ps_msp430);
err5:
	msp430_device_power_off(ps_msp430);
err4:
	if (ps_msp430->pdata->exit)
		ps_msp430->pdata->exit();
err3:
	kfree(ps_msp430->pdata);
err2:
	destroy_workqueue(ps_msp430->irq_work_queue);
err1:
	mutex_unlock(&ps_msp430->lock);
	mutex_destroy(&ps_msp430->lock);
	wake_lock_destroy(&ps_msp430->wakelock);
	kfree(ps_msp430);
err0:
	return err;
}

static int __devexit msp430_remove(struct i2c_client *client)
{
	struct msp430_data *ps_msp430 = i2c_get_clientdata(client);
	pr_err("MSP430 msp430_remove\n");
	free_irq(ps_msp430->irq, ps_msp430);
	misc_deregister(&msp430_misc_device);
	msp430_input_cleanup(ps_msp430);
	msp430_device_power_off(ps_msp430);
	if (ps_msp430->pdata->exit)
		ps_msp430->pdata->exit();
	kfree(ps_msp430->pdata);
	destroy_workqueue(ps_msp430->irq_work_queue);
	mutex_destroy(&ps_msp430->lock);
	wake_lock_destroy(&ps_msp430->wakelock);
#ifdef  CONFIG_QUICK_WAKEUP
	wakeup_del_status_timer(ps_msp430->waketimer);
#endif
	disable_irq_wake(ps_msp430->irq);
#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&ps_msp430->early_suspend);
#endif
	kfree(ps_msp430);

	return 0;
}

static int msp430_resume(struct i2c_client *client)
{
	struct msp430_data *ps_msp430 = i2c_get_clientdata(client);
	KDEBUG("MSP430 msp430_resume\n");
	mutex_lock(&ps_msp430->lock);
	if ((ps_msp430->mode == NORMALMODE) && (g_sensor_state != 0)) {
		/* restore traditional sensor data */
		msp_cmdbuff[0] = MODULE_CONFIG;
		msp_cmdbuff[1] = g_sensor_state;
		msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
	}

	mutex_unlock(&ps_msp430->lock);
	return 0;
}

static int msp430_suspend(struct i2c_client *client, pm_message_t mesg)
{
	struct msp430_data *ps_msp430 = i2c_get_clientdata(client);
	KDEBUG("MSP430 msp430_suspend\n");
	mutex_lock(&ps_msp430->lock);
	if ((ps_msp430->mode == NORMALMODE) && (g_sensor_state != 0)) {
		/* turn off traditional sensor data */
		msp_cmdbuff[0] = MODULE_CONFIG;
		msp_cmdbuff[1] = 0;
		msp430_i2c_write(ps_msp430, msp_cmdbuff, 2);
	}

	mutex_unlock(&ps_msp430->lock);
	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void msp430_early_suspend(struct early_suspend *handler)
{
	struct msp430_data *ps_msp430;

	ps_msp430 = container_of(handler, struct msp430_data, early_suspend);
	msp430_suspend(ps_msp430->client, PMSG_SUSPEND);
}

static void msp430_late_resume(struct early_suspend *handler)
{
	struct msp430_data *ps_msp430;

	ps_msp430 = container_of(handler, struct msp430_data, early_suspend);
	msp430_resume(ps_msp430->client);
}
#endif

static const struct i2c_device_id msp430_id[] = {
	{NAME, 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, msp430_id);

static struct i2c_driver msp430_driver = {
	.driver = {
		   .name = NAME,
		   },
	.probe = msp430_probe,
	.remove = __devexit_p(msp430_remove),
#ifndef CONFIG_HAS_EARLYSUSPEND
	.resume = msp430_resume,
	.suspend = msp430_suspend,
#endif
	.id_table = msp430_id,
};

static int __init msp430_init(void)
{
	pr_info(KERN_ERR "MSP430 msp430 sensor processor\n");
	return i2c_add_driver(&msp430_driver);
}

static void __exit msp430_exit(void)
{
	pr_err("MSP430 msp430_exit\n");
	i2c_del_driver(&msp430_driver);
	return;
}

module_init(msp430_init);
module_exit(msp430_exit);

MODULE_DESCRIPTION("MSP430 sensor processor");
MODULE_AUTHOR("Motorola");
MODULE_LICENSE("GPL");
