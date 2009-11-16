/*
 * linux/drivers/mmc/host/sh-sdhi.c
 *
 * SD/MMC driver.
 *
 * Copyright (C) 2008 Renesas Solutions Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License.
 *
 */

#include <linux/autoconf.h>
#include <linux/dma-mapping.h>
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/core.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/sdio.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>

#include "sh_sdhi.h"

#define DRIVER_NAME "sh-sdhi"

static void sdhi_clock_control(int ch, unsigned long clk)
{
	if (pread(ch, SDHI_R15) & BIT(14)) {
		pr_err(DRIVER_NAME": Busy state ! Cannot change the clock\n");
		return;
	}

	pwrite(ch, SDHI_R18, ~R18_ENABLE & pread(ch, SDHI_R18));
	switch (clk) {
	case 0:
		return;
	case CLKDEV_INIT:
		pwrite(ch, SDHI_R18, R18_INIT);
		break;
	case CLKDEV_SD_DATA:
		pwrite(ch, SDHI_R18, R18_TRANS);
		break;
	case CLKDEV_HS_DATA:
		pwrite(ch, SDHI_R18, R18_HS_TRANS);
		break;
	case CLKDEV_MMC_DATA:
		pwrite(ch, SDHI_R18, R18_MMC_TRANS);
		break;
	default:
		return;
	}
	pwrite(ch, SDHI_R18, R18_ENABLE | pread(ch, SDHI_R18));
}

static void sdhi_sync_reset(int ch)
{
	pwrite(ch, SDHI_R112, R112_SDRST_ON);
	pwrite(ch, SDHI_R112, R112_SDRST_OFF);
	pwrite(ch, SDHI_R18, R18_ENABLE | pread(ch, SDHI_R18));
}

static int sdhi_error_manage(int ch)
{
	unsigned short e_state1, e_state2;
	int ret;

	g_sd_error[ch] = 0;
	g_wait_int[ch] = 0;

	if (!(pread(ch, SDHI_R14) & R14_ISD0CD)) {
		pr_debug("%s: card remove(R14 = %04x)\n", \
			DRIVER_NAME, pread(ch, SDHI_R14));
		return -ENOMEDIUM;
	}

	e_state1 = pread(ch, SDHI_R22);
	e_state2 = pread(ch, SDHI_R23);
	if (e_state2 & R23_SYS_ERROR) {
		if (e_state2 & R23_RES_STOP_TIMEOUT)
			ret = -ETIMEDOUT;
		else
			ret = -EILSEQ;
		pr_debug("%s: R23 = %04x\n", \
				DRIVER_NAME, pread(ch, SDHI_R23));
		sdhi_sync_reset(ch);
		pwrite(ch, SDHI_R16, R16_DATA3_CARD_RE | R16_DATA3_CARD_IN);
		return ret;
	}
	if (e_state1 & R22_CRC_ERROR || e_state1 & R22_CMD_ERROR)
		ret = -EILSEQ;
	else
		ret = -ETIMEDOUT;

	pr_debug("%s: R22 = %04x \n", DRIVER_NAME, pread(ch, SDHI_R22));
	sdhi_sync_reset(ch);
	pwrite(ch, SDHI_R16, R16_DATA3_CARD_RE | R16_DATA3_CARD_IN);
	return ret;
}

static int sdhi_single_read(int ch, struct mmc_request *mrq)
{
	struct mmc_data *data = mrq->data;
	long time, timeout;
	unsigned short blocksize, i;
	unsigned short *p = sg_virt(data->sg);

	g_wait_int[ch] = 0;
	pwrite(ch, SDHI_R17, ~(R17_BRE_ENABLE | R17_BUF_ILL_READ)
				& pread(ch, SDHI_R17));
	pwrite(ch, SDHI_R16, ~R16_ACCESS_END & pread(ch, SDHI_R16));
	timeout = g_timeout;
	time = wait_event_interruptible_timeout(intr_wait,
		g_wait_int[ch] == 1 || g_sd_error[ch] == 1, timeout);
	if (time == 0 || g_sd_error[ch] != 0)
		return sdhi_error_manage(ch);

	g_wait_int[ch] = 0;
	blocksize = pread(ch, SDHI_R19) + 1;
	for (i = 0; i < blocksize / 2; i++)
		*p++ = pread(ch, SDHI_R24);

	timeout = g_timeout;
	time = wait_event_interruptible_timeout(intr_wait,
		g_wait_int[ch] == 1 || g_sd_error[ch] == 1, timeout);
	if (time == 0 || g_sd_error[ch] != 0)
		return sdhi_error_manage(ch);

	g_wait_int[ch] = 0;
	return 0;
}

