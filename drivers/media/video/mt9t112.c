/*
 * mt9t112 Camera Driver
 *
 * Copyright (C) 2009 Renesas Solutions Corp.
 * Kuninori Morimoto <morimoto.kuninori@renesas.com>
 *
 * Based on ov772x driver, mt9m111 driver,
 *
 * Copyright (C) 2008 Kuninori Morimoto <morimoto.kuninori@renesas.com>
 * Copyright (C) 2008, Robert Jarzmik <robert.jarzmik@free.fr>
 * Copyright 2006-7 Jonathan Corbet <corbet@lwn.net>
 * Copyright (C) 2008 Magnus Damm
 * Copyright (C) 2008, Guennadi Liakhovetski <kernel@pengutronix.de>
 *
 *
 *
 * This driver support
 * - parallel data output
 * - Context A
 * - Primary camera
 *
 * This driver doesn't support
 *  - mipi
 *  - Context B
 *  - Secondary camera
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/videodev2.h>
#include <media/v4l2-chip-ident.h>
#include <media/v4l2-common.h>
#include <media/soc_camera.h>
#include <media/mt9t112.h>

/*
 * max frame size
 */
#define MAX_WIDTH   2048
#define MAX_HEIGHT  1536

/*
 * error checker
 */
#define ECHECKER(x)	\
	ret = x;	\
	if (ret < 0)	\
		return ret;

/*
 * Logical address
 */

#define _VAR(id, offset, base)	(base | (id & 0x1f) << 10 | (offset & 0x3ff))
#define VAR(id, offset) _VAR(id, offset, 0x0000)
#define VAR8(id, offset) _VAR(id, offset, 0x8000)

/* flags for mt9t112_priv :: flags */
#define INIT_DONE  (1<<0)

struct mt9t112_frame_size {
	char  *name;
	u16    width;
	u16    height;
};

struct mt9t112_format {
	char  *name;
	u16    fmt;
	u16    option;
	u32    pixfmt;
};

/*
 * struct
 */
struct mt9t112_priv {
	struct mt9t112_camera_info *info;
	struct i2c_client          *client;
	struct soc_camera_device    icd;
	int                         model;
	u32                         flags;
	const struct mt9t112_frame_size  *frame;
	const struct mt9t112_format      *format;
};


/*
 * supported format list
 */
#define COL_FMT(_name, _depth, _fourcc, _colorspace)		\
	{ .name = _name, .depth = _depth, .fourcc = _fourcc,	\
			.colorspace = _colorspace }
#define RGB_FMT(_name, _depth, _fourcc)				\
	COL_FMT(_name, _depth, V4L2_PIX_FMT_ ## _fourcc, V4L2_COLORSPACE_SRGB)
#define JPG_FMT(_name, _depth, _fourcc)				\
	COL_FMT(_name, _depth, V4L2_PIX_FMT_ ## _fourcc, V4L2_COLORSPACE_JPEG)

static const struct soc_camera_data_format mt9t112_fmt_lists[] = {
	JPG_FMT("CbYCrY 16 bit", 16, UYVY),
	JPG_FMT("CrYCbY 16 bit", 16, VYUY),
	JPG_FMT("YCbYCr 16 bit", 16, YUYV),
	JPG_FMT("YCrYCb 16 bit", 16, YVYU),

	RGB_FMT("RGB 555", 16, RGB555),
	RGB_FMT("RGB 565", 16, RGB565),
};

/*
 * color format list
 */
#define FMT(_pix, _fmt, _option) { .name = #_pix, .fmt = _fmt, .option = _option, .pixfmt = V4L2_PIX_FMT_ ## _pix }
static const struct mt9t112_format mt9t112_cfmts[] = {
	FMT(UYVY, 0x0001, 0x0000),
	FMT(VYUY, 0x0001, 0x0001),
	FMT(YUYV, 0x0001, 0x0002),
	FMT(YVYU, 0x0001, 0x0003),

	FMT(RGB555, 0x0008, 0x0002),
	FMT(RGB565, 0x0004, 0x0002),
};

#define FRMDEF(n, w, h)  { .name = #n, .width = w, .height = h }
static const struct mt9t112_frame_size mt9t112_frame_size[] =
{
	FRMDEF(720p,	1280, 720),
	FRMDEF(XGA,	1024, 768),
	FRMDEF(SVGA,	 800, 600),
	FRMDEF(VGA,	 640, 480),
	FRMDEF(??,	 512, 384),
	FRMDEF(??,	 400, 300),
	FRMDEF(CIF,	 352, 288),
	FRMDEF(QVGA,	 320, 240),
	FRMDEF(???,	 256, 192),
	FRMDEF(QCIF,	 176, 144),
	FRMDEF(QQVGA,	 160, 120),
	FRMDEF(QQCIF,	  88,  72),
};

/*
 * general function
 */

static int mt9t112_reg_read(struct soc_camera_device *icd, u16 command)
{
	struct mt9t112_priv *priv = container_of(icd, struct mt9t112_priv, icd);
	struct i2c_client *client = priv->client;
	struct i2c_msg msg[2];
	u8 buf[2];
	int ret;

	command = swab16(command);

	msg[0].addr  = client->addr;
	msg[0].flags = 0;
	msg[0].len   = 2;
	msg[0].buf   = (u8*)&command;

	msg[1].addr  = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len   = 2;
	msg[1].buf   = buf;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0)
		return ret;

	memcpy(&ret, buf, 2);
	return swab16(ret);
}

static int mt9t112_reg_write(struct soc_camera_device *icd, u16 command, u16 data)
{
	struct mt9t112_priv *priv = container_of(icd, struct mt9t112_priv, icd);
	struct i2c_client *client = priv->client;
	struct i2c_msg msg;
	u8 buf[4];
	int ret;

	command = swab16(command);
	data = swab16(data);

	memcpy(buf + 0, &command, 2);
	memcpy(buf + 2, &data,    2);

	msg.addr  = client->addr;
	msg.flags = 0;
	msg.len   = 4;
	msg.buf   = buf;

	/*
	 * i2c_transfer return sent message length,
	 * but this function should return 0 if correct case
	 * */
	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret >= 0)
		ret = 0;

	return ret;
}