static int sdhi_multi_read(int ch, struct mmc_request *mrq)
{
	struct mmc_data *data = mrq->data;
	long time, timeout;
	unsigned short blocksize, i, j, sec, *p;

	blocksize = pread(ch, SDHI_R19);

	for (j = 0; j < data->sg_len; j++) {
		p = sg_virt(data->sg);
		g_wait_int[ch] = 0;
		for (sec = 0; sec < data->sg->length / blocksize; sec++) {
			pwrite(ch, SDHI_R17, ~(R17_BRE_ENABLE |
				R17_BUF_ILL_READ) & pread(ch, SDHI_R17));

			timeout = g_timeout;
			time = wait_event_interruptible_timeout(intr_wait,
				g_wait_int[ch] == 1 || g_sd_error[ch] == 1,
				timeout);
			if (time == 0 || g_sd_error[ch] != 0)
				return sdhi_error_manage(ch);

			g_wait_int[ch] = 0;
			for (i = 0; i < blocksize / 2; i++)
				*p++ = pread(ch, SDHI_R24);
		}
		if (j < data->sg_len - 1)
			data->sg++;
	}
	return 0;
}

static int sdhi_single_write(int ch, struct mmc_request *mrq)
{
	struct mmc_data *data = mrq->data;
	long time, timeout;
	unsigned short blocksize, i;
	unsigned short *p = sg_virt(data->sg);

	g_wait_int[ch] = 0;
	pwrite(ch, SDHI_R17, ~(R17_BWE_ENABLE |
			R17_BUF_ILL_WRITE) & pread(ch, SDHI_R17));
	pwrite(ch, SDHI_R16, ~R16_ACCESS_END & pread(ch, SDHI_R16));

	timeout = g_timeout;
	time = wait_event_interruptible_timeout(intr_wait,
		g_wait_int[ch] == 1 || g_sd_error[ch] == 1, timeout);
	if (time == 0 || g_sd_error[ch] != 0)
		return sdhi_error_manage(ch);

	g_wait_int[ch] = 0;
	blocksize = pread(ch, SDHI_R19) + 1;
	for (i = 0; i < blocksize / 2; i++)
		pwrite(ch, SDHI_R24, *p++);

	timeout = g_timeout;
	time = wait_event_interruptible_timeout(intr_wait,
		g_wait_int[ch] == 1 || g_sd_error[ch] == 1, timeout);
	if (time == 0 || g_sd_error[ch] != 0)
		return sdhi_error_manage(ch);

	g_wait_int[ch] = 0;
	return 0;
}

static int sdhi_multi_write(int ch, struct mmc_request *mrq)
{
	struct mmc_data *data = mrq->data;
	long time, timeout;
	unsigned short i, sec, j, blocksize, *p;

	blocksize = pread(ch, SDHI_R19);

	for (j = 0; j < data->sg_len; j++) {
		p = sg_virt(data->sg);
		g_wait_int[ch] = 0;
		for (sec = 0; sec < data->sg->length / blocksize; sec++) {
			pwrite(ch, SDHI_R17, ~(R17_BWE_ENABLE |
				R17_BUF_ILL_WRITE) & pread(ch, SDHI_R17));

			timeout = g_timeout;
			time = wait_event_interruptible_timeout(intr_wait,
				g_wait_int[ch] == 1 || g_sd_error[ch] == 1,
				timeout);
			if (time == 0 || g_sd_error[ch] != 0)
				return sdhi_error_manage(ch);

			g_wait_int[ch] = 0;
			for (i = 0; i < blocksize / 2; i++)
				pwrite(ch, SDHI_R24, *p++);
		}
		if (j < data->sg_len - 1)
			data->sg++;
	}
	return 0;
}

static void sdhi_get_response(int ch, struct mmc_command *cmd)
{
	unsigned short i, j, resp[8];
	unsigned long *p1, *p2;

	if (cmd->flags & MMC_RSP_136) {
		resp[0] = pread(ch, SDHI_R06);
		resp[1] = pread(ch, SDHI_R07);
		resp[2] = pread(ch, SDHI_R08);
		resp[3] = pread(ch, SDHI_R09);
		resp[4] = pread(ch, SDHI_R10);
		resp[5] = pread(ch, SDHI_R11);
		resp[6] = pread(ch, SDHI_R12);
		resp[7] = pread(ch, SDHI_R13);

		/* SDHI REGISTER SPECIFICATION */
		for (i = 7, j = 6; i > 0; i--) {
			resp[i] = (resp[i] << 8) & 0xff00;
			resp[i] |= (resp[j--] >> 8) & 0x00ff;
		}
		resp[0] = (resp[0] << 8) & 0xff00;
		/* SDHI REGISTER SPECIFICATION */

		p1 = ((unsigned long *)resp) + 3;
		p2 = (unsigned long *)cmd->resp;
#if defined(__BIG_ENDIAN_BITFIELD)
		for (i = 0; i < 4; i++) {
			*p2++ = ((*p1 >> 16) & 0x0000ffff) |
					((*p1 << 16) & 0xffff0000);
			p1--;
		}
#else
		for (i = 0; i < 4; i++)
			*p2++ = *p1--;
#endif /* __BIG_ENDIAN_BITFIELD */

	} else {
		resp[0] = pread(ch, SDHI_R06);
		resp[1] = pread(ch, SDHI_R07);

		p1 = ((unsigned long *)resp);
		p2 = (unsigned long *)cmd->resp;
#if defined(__BIG_ENDIAN_BITFIELD)
		*p2 = ((*p1 >> 16) & 0x0000ffff) | ((*p1 << 16) & 0xffff0000);
#else
		*p2 = *p1;
#endif /* __BIG_ENDIAN_BITFIELD */
	}
}

static unsigned short sdhi_set_cmd(struct sdhi_host *host,
			struct mmc_request *mrq, unsigned short opc)
{
	struct mmc_data *data = mrq->data;

	switch (opc) {
	case SD_APP_OP_COND:
	case SD_APP_SEND_SCR:
	case SD_APP_SEND_NUM_WR_BLKS:
		opc |= pread(host->ch, SDHI_APP);
		break;
	case SD_APP_SET_BUS_WIDTH:
		 /* SD_APP_SET_BUS_WIDTH*/
		if (host->data == 0)
			opc |= pread(host->ch, SDHI_APP);
		else /* SD_SWITCH */
			opc = R00_SD_SWITCH;
		break;
	case SD_IO_SEND_OP_COND:
		opc = R00_SD_IO_SEND_OP_COND;
		break;
	case SD_IO_RW_DIRECT:
		opc = R00_SD_IO_RW_DIRECT;
		pwrite(host->ch, SDHI_R26, R26_SDIO_MODE_ON);
		pwrite(host->ch, SDHI_R28, R28_SDIO_MASK_CLEAR);
		break;
	case SD_IO_RW_EXTENDED:
		pwrite(host->ch, SDHI_R26, R26_SDIO_MODE_ON);
		pwrite(host->ch, SDHI_R28, R28_SDIO_MASK_CLEAR);
		if (mrq->data->flags & MMC_DATA_READ) {
			if (data->sg->length <= 512)
				opc = R00_SD_IO_RW_EXTENDED_SREAD;
			else
				opc = R00_SD_IO_RW_EXTENDED_MREAD;
		} else if (mrq->data->flags & MMC_DATA_WRITE) {
			if (data->sg->length <= 512)
				opc = R00_SD_IO_RW_EXTENDED_SWRITE;
			else
				opc = R00_SD_IO_RW_EXTENDED_MWRITE;
		} else
			pr_err(DRIVER_NAME" : CMD 53 Setting Err !!\n");
		break;
	default:
		break;
	}
	return opc;
}