static int mt9t112_reg_mask_set(struct soc_camera_device *icd,
				u16  command,
				u16  mask,
				u16  set)
{
	int val = mt9t112_reg_read(icd, command);
	if (val < 0)
		return val;

	val &= ~mask;
	val |= set & mask;

	return mt9t112_reg_write(icd, command, val);
}

/* mcu access */
static int mt9t112_mcu_read(struct soc_camera_device *icd, u16 command)
{
	int ret;

	ret = mt9t112_reg_write(icd, 0x098E, command);
	if (ret < 0)
		return ret;

	return mt9t112_reg_read(icd, 0x0990);
}

/* mcu access */
static int mt9t112_mcu_write(struct soc_camera_device *icd, u16 command, u16 data)
{
	int ret;

	ret = mt9t112_reg_write(icd, 0x098E, command);
	if (ret < 0)
		return ret;

	return mt9t112_reg_write(icd, 0x0990, data);
}

static int mt9t112_mcu_mask_set(struct soc_camera_device *icd,
				u16  command,
				u16  mask,
				u16  set)
{
	int val = mt9t112_mcu_read(icd, command);
	if (val < 0)
		return val;

	val &= ~mask;
	val |= set & mask;

	return mt9t112_mcu_write(icd, command, val);
}

static int mt9t112_reset(struct soc_camera_device *icd)
{
	int ret;

	ret = mt9t112_reg_mask_set(icd, 0x001a, 0x0001, 0x0001);
	if (ret < 0)
		return ret;

	msleep(1);
	return mt9t112_reg_mask_set(icd, 0x001a, 0x0001, 0x0000);
}

static void mt9t112_clock_info(struct soc_camera_device *icd, u32 ext)
{
	int m, n, p1, p2, p3, p4, p5, p6, p7;
	u32 vco, clk;
	char *enable;

	ext /= 1000; /* kbyte order */

	n = mt9t112_reg_read(icd, 0x0012);
	p1 = n & 0x000f;
	n = n >> 4;
	p2 = n & 0x000f;
	n = n >> 4;
	p3 = n & 0x000f;

	n = mt9t112_reg_read(icd, 0x002a);
	p4 = n & 0x000f;
	n = n >> 4;
	p5 = n & 0x000f;
	n = n >> 4;
	p6 = n & 0x000f;

	n = mt9t112_reg_read(icd, 0x002c);
	p7 = n & 0x000f;

	n = mt9t112_reg_read(icd, 0x0010);
	m = n & 0x00ff;
	n = (n >> 8) & 0x003f;

	enable = ((6000 > ext) || (54000 < ext)) ? "X" : "";
	dev_info(&icd->dev, "EXTCLK          : %10u K %s\n", ext, enable);

	vco = 2 * m * ext / (n+1);
	enable = ((384000 > vco) || (768000 < vco)) ? "X" : "";
	dev_info(&icd->dev, "VCO             : %10u K %s\n", vco, enable);

	clk = vco / (p1+1) / (p2+1);
	enable = (96000 < clk) ? "X" : "";
	dev_info(&icd->dev, "PIXCLK          : %10u K %s\n", clk, enable);

	clk = vco / (p3+1);
	enable = (768000 < clk) ? "X" : "";
	dev_info(&icd->dev, "MIPICLK         : %10u K %s\n", clk, enable);

	clk = vco / (p6+1);
	enable = (96000 < clk) ? "X" : "";
	dev_info(&icd->dev, "MCU CLK         : %10u K %s\n", clk, enable);

	clk = vco / (p5+1);
	enable = (54000 < clk) ? "X" : "";
	dev_info(&icd->dev, "SOC CLK         : %10u K %s\n", clk, enable);

	clk = vco / (p4+1);
	enable = (70000 < clk) ? "X" : "";
	dev_info(&icd->dev, "Sensor CLK      : %10u K %s\n", clk, enable);

	clk = vco / (p7+1);
	dev_info(&icd->dev, "External sensor : %10u K\n", clk);

	clk = ext / (n+1);
	enable = ((2000 > clk) || (24000 < clk)) ? "X" : "";
	dev_info(&icd->dev, "PFD             : %10u K %s\n", clk, enable);
}

static const struct mt9t112_frame_size*
mt9t112_select_frame(u16 width, u16 height)
{
	__u32 diff, mini;
	const struct mt9t112_frame_size *pos, *frame = NULL;
	int i;

	mini = MAX_HEIGHT + MAX_WIDTH;
	for (i=0; i<ARRAY_SIZE(mt9t112_frame_size); i++) {
		pos = mt9t112_frame_size + i;
		diff = abs(width - pos->width) + abs(height - pos->height);
		if (diff < mini) {
			mini = diff;
			frame = pos;
		}
	}

	return frame;
}

static int mt9t112_set_a_frame_size(struct soc_camera_device *icd,
				   u16 width,
				   u16 height )
{
	int ret;
	u16 wstart = (2048 - width) / 2;
	u16 hstart = (1356 - height) / 2;

	/* Output Width (A) */
	ret = mt9t112_mcu_write(icd, VAR(26, 0), width);
	if (ret < 0)
		return ret;

	/* Output Height (A) */
	ret = mt9t112_mcu_write(icd, VAR(26, 2), height);
	if (ret < 0)
		return ret;

	/* Row Start (A)
	 * +4
	 */
	ret = mt9t112_mcu_write(icd, VAR(18, 2), 4 + hstart);
	if (ret < 0)
		return ret;

	/* Column Start (A)
	 * +4
	 */
	ret = mt9t112_mcu_write(icd, VAR(18, 4), 4 + wstart);
	if (ret < 0)
		return ret;

	/* Row End (A)
	 * +11
	 */
	ret = mt9t112_mcu_write(icd, VAR(18, 6), height + 11 + hstart);
	if (ret < 0)
		return ret;

	/* Column End (A)
	 * +11
	 */
	ret = mt9t112_mcu_write(icd, VAR(18, 8), width + 11 + wstart);
	if (ret < 0)
		return ret;

	/* Contxt A Output Width (A)
	 * +8
	 */
	ret = mt9t112_mcu_write(icd, VAR(18, 43), width + 8);
	if (ret < 0)
		return ret;

	/* Contxt A Output Height (A)
	 * +8
	 */
	ret = mt9t112_mcu_write(icd, VAR(18, 45), height + 8);
	if (ret < 0)
		return ret;

	mt9t112_mcu_write(icd, VAR8(1, 0), 0x06);
	return ret;
}

static int mt9t112_set_pll_dividers(struct soc_camera_device *icd,
				    u8 m, u8 n,
				    u8 p1, u8 p2, u8 p3, u8 p4, u8 p5, u8 p6, u8 p7)
{
	int ret;

	ECHECKER(mt9t112_reg_mask_set(icd, 0x0010, 0x3fff, (n << 8) | m));
	ECHECKER(mt9t112_reg_mask_set(icd, 0x0012, 0x0fff, (p3 << 8) | (p2 << 4) | p1));
	ECHECKER(mt9t112_reg_mask_set(icd, 0x002A, 0x7fff, 0x7000 | (p6 << 8) | (p5 << 4) | p4));
	ECHECKER(mt9t112_reg_mask_set(icd, 0x002C, 0x100f, 0x1000 | p7));

	return ret;
}