static unsigned short sdhi_data_trans(struct sdhi_host *host,
				struct mmc_request *mrq, unsigned short opc)
{
	unsigned short ret;

	switch (opc) {
	case MMC_READ_MULTIPLE_BLOCK:
	case R00_SD_IO_RW_EXTENDED_MREAD:
		ret = sdhi_multi_read(host->ch, mrq);
		break;
	case MMC_WRITE_MULTIPLE_BLOCK:
	case R00_SD_IO_RW_EXTENDED_MWRITE:
		ret = sdhi_multi_write(host->ch, mrq);
		break;
	case MMC_WRITE_BLOCK:
	case R00_SD_IO_RW_EXTENDED_SWRITE:
		ret = sdhi_single_write(host->ch, mrq);
		break;
	case MMC_READ_SINGLE_BLOCK:
	case R00_SD_APP_SEND_SCR:
	case R00_SD_SWITCH: /* SD_SWITCH */
	case R00_SD_IO_RW_EXTENDED_SREAD:
		ret = sdhi_single_read(host->ch, mrq);
		break;
	default:
		pr_err(DRIVER_NAME": SD: SDHI NOT SUPPORT CMD = d'%04d\n", opc);
		ret = -EINVAL;
		break;
	}
	return ret;

}

static void sdhi_start_cmd(struct sdhi_host *host,
			struct mmc_request *mrq, struct mmc_command *cmd)
{
	long time, timeout;
	unsigned short opc = cmd->opcode;
	int ret = 0;
	struct mmc_data *data = mrq->data;

	if (host->data) {
		if (opc == MMC_READ_MULTIPLE_BLOCK	||
			opc == MMC_WRITE_MULTIPLE_BLOCK	||
			(opc == SD_IO_RW_EXTENDED && data->sg->length > 512)) {
			pwrite(host->ch, SDHI_R04, R04_SEC_ENABLE);
			pwrite(host->ch, SDHI_R05, mrq->data->blocks);
		}
		pwrite(host->ch, SDHI_R19, mrq->data->blksz);
	}
	opc = sdhi_set_cmd(host, mrq, opc);

	pwrite(host->ch, SDHI_R16, R16_RESP_END |
					 pread(host->ch, SDHI_R16));
	pwrite(host->ch, SDHI_R02, (unsigned short)(cmd->arg & R02_MASK));
	pwrite(host->ch, SDHI_R03,
			(unsigned short)((cmd->arg >> 16) & R03_MASK));
	g_wait_int[host->ch] = 0;
	pwrite(host->ch, SDHI_R00, (unsigned short)(opc & R00_MASK));

	pwrite(host->ch, SDHI_R16, ~R16_RESP_END & pread(host->ch, SDHI_R16));
	pwrite(host->ch, SDHI_R17,
			~(R17_CMD_ERROR | R17_CRC_ERROR |
			  R17_END_ERROR | R17_TIMEOUT	|
			  R17_RESP_TIMEOUT | R17_ILA)	&
			  pread(host->ch, SDHI_R17));