static int mt9t112_af_set(struct soc_camera_device *icd)
{
	int ret;

	/*
	 * Auto focus settings
	 */
	ECHECKER(mt9t112_mcu_write(icd, VAR(12, 13), 0x000F));	// AF_FILTER)S
	ECHECKER(mt9t112_mcu_write(icd, VAR(12, 23), 0x0F0F));	// AF_THRESHOLD)S
	ECHECKER(mt9t112_mcu_write(icd, VAR8(1, 0), 0x06)); // SEQ_CMD

	//********** Add AF Register *************************
	ECHECKER(mt9t112_reg_write(icd, 0x0614, 0x0000)); 	// SECOND_SCL_SDA_PD [1]
	ECHECKER(mt9t112_mcu_write(icd, VAR8(1, 0), 0x05)); // SEQ_CMD [1]
	ECHECKER(mt9t112_mcu_write(icd, VAR8(12, 0x0002), 0x02)); 	// AF_MODE [1]
	ECHECKER(mt9t112_mcu_write(icd, VAR(12, 0x0003), 0x0002)); 	// AF_ALGO [1]
	ECHECKER(mt9t112_mcu_write(icd, VAR(17, 0x0003), 0x8001)); 	// AFM_ALGO [1]
	ECHECKER(mt9t112_mcu_write(icd, VAR(17, 0x000B), 0x0025)); 	// AFM_POS_MIN [1]
	ECHECKER(mt9t112_mcu_write(icd, VAR(17, 0x000D), 0x0193)); 	// AFM_POS_MAX [1]
	ECHECKER(mt9t112_mcu_write(icd, VAR8(17, 0x0021), 0x18)); 	// AFM_SI_SLAVE_ADDR [1]
	ECHECKER(mt9t112_mcu_write(icd, VAR8(1, 0), 0x05));// SEQ_CMD

	return ret;
}

static int mt9t112_af_trigger(struct soc_camera_device *icd)
{
	int ret;

	//********* AF Trigger ***************
	ECHECKER(mt9t112_reg_write(icd, 0x098e, 0xb019));
	ECHECKER(mt9t112_reg_write(icd, 0x0990, 0x0001));

	return ret;
}

static int mt9t112_init_pll(struct soc_camera_device *icd)
{
	struct mt9t112_priv *priv = container_of(icd, struct mt9t112_priv, icd);
	int data, i, ret;

	ECHECKER(mt9t112_reg_mask_set(icd, 0x0014, 0x003, 0x0001));

	ECHECKER(mt9t112_reg_write(icd, 0x0014, 0x2145));        //PLL control: BYPASS PLL = 8517

	/* Replace these registers when new timing parameters are generated */
	ECHECKER(mt9t112_set_pll_dividers(icd,
					  priv->info->divider.m,
					  priv->info->divider.n,
					  priv->info->divider.p1,
					  priv->info->divider.p2,
					  priv->info->divider.p3,
					  priv->info->divider.p4,
					  priv->info->divider.p5,
					  priv->info->divider.p6,
					  priv->info->divider.p7));

	ECHECKER(mt9t112_reg_write(icd, 0x0014, 0x2525));        //PLL control: TEST_BYPASS on = 9541
	ECHECKER(mt9t112_reg_write(icd, 0x0014, 0x2527));        //PLL control: PLL_ENABLE on = 9543
	ECHECKER(mt9t112_reg_write(icd, 0x0014, 0x3427));        //PLL control: SEL_LOCK_DET on = 13383
	ECHECKER(mt9t112_reg_write(icd, 0x0014, 0x3027));        //PLL control: TEST_BYPASS off = 12359

	mdelay(10);

	ECHECKER(mt9t112_reg_write(icd, 0x0014, 0x3046));        /* PLL control: PLL_BYPASS off  */
	ECHECKER(mt9t112_reg_write(icd, 0x0022, 0x0190));        /* Reference clock count  */
	ECHECKER(mt9t112_reg_write(icd, 0x3B84, 0x0212));        /* I2C Master Clock Divider */

	ECHECKER(mt9t112_reg_write(icd, 0x002E, 0x0500));	//External sensor clock is PLL bypass

	ECHECKER(mt9t112_reg_mask_set(icd, 0x0018, 0x0002, 0x0002));
	ECHECKER(mt9t112_reg_mask_set(icd, 0x3B82, 0x0004, 0x0004));

	/* MCU disabled */
	ECHECKER(mt9t112_reg_mask_set(icd, 0x0018, 0x0004, 0x0004));

	/* out of standby */
	ECHECKER(mt9t112_reg_mask_set(icd, 0x0018, 0x0001, 0));

	mdelay(50);

	ECHECKER(mt9t112_reg_write(icd, 0x0614, 0x0001));	// Disable Secondary I2C Pads
	mdelay(1);
	ECHECKER(mt9t112_reg_write(icd, 0x0614, 0x0001));	// Disable Secondary I2C Pads
	mdelay(1);
	ECHECKER(mt9t112_reg_write(icd, 0x0614, 0x0001));	// Disable Secondary I2C Pads
	mdelay(1);
	ECHECKER(mt9t112_reg_write(icd, 0x0614, 0x0001));	// Disable Secondary I2C Pads
	mdelay(1);
	ECHECKER(mt9t112_reg_write(icd, 0x0614, 0x0001));	// Disable Secondary I2C Pads
	mdelay(1);
	ECHECKER(mt9t112_reg_write(icd, 0x0614, 0x0001));	// Disable Secondary I2C Pads
	mdelay(1);

	/* poll to verify out of standby. Must Poll this bit */
	for (i=0; i<100; i++) {
		data = mt9t112_reg_read(icd, 0x0018);
		if (!(0x4000 & data))
			break;
		mdelay(10);
	}

	return ret;
}