	timeout = g_timeout;
	time = wait_event_interruptible_timeout(intr_wait,
		g_wait_int[host->ch] == 1 || g_sd_error[host->ch] == 1,
		timeout);
	if (time == 0) {
		cmd->error = sdhi_error_manage(host->ch);
		return;
	}
	if (g_sd_error[host->ch]) {
		switch (cmd->opcode) {
		case MMC_ALL_SEND_CID:
		case MMC_SELECT_CARD:
		case SD_SEND_IF_COND:
		case SD_IO_SEND_OP_COND:
		case MMC_APP_CMD:
			cmd->error = -ETIMEDOUT;
			break;
		default:
			pr_err(DRIVER_NAME": Cmd(d'%d) err\n", cmd->opcode);
			cmd->error = sdhi_error_manage(host->ch);
			break;
		}
		g_sd_error[host->ch] = 0;
		g_wait_int[host->ch] = 0;
		return;
	}
	if (!(cmd->flags & MMC_RSP_PRESENT)) {
		cmd->error = ret;
		g_wait_int[host->ch] = 0;
		return;
	}
	if (pread(host->ch, SDHI_R14) & R14_RESP_END) {
		cmd->error = -EINVAL;
		return;
	}
	if (g_wait_int[host->ch] == 1) {
		sdhi_get_response(host->ch, cmd);
		g_wait_int[host->ch] = 0;
	}
	if (host->data) {
		ret = sdhi_data_trans(host, mrq, opc);
		if (ret == 0) {
			mrq->data->bytes_xfered =
				mrq->data->blocks * mrq->data->blksz;
		} else
			mrq->data->bytes_xfered = 0;
	}
	cmd->error = ret;
}

static void sdhi_stop_cmd(struct sdhi_host *host, struct mmc_request *mrq,
				struct mmc_command *cmd)
{
	long time, timeout;

	pwrite(host->ch, SDHI_R16,
		~R16_ACCESS_END & pread(host->ch, SDHI_R16));

	timeout = g_timeout;
	time = wait_event_interruptible_timeout(intr_wait,
		g_wait_int[host->ch] == 1 || g_sd_error[host->ch] == 1,
		timeout);
	if (time == 0 || g_sd_error[host->ch] != 0) {
		cmd->error = sdhi_error_manage(host->ch);
		return;
	}
	sdhi_get_response(host->ch, cmd);
	g_wait_int[host->ch] = 0;
	cmd->error = 0;
	return;
}

static void sdhi_sdio_ace(int ch, struct mmc_command *cmd)
{
	long time, timeout;

	pwrite(ch, SDHI_R16, ~R16_ACCESS_END & pread(ch, SDHI_R16));

	timeout = g_timeout;
	time = wait_event_interruptible_timeout(intr_wait,
			g_wait_int[ch] == 1 || g_sd_error[ch] == 1, timeout);
	if (time == 0 || g_sd_error[ch] != 0) {
		cmd->error = sdhi_error_manage(ch);
		return;
	}
	g_wait_int[ch] = 0;
	cmd->error = 0;
	return;
}


static void sdhi_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct sdhi_host *host = mmc_priv(mmc);
	struct mmc_data *data = mrq->data;

	if (!(pread(host->ch, SDHI_R14) & R14_ISD0CD)) {
		mrq->cmd->error = -ENOMEDIUM;
		host->data = NULL;
		mmc_request_done(mmc, mrq);
		return;
	}
	host->data = mrq->data;
	sdhi_start_cmd(host, mrq, mrq->cmd);
	host->data = NULL;

	if (mrq->cmd->error != 0) {
		mmc_request_done(mmc, mrq);
		return;
	}

	if (mrq->stop)
		sdhi_stop_cmd(host, mrq, mrq->stop);
	else if (mrq->cmd->opcode == SD_IO_RW_EXTENDED) {
		if (data->sg->length > 512)
			sdhi_sdio_ace(host->ch, mrq->cmd);
	}
	mmc_request_done(mmc, mrq);
}

static void sdhi_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct sdhi_host *host = mmc_priv(mmc);

	if (ios->clock)
		host->clock = ios->clock;

	switch (ios->clock) {
	case CLKDEV_INIT:
	case CLKDEV_SD_DATA:
	case CLKDEV_HS_DATA:
	case CLKDEV_MMC_DATA:
		sdhi_clock_control(host->ch, host->clock);
		break;
	case 0:
	default:
		sdhi_clock_control(host->ch, ios->clock);
		break;
	}

	if (ios->bus_width == MMC_BUS_WIDTH_4)
		pwrite(host->ch, SDHI_R20, ~BUS_WIDTH_1 &
					pread(host->ch, SDHI_R20));
	else
		pwrite(host->ch, SDHI_R20, BUS_WIDTH_1 |
					pread(host->ch, SDHI_R20));
}

static int sdhi_get_ro(struct mmc_host *mmc)
{
	struct sdhi_host *host = mmc_priv(mmc);

	if (pread(host->ch, SDHI_R14) & R14_WRITE_PRO)
		return 0;	/* rw */
	else
		return 1;	/* ro */
}

static void sdhi_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct sdhi_host *host = mmc_priv(mmc);
	if (enable)
		pwrite(host->ch, SDHI_R28, R28_SDIO_MASK_CLEAR);
	else
		pwrite(host->ch, SDHI_R28, R28_SDIO_MASK_ON);
}

static struct mmc_host_ops sdhi_ops = {
	.request	= sdhi_request,
	.set_ios	= sdhi_set_ios,
	.get_ro		= sdhi_get_ro,
	.enable_sdio_irq = sdhi_enable_sdio_irq,
};

static int detect_waiting;
static void sdhi_detect(struct mmc_host *mmc)
{
	struct sdhi_host *host = mmc_priv(mmc);

	pwrite(host->ch, SDHI_R20, BUS_WIDTH_1 | pread(host->ch, SDHI_R20));

	mmc_detect_change(mmc, 0);
	detect_waiting = 0;
}

static irqreturn_t sdhi_intr(int irq, void *dev_id)
{
	struct sdhi_host *host = dev_id;
	int state1 = 0, state2 = 0;

	state1 = pread(host->ch, SDHI_R14);
	state2 = pread(host->ch, SDHI_R15);

	/* CARD Insert */
	if (state1 & R14_CARD_IN) {
		pwrite(host->ch, SDHI_R14,
			~R14_CARD_IN & pread(host->ch, SDHI_R14));
		if (!detect_waiting) {
			detect_waiting = 1;
			sdhi_detect(host->mmc);
		}
		pwrite(host->ch, SDHI_R16, R16_RESP_END	|
				R16_ACCESS_END		|
				R16_CARD_IN		|
				R16_DATA3_CARD_RE	|
				R16_DATA3_CARD_IN);
		return IRQ_HANDLED;
	}
	/* CARD Removal */
	if (state1 & R14_CARD_RE) {
		pwrite(host->ch, SDHI_R14,
			~R14_CARD_RE & pread(host->ch, SDHI_R14));
		if (!detect_waiting) {
			detect_waiting = 1;
			sdhi_detect(host->mmc);
		}
		pwrite(host->ch, SDHI_R16, R16_RESP_END |
				R16_ACCESS_END		|
				R16_CARD_RE		|
				R16_DATA3_CARD_RE	|
				R16_DATA3_CARD_IN);
		pwrite(host->ch, SDHI_R28, R28_SDIO_MASK_ON);
		pwrite(host->ch, SDHI_R26, R26_SDIO_MODE_OFF);
		return IRQ_HANDLED;
	}

	if (state2 & R15_ALL_ERR) {
		pwrite(host->ch, SDHI_R15,
			~R15_ALL_ERR & pread(host->ch, SDHI_R15));
		pwrite(host->ch, SDHI_R17,
			R17_ALL_ERR | pread(host->ch, SDHI_R17));
		g_sd_error[host->ch] = 1;
		g_wait_int[host->ch] = 1;
		wake_up(&intr_wait);
		return IRQ_HANDLED;
	}
	/* Respons End */
	if (state1 & R14_RESP_END) {
		pwrite(host->ch, SDHI_R14,
			~R14_RESP_END & pread(host->ch, SDHI_R14));
		pwrite(host->ch, SDHI_R16,
			R16_RESP_END | pread(host->ch, SDHI_R16));
		g_wait_int[host->ch] = 1;
		wake_up(&intr_wait);
		return IRQ_HANDLED;
	}
	/* SD_BUF Read Enable */
	if (state2 & R15_BRE_ENABLE) {
		pwrite(host->ch, SDHI_R15,
			~R15_BRE_ENABLE & pread(host->ch, SDHI_R15));
		pwrite(host->ch, SDHI_R17, R17_BRE_ENABLE |
			R17_BUF_ILL_READ | pread(host->ch, SDHI_R17));
		g_wait_int[host->ch] = 1;
		wake_up(&intr_wait);
		return IRQ_HANDLED;
	}
	/* SD_BUF Write Enable */
	if (state2 & R15_BWE_ENABLE) {
		pwrite(host->ch, SDHI_R15,
			~R15_BWE_ENABLE & pread(host->ch, SDHI_R15));
		pwrite(host->ch, SDHI_R17, R15_BWE_ENABLE |
			R17_BUF_ILL_WRITE | pread(host->ch, SDHI_R17));
		g_wait_int[host->ch] = 1;
		wake_up(&intr_wait);
		return IRQ_HANDLED;
	}
	/* Access End */
	if (state1 & R14_ACCESS_END) {
		pwrite(host->ch, SDHI_R14,
			~R14_ACCESS_END & pread(host->ch, SDHI_R14));
		pwrite(host->ch, SDHI_R16,
			R14_ACCESS_END | pread(host->ch, SDHI_R16));
		g_wait_int[host->ch] = 1;
		wake_up(&intr_wait);
		return IRQ_HANDLED;
	}
	return IRQ_HANDLED;
}