static int mt9t112_init_timing_param(struct soc_camera_device *icd)
{

	int ret;

	/* Adaptive Output Clock (A) */
	ECHECKER(mt9t112_mcu_mask_set(icd, VAR(26, 160), 0x0040, 0x0000));

	/* Read Mode (A) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 12), 0x0024));

	/* Fine Correction (A) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 15), 0x00CC));

	/* Fine IT Min (A) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 17), 0x01f1));

	/* Fine IT Max Margin (A) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 19), 0x00fF));

	/* Base Frame Lines (A) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 29), 0x032D));

	/* Min Line Length (A) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 31), 0x073a));

	/* Line Length (A) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 37), 0x07d0));

	/* Adaptive Output Clock (B) */
	ECHECKER(mt9t112_mcu_mask_set(icd, VAR(27, 160), 0x0040, 0x0000));

	/* Row Start (B) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 74), 0x004));

	/* Column Start (B) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 76), 0x004));

	/* Row End (B) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 78), 0x60B));

	/* Column End (B) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 80), 0x80B));

	/* Fine Correction (B) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 87), 0x008C));

	/* Fine IT Min (B) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 89), 0x01F1));

	/* Fine IT Max Margin (B) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 91), 0x00FF));

	/* Base Frame Lines (B) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 101), 0x0668));

	/* Min Line Length (B) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 103), 0x0AF0));

	/* Line Length (B) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 109), 0x0AF0));

	/*
	 * Flicker Dectection registers
	 * This section should be replace whenever new Timing file is generated
	 * All the following registers need to be replaced
	 * Following registers are generated from Register Wizard but user can
	 * modify them for detail auto flicker detection tuning
	 */

	/* FD_FDPERIOD_SELECT */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(8, 5), 0x01));

	/* PRI_B_CONFIG_FD_ALGO_RUN */
	ECHECKER(mt9t112_mcu_write(icd, VAR(27, 17), 0x0003));

	/* PRI_A_CONFIG_FD_ALGO_RUN */
	ECHECKER(mt9t112_mcu_write(icd, VAR(26, 17), 0x0003));

	/*
	 * AFD range detection tuning registers
	 */

	/* search_f1_50 */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 165), 0x25));

	/* search_f2_50 */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 166), 0x28));

	/* search_f1_60 */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 167), 0x2C));

	/* search_f2_60 */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 168), 0x2F));

	/* period_50Hz (A) */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 68), 0xBA));

	/* secret register by aptina */
	/* period_50Hz (A MSB) */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 303), 0x00));

	/* period_60Hz (A) */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 69), 0x9B));

	/* secret register by aptina */
	/* period_60Hz (A MSB) */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 301), 0x00));

	/* period_50Hz (B) */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 140), 0x82));

	/* secret register by aptina */
	/* period_50Hz (B) MSB */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 304), 0x00));

	/* period_60Hz (B) */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 141), 0x6D));

	/* secret register by aptina */
	/* period_60Hz (B) MSB */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 302), 0x00));

	/* FD Mode */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(8, 2), 0x10));

	/* Stat_min */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(8, 9), 0x02));

	/* Stat_max */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(8, 10), 0x03));

	/* Min_amplitude */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(8, 12), 0x0A));

	/* RX FIFO Watermark (A) */
	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 70), 0x0014));

	/* RX FIFO Watermark (B) */
 	ECHECKER(mt9t112_mcu_write(icd, VAR(18, 142), 0x0014));

	/* MCLK: 16MHz
	 * PCLK: 73MHz
	 * CorePixCLK: 36.5 MHz
	 */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 0x0044), 133));
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 0x0045), 110));
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 0x008c), 130));
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 0x008d), 108));

	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 0x00A5), 27));
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 0x00a6), 30));
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 0x00a7), 32));
	ECHECKER(mt9t112_mcu_write(icd, VAR8(18, 0x00a8), 35));

	return ret;
}

/*
 * soc_camera_ops function
 */

static int mt9t112_init(struct soc_camera_device *icd)
{
	struct mt9t112_priv *priv = container_of(icd, struct mt9t112_priv, icd);
	int ret = 0;

	if (priv->info->link.power) {
		ret = priv->info->link.power(&priv->client->dev, 1);
		if (ret < 0)
			return ret;
	}

	if (priv->info->link.reset)
		ret = priv->info->link.reset(&priv->client->dev);

	return ret;
}

static int mt9t112_release(struct soc_camera_device *icd)
{
	struct mt9t112_priv *priv = container_of(icd, struct mt9t112_priv, icd);
	int ret = 0;

	if (priv->info->link.power)
		ret = priv->info->link.power(&priv->client->dev, 0);

	return ret;
}

static int mt9t112_start_capture(struct soc_camera_device *icd)
{
	struct mt9t112_priv *priv = container_of(icd, struct mt9t112_priv, icd);
	u32 clock = 24000000;
//	u32 clock = 18000000;
//	u32 clock = 6750000;

	if (!(priv->flags & INIT_DONE)) {
		dev_err(&icd->dev, "device init doesn't done\n");
		return -EINVAL;
	}

	priv->info->clock_ctrl(clock);

	mt9t112_af_trigger(icd);

	dev_info(&icd->dev, "format : %s\n", priv->format->name);
	dev_info(&icd->dev, "size   : %s (%d x %d)\n",
		 priv->frame->name,
		 priv->frame->width,
		 priv->frame->height);
	mt9t112_clock_info(icd, clock);


	return 0;
}

static int mt9t112_stop_capture(struct soc_camera_device *icd)
{
//	struct mt9t112_priv *priv = container_of(icd, struct mt9t112_priv, icd);
//	priv->info->clock_ctrl(0);
	return 0;
}

static int mt9t112_set_bus_param(struct soc_camera_device *icd,
				 unsigned long	flags)
{
	return 0;
}

static unsigned long mt9t112_query_bus_param(struct soc_camera_device *icd)
{
	struct mt9t112_priv *priv = container_of(icd, struct mt9t112_priv, icd);
	struct soc_camera_link *icl = &priv->info->link;
	unsigned long flags = SOCAM_MASTER | SOCAM_VSYNC_ACTIVE_HIGH |
		SOCAM_HSYNC_ACTIVE_HIGH | SOCAM_DATA_ACTIVE_HIGH;

	flags |= (priv->info->flags & MT9T112_FLAG_PCLK_RISING_EDGE) ?
		SOCAM_PCLK_SAMPLE_RISING : SOCAM_PCLK_SAMPLE_FALLING;

	if (priv->info->flags & MT9T112_FLAG_DATAWIDTH_8)
		flags |= SOCAM_DATAWIDTH_8;
	else
		flags |= SOCAM_DATAWIDTH_10;

	return soc_camera_apply_sensor_flags(icl, flags);
}

static int mt9t112_get_control(struct soc_camera_device *icd,
			      struct v4l2_control *ctrl)
{
/*
	struct mt9t112_priv *priv = container_of(icd, struct mt9t112_priv, icd);

	switch (ctrl->id) {
	case V4L2_CID_VFLIP:
		ctrl->value = priv->flag_vflip;
		break;
	case V4L2_CID_HFLIP:
		ctrl->value = priv->flag_hflip;
		break;
	}
*/
	return 0;
}

static int mt9t112_set_control(struct soc_camera_device *icd,
			      struct v4l2_control *ctrl)
{
/*
	struct mt9t112_priv *priv = container_of(icd, struct mt9t112_priv, icd);
	int ret = 0;
	u8 val;

	switch (ctrl->id) {
	case V4L2_CID_VFLIP:
		val = ctrl->value ? VFLIP_IMG : 0x00;
		priv->flag_vflip = ctrl->value;
		if (priv->info->flags & OV772X_FLAG_VFLIP)
			val ^= VFLIP_IMG;
		ret = mt9t112_mask_set(priv->client, COM3, VFLIP_IMG, val);
		break;
	case V4L2_CID_HFLIP:
		val = ctrl->value ? HFLIP_IMG : 0x00;
		priv->flag_hflip = ctrl->value;
		if (priv->info->flags & OV772X_FLAG_HFLIP)
			val ^= HFLIP_IMG;
		ret = mt9t112_mask_set(priv->client, COM3, HFLIP_IMG, val);
		break;
	}
	return ret;
*/
	return 0;
}

static int mt9t112_get_chip_id(struct soc_camera_device *icd,
			      struct v4l2_dbg_chip_ident   *id)
{
	struct mt9t112_priv *priv = container_of(icd, struct mt9t112_priv, icd);

	id->ident    = priv->model;
	id->revision = 0;

	return 0;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int mt9t112_get_register(struct soc_camera_device *icd,
			       struct v4l2_dbg_register *reg)
{
	int                 ret;

	reg->size = 2;
	ret = mt9t112_reg_read(icd, reg);
	if (ret < 0)
		return ret;

	reg->val = (__u64)ret;