static irqreturn_t sdhi_sdio_intr(int irq, void *dev_id)
{
	unsigned short state;
	struct sdhi_host *host = dev_id;

	state = pread(host->ch, SDHI_R27);
	if (state & R27_SDIO_IOIRQ) {
		pwrite(host->ch, SDHI_R27,
			~R27_SDIO_IOIRQ & pread(host->ch, SDHI_R27));
		state = pread(host->ch, SDHI_R27);
		if (state & R27_SDIO_IOIRQ)
			mmc_signal_sdio_irq(host->mmc);
	} else if (state & R27_SDIO_EXPUB52)
		pwrite(host->ch, SDHI_R27,
			~R27_SDIO_EXPUB52 & pread(host->ch, SDHI_R27));
	else if (state & R27_SDIO_EXWT)
		pwrite(host->ch, SDHI_R27,
			~R27_SDIO_EXWT & pread(host->ch, SDHI_R27));
	else
		pr_err(DRIVER_NAME": Not Supprt SDIO INT = %x\n",
						pread(host->ch, SDHI_R27));
	return IRQ_HANDLED;
}

static int __devinit sdhi_probe(struct platform_device *pdev)
{
	int ret = 0, irq[2];
	static int firm_check;
	struct mmc_host *mmc;
	struct sdhi_host *host = NULL;

	static unsigned long pfunc[2];
	void(*get_address)(unsigned long *pfunc, void *addr);

	if (pdev->id == 0) {
		ret = request_firmware(&fw_p, "u-code.bin", &(pdev->dev));
		if (ret < 0) {
			pr_err(DRIVER_NAME": Not bios code !! " \
					"Cannot use this driver.\n");
			firm_check = 1;
			return ret;
		}
		get_address = (void *)fw_p->data;
		get_address(pfunc, (void *)fw_p->data);
		pwrite	= (void *)pfunc[0];
		pread	= (void *)pfunc[1];
	}
	if (firm_check == 1)
		return -ENODEV;

	if (pdev->id > CONFIG_MMC_SH_SDHI_NR_CHANNEL - 1) {
		pr_err(DRIVER_NAME": id number '%x' probe error\n", pdev->id);
		return -ENODEV;
	}
	irq[0] = platform_get_irq(pdev, 0);
	irq[1] = platform_get_irq(pdev, 1);
	irq[2] = platform_get_irq(pdev, 2);
	if (irq[0] < 0 || irq[1] < 0 || irq[2] < 0) {
		pr_err(DRIVER_NAME": Get irq error\n");
		return -ENODEV;
	}

	mmc = mmc_alloc_host(sizeof(struct sdhi_host), &pdev->dev);
	if (!mmc)
		return -ENOMEM;

	host = mmc_priv(mmc);
	host->mmc = mmc;
	host->ch = pdev->id;

	mmc->ops = &sdhi_ops;
	mmc->f_min = CLKDEV_INIT;
	mmc->f_max = CLKDEV_HS_DATA;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps = MMC_CAP_4_BIT_DATA 		|
		    MMC_CAP_SD_HIGHSPEED	|
		    MMC_CAP_SDIO_IRQ;
	mmc->max_phys_segs = 32;
	mmc->max_hw_segs = 32;
	mmc->max_blk_count = 256;
	mmc->max_req_size = 131072;
	mmc->max_seg_size = mmc->max_req_size;
	mmc->max_blk_size = mmc->max_req_size;

	sdhi_sync_reset(host->ch);
	pwrite(host->ch, SDHI_R01, USE_PORT0);

#if defined(__BIG_ENDIAN_BITFIELD)
	pwrite(host->ch, SDHI_R120, SET_SWAP);
#endif

	platform_set_drvdata(pdev, host);
	mmc_add_host(mmc);

	ret = request_irq(irq[0], sdhi_intr, 0, "SDHI SD-DETECT", host);
	if (ret) {
		pr_err(DRIVER_NAME": request_irq error (SDHI SD-DETECT)\n");
		mmc_free_host(mmc);
		return ret;
	}
	ret = request_irq(irq[1], sdhi_intr, 0, "SDHI SD-ACCESS", host);
	if (ret) {
		pr_err(DRIVER_NAME": request_irq error (SDHI SD-ACCESS)\n");
		mmc_free_host(mmc);
		return ret;
	}
	ret = request_irq(irq[2], sdhi_sdio_intr, 0, "SDHI SDIO-INT", host);
	if (ret) {
		pr_err(DRIVER_NAME": request_irq error (SDHI SDIO-INT)\n");
		mmc_free_host(mmc);
		return ret;
	}
	pwrite(host->ch, SDHI_R16, R16_RESP_END | R16_ACCESS_END
			| R16_CARD_RE | R16_DATA3_CARD_RE
			| R16_DATA3_CARD_IN);
	return ret;
}