	return 0;
}

static int mt9t112_set_register(struct soc_camera_device *icd,
			       struct v4l2_dbg_register *reg)
{
	return mt9t112_reg_write(icd, reg->reg, reg->val);
}
#endif

static int mt9t112_init_camera(struct soc_camera_device *icd)
{
	int ret;

	/*
	 * reset
	 */
	ECHECKER(mt9t112_reset(icd));

	/* section 1 */
	ECHECKER(mt9t112_init_pll(icd));

	/* section 2 */
	ECHECKER(mt9t112_init_timing_param(icd));

	ECHECKER(mt9t112_af_set(icd));

	/*
	 * section 7
	 */
	ECHECKER(mt9t112_reg_mask_set(icd, 0x0018, 0x0004, 0));

	/* Analog setting B */
	ECHECKER(mt9t112_reg_write(icd, 0x3084, 0x2409));
	ECHECKER(mt9t112_reg_write(icd, 0x3092, 0x0A49));
	ECHECKER(mt9t112_reg_write(icd, 0x3094, 0x4949));
	ECHECKER(mt9t112_reg_write(icd, 0x3096, 0x4950));

	/* Disable adaptive clock */
	ECHECKER(mt9t112_mcu_write(icd, VAR(26, 160), 0x0A2E)); /* PRI_A_CONFIG_JPEG_OB_TX_CONTROL_VAR */
	ECHECKER(mt9t112_mcu_write(icd, VAR(27, 160), 0x0A2E)); /* PRI_B_CONFIG_JPEG_OB_TX_CONTROL_VAR */

	/* Configure STatus in Status_before_length Format and enable header */
	ECHECKER(mt9t112_mcu_write(icd, VAR(27, 144), 0x0CB4)); /* PRI_B_CONFIG_JPEG_OB_TX_CONTROL_VAR */

	/* Enable JPEG in context B */
	ECHECKER(mt9t112_mcu_write(icd, VAR8(27, 142), 0x01)); /* PRI_B_CONFIG_JPEG_OB_TX_CONTROL_VAR */

	/* Disable Dac_TXLO */
	ECHECKER(mt9t112_reg_write(icd, 0x316C, 0x350F));

	/* Set max slew rates */
	ECHECKER(mt9t112_reg_write(icd, 0x1E, 0x777));

	return ret;
}

static int mt9t112_set_params(struct soc_camera_device *icd,
			      u32 width, u32 height, u32 pixfmt)
{
	struct mt9t112_priv *priv = container_of(icd, struct mt9t112_priv, icd);
	int i, ret;

	/*
	 * get frame size
	 */
	priv->frame = mt9t112_select_frame(width, height);
	if (!priv->frame)
		return -EINVAL;

	/*
	 * get color format
	 */
	priv->format = NULL;
	for (i=0; i<ARRAY_SIZE(mt9t112_cfmts); i++) {
		if (mt9t112_cfmts[i].pixfmt == pixfmt) {
			priv->format = mt9t112_cfmts + i;
			break;
		}
	}
	if (!priv->format)
		return -EINVAL;

	/*
	 * mt9t112 should init in 1st time.
	 */
	if (!(priv->flags & INIT_DONE)) {

		u16 param = (MT9T112_FLAG_PCLK_RISING_EDGE & priv->info->flags) ? 0x0001 : 0x0000;

		ECHECKER(mt9t112_init_camera(icd));

		/* for 30Hz
		ECHECKER(mt9t112_mcu_write(icd, VAR(18, 12), 0x0024));
		ECHECKER(mt9t112_mcu_write(icd, VAR(26, 21), 0x0006));
		ECHECKER(mt9t112_mcu_write(icd, VAR(26, 23), 0x0007));
		*/

		/*
		 * Invert PCLK (Data sampled on falling edge of pixclk)
		 */
		ECHECKER(mt9t112_reg_write(icd, 0x3C20, param));

		priv->flags |= INIT_DONE;
	}

	mdelay(5);

	/*
	 * set color format, frame size
	 */
	ECHECKER(mt9t112_mcu_write(icd, VAR(26, 7), priv->format->fmt));
	ECHECKER(mt9t112_mcu_write(icd, VAR(26, 9), priv->format->option));
	ECHECKER(mt9t112_mcu_write(icd, VAR8(1, 0), 0x06));

	return mt9t112_set_a_frame_size(icd, priv->frame->width, priv->frame->height);
}

static int mt9t112_set_crop(struct soc_camera_device *icd,
			   struct v4l2_rect *rect)
{
	return mt9t112_set_params(icd, rect->width, rect->height,
				 V4L2_PIX_FMT_UYVY);
}

static int mt9t112_set_fmt(struct soc_camera_device *icd,
			  struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;

	return mt9t112_set_params(icd, pix->width, pix->height,
				 pix->pixelformat);
}

static int mt9t112_try_fmt(struct soc_camera_device *icd,
			  struct v4l2_format       *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;
	const struct mt9t112_frame_size *frame;

	frame = mt9t112_select_frame(pix->width, pix->height);
	if (!frame)
		return -EINVAL;

	pix->width  = frame->width;
	pix->height = frame->height;
	pix->field  = V4L2_FIELD_NONE;

	return 0;
}

static int mt9t112_camera_probe(struct soc_camera_device *icd)
{
	struct mt9t112_priv *priv = container_of(icd, struct mt9t112_priv, icd);
	const char          *devname;
	int                  chipid;

	/*
	 * We must have a parent by now. And it cannot be a wrong one.
	 * So this entire test is completely redundant.
	 */
	if (!icd->dev.parent ||
	    to_soc_camera_host(icd->dev.parent)->nr != icd->iface)
		return -ENODEV;

	/*
	 * check and show chip ID
	 */
	chipid = mt9t112_reg_read(icd, 0x0000);
	if (chipid < 0)
		return -EIO;

	switch (chipid) {
	case 0x2680 :
		devname = "mt9t111";
		priv->model = V4L2_IDENT_MT9T111;
		break;
	case 0x2682 :
		devname = "mt9t112";
		priv->model = V4L2_IDENT_MT9T112;
		break;
	default:
		dev_err(&icd->dev,
			"Product ID error %04x\n", chipid);
		return -ENODEV;
	}

	icd->formats     = mt9t112_fmt_lists;
	icd->num_formats = ARRAY_SIZE(mt9t112_fmt_lists);

	dev_info(&icd->dev,
		 "%s chip ID %04x\n", devname, chipid);

	return soc_camera_video_start(icd);
}

static void mt9t112_camera_remove(struct soc_camera_device *icd)
{
	soc_camera_video_stop(icd);
}

static struct soc_camera_ops mt9t112_ops = {
	.owner			= THIS_MODULE,
	.probe			= mt9t112_camera_probe,
	.remove			= mt9t112_camera_remove,
	.init			= mt9t112_init,
	.release		= mt9t112_release,
	.start_capture		= mt9t112_start_capture,
	.stop_capture		= mt9t112_stop_capture,
	.set_crop		= mt9t112_set_crop,
	.set_fmt		= mt9t112_set_fmt,
	.try_fmt		= mt9t112_try_fmt,
	.set_bus_param		= mt9t112_set_bus_param,
	.query_bus_param	= mt9t112_query_bus_param,
//	.controls		= mt9t112_controls,
//	.num_controls		= ARRAY_SIZE(mt9t112_controls),
	.get_control		= mt9t112_get_control,
	.set_control		= mt9t112_set_control,
	.get_chip_id		= mt9t112_get_chip_id,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.get_register		= mt9t112_get_register,
	.set_register		= mt9t112_set_register,
#endif
};

/*
 * i2c_driver function
 */

static int mt9t112_probe(struct i2c_client *client,
			 const struct i2c_device_id *did)
{
	struct mt9t112_priv        *priv;
	struct mt9t112_camera_info *info;
	struct soc_camera_device   *icd;
	struct i2c_adapter         *adapter = to_i2c_adapter(client->dev.parent);
	int                         ret;

	info = container_of(client->dev.platform_data,
			    struct mt9t112_camera_info, link);
	if (!info)
		return -EINVAL;

	if (!info->clock_ctrl) {
		dev_err(&adapter->dev,
			"clock_ctrl function is needed in mt9t112 driver\n");
		return -EINVAL;
	}

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&adapter->dev,
			"I2C-Adapter doesn't support "
			"I2C_FUNC_SMBUS_BYTE_DATA\n");
		return -EIO;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->info   = info;
	priv->client = client;
	i2c_set_clientdata(client, priv);

	icd             = &priv->icd;
	icd->ops        = &mt9t112_ops;
	icd->control    = &client->dev;
	icd->width_max  = MAX_WIDTH;
	icd->height_max = MAX_HEIGHT;
	icd->iface      = priv->info->link.bus_id;

	ret = soc_camera_device_register(icd);

	if (ret) {
		i2c_set_clientdata(client, NULL);
		kfree(priv);
	}

	return ret;
}

static int mt9t112_remove(struct i2c_client *client)
{
	struct mt9t112_priv *priv = i2c_get_clientdata(client);

	soc_camera_device_unregister(&priv->icd);
	i2c_set_clientdata(client, NULL);
	kfree(priv);
	return 0;
}

static const struct i2c_device_id mt9t112_id[] = {
	{ "mt9t112", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mt9t112_id);

static struct i2c_driver mt9t112_i2c_driver = {
	.driver = {
		.name = "mt9t112",
	},
	.probe    = mt9t112_probe,
	.remove   = mt9t112_remove,
	.id_table = mt9t112_id,
};

/*
 * module function
 */

static int __init mt9t112_module_init(void)
{
	return i2c_add_driver(&mt9t112_i2c_driver);
}

static void __exit mt9t112_module_exit(void)
{
	i2c_del_driver(&mt9t112_i2c_driver);
}

module_init(mt9t112_module_init);
module_exit(mt9t112_module_exit);

MODULE_DESCRIPTION("SoC Camera driver for mt9t11x");
MODULE_AUTHOR("Kuninori Morimoto");
MODULE_LICENSE("GPL v2");