static int __devexit sdhi_remove(struct platform_device *pdev)
{
	struct sdhi_host *host = platform_get_drvdata(pdev);
	int irq[2];

	pwrite(host->ch, SDHI_R16, R16_ALL);
	pwrite(host->ch, SDHI_R17, R17_ALL);

	irq[0] = platform_get_irq(pdev, 0);
	irq[1] = platform_get_irq(pdev, 1);
	irq[2] = platform_get_irq(pdev, 2);

	platform_set_drvdata(pdev, NULL);
	mmc_remove_host(host->mmc);

	free_irq(irq[0], host);
	free_irq(irq[1], host);
	free_irq(irq[2], host);

	mmc_free_host(host->mmc);
	if (pdev->id == 0)
		release_firmware(fw_p);
	return 0;
}

static struct platform_driver sdhi_driver = {
	.probe		= sdhi_probe,
	.remove		= sdhi_remove,
	.driver		= {
		.name	= DRIVER_NAME,
	},
};

static int __init sdhi_init(void)
{
	return platform_driver_register(&sdhi_driver);
}

static void __exit sdhi_exit(void)
{
	platform_driver_unregister(&sdhi_driver);
}

module_param(g_timeout, long, 0444);

module_init(sdhi_init);
module_exit(sdhi_exit);


MODULE_DESCRIPTION("The SD interface driver of the SuperH internal");
MODULE_LICENSE("GPL");
MODULE_ALIAS(DRIVER_NAME);
