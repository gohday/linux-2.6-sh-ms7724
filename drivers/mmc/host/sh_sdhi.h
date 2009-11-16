/*
 * linux/drivers/mmc/host/sh-sdhi.h
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

#ifndef _SH_SDHI_H_
#define _SH_SDHI_H_

/* R00 */
#define R00_MASK			0x0000ffff
#define R00_SD_APP_SEND_SCR		0x0073
#define R00_SD_SWITCH			0x1C06
#define R00_SD_IO_SEND_OP_COND		0x0705
#define R00_SD_IO_RW_DIRECT		0x0434
#define R00_SD_IO_RW_EXTENDED_SREAD	0x1C35
#define R00_SD_IO_RW_EXTENDED_MREAD	0x7C35
#define R00_SD_IO_RW_EXTENDED_SWRITE	0x0C35
#define R00_SD_IO_RW_EXTENDED_MWRITE	0x6C35

#define R02_MASK			0x0000ffff
#define R03_MASK			0x0000ffff

/* R04 */
#define R04_SEC_ENABLE		(0x0100)

/* R14 */
#define R14_RESP_END		(1 << 0)
#define R14_ACCESS_END		(1 << 2)
#define R14_CARD_RE		(1 << 3)
#define R14_CARD_IN		(1 << 4)
#define R14_ISD0CD		(1 << 5)
#define R14_WRITE_PRO		(1 << 7)
#define R14_DATA3_CARD_RE	(1 << 8)
#define R14_DATA3_CARD_IN	(1 << 9)
#define R14_DATA3		(1 << 10)

/* R15 */
#define R15_CMD_ERROR		(1 << 0)
#define R15_CRC_ERROR		(1 << 1)
#define R15_END_ERROR		(1 << 2)
#define R15_TIMEOUT		(1 << 3)
#define R15_BUF_ILL_WRITE	(1 << 4)
#define R15_BUF_ILL_READ	(1 << 5)
#define R15_RESP_TIMEOUT	(1 << 6)
#define R15_SDDAT0		(1 << 7)
#define R15_BRE_ENABLE		(1 << 8)
#define R15_BWE_ENABLE		(1 << 9)
#define R15_CBUSY		(1 << 14)
#define R15_ILA			(1 << 15)
#define R15_ALL_ERR		(0x807f)

/* R16 */
#define R16_RESP_END		(1 << 0)
#define R16_ACCESS_END		(1 << 2)
#define R16_CARD_RE		(1 << 3)
#define R16_CARD_IN		(1 << 4)
#define R16_DATA3_CARD_RE	(1 << 8)
#define R16_DATA3_CARD_IN	(1 << 9)
#define R16_ALL			(0xffff)
#define R16_SET			(R16_RESP_END |		\
				 R16_ACCESS_END |	\
				 R16_DATA3_CARD_RE |	\
				 R16_DATA3_CARD_IN)

/* R17 */
#define R17_CMD_ERROR		(1 << 0)
#define R17_CRC_ERROR		(1 << 1)
#define R17_END_ERROR		(1 << 2)
#define R17_TIMEOUT		(1 << 3)
#define R17_BUF_ILL_WRITE	(1 << 4)
#define R17_BUF_ILL_READ	(1 << 5)
#define R17_RESP_TIMEOUT	(1 << 6)
#define R17_BRE_ENABLE		(1 << 8)
#define R17_BWE_ENABLE		(1 << 9)
#define R17_ILA			(1 << 15)
#define R17_ALL			0xffff
#define R17_ALL_ERR		0x807f

/* R22 */
#define R22_CRC_ERROR		((1 << 11) | (1 << 10) | (1 << 9) |	\
				 (1 << 8) | (1 << 5))
#define R22_CMD_ERROR		((1 << 4) | (1 << 3) | (1 << 2) |	\
				 (1 << 1) | (1 << 0))

/* R23 */
#define R23_RES_TIMEOUT		0x0001
#define R23_RES_STOP_TIMEOUT	0x0003
#define R23_SYS_ERROR		((1 << 6) | (1 << 5) | (1 << 4) |	\
				 (1 << 3) | (1 << 2) | (1 << 1) |	\
				 (1 << 0))

/* R02 */
#define USE_PORT0	0x0100		/* port 0 */

/* R20 */
#define BUS_WIDTH_1	0x8000		/* bus width = 1 bit */

/* R120 */
#define SET_SWAP	0x00C0		/* SWAP */

/* R18 */
#define R18_ENABLE	0x0100
#define R18_INIT	0x0040

#if defined(CONFIG_CPU_SUBTYPE_SH7723) || \
    defined(CONFIG_CPU_SUBTYPE_SH7722) || \
    defined(CONFIG_CPU_SUBTYPE_SH7366)
#define R18_MMC_TRANS	0x0001		/* for MMC */
#define R18_TRANS	0x0001		/* for SD */
#define R18_HS_TRANS	0x0000
#else
#define R18_MMC_TRANS	0x0000		/* for MMC */
#define R18_TRANS	0x0000		/* for SD */
#if defined(CONFIG_BCLK_OPTION_SUPPORT)
#define R18_HS_TRANS	0x00ff
#else
#define R18_HS_TRANS	0x0000
#endif /* BCLK_OPTION_SUPPORT */
#endif /* CONFIG_CPU_SUBTYPE_SH7723 */

/* R26 */
#define R26_SDIO_MODE_ON	0x0001
#define R26_SDIO_MODE_OFF	0x0000

/* R27 */
#define R27_SDIO_IOIRQ		0x0001
#define R27_SDIO_EXPUB52	0x4000
#define R27_SDIO_EXWT		0x8000

/* R28 */
#define R28_SDIO_MASK_CLEAR	0x0006
#define R28_SDIO_MASK_ON	0xC007

/* R112 */
#define R112_SDRST_ON		0x0000
#define R112_SDRST_OFF		0x0001

#define	IMCLK		33000000	/* SDHI input clock */
#define	CLKDEV_SD_DATA	25000000	/* 25 MHz */
#define CLKDEV_HS_DATA	50000000	/* 50 MHz */
#define CLKDEV_MMC_DATA	20000000	/* 20MHz */
#define	CLKDEV_INIT	400000		/* 100 - 400 KHz */

struct sdhi_host {
	struct mmc_host	*mmc;
	struct mmc_data	*data;
	unsigned int	clock;
	unsigned int	power_mode;
	int		ch;
	spinlock_t	lock;
};

static DECLARE_WAIT_QUEUE_HEAD(intr_wait);
static unsigned short g_wait_int[CONFIG_MMC_SH_SDHI_NR_CHANNEL];
static unsigned short g_sd_error[CONFIG_MMC_SH_SDHI_NR_CHANNEL];
static long g_timeout = 1000;		/* Default (1 = 10ms) */
const struct firmware *fw_p;

/* firmware call */
void (*pwrite)(int ch, unsigned char add, unsigned short data);
unsigned short (*pread)(int ch, unsigned char add);

/* firmware I/F */
enum sd_param {
	SDHI_R00 = 1,
	SDHI_R01,	SDHI_R02,	SDHI_R03,	SDHI_R04,
	SDHI_R05,	SDHI_R06,	SDHI_R07,	SDHI_R08,
	SDHI_R09,	SDHI_R10,	SDHI_R11,	SDHI_R12,
	SDHI_R13,	SDHI_R14,	SDHI_R15,	SDHI_R16,
	SDHI_R17,	SDHI_R18,	SDHI_R19,	SDHI_R20,
	SDHI_R22,	SDHI_R23,	SDHI_R24,	SDHI_R26,
	SDHI_R27,	SDHI_R28,	SDHI_R108,	SDHI_R112,
	SDHI_R113,	SDHI_R120,	SDHI_R121,	SDHI_R122,
	SDHI_R123,	SDHI_R124,	SDHI_R125,	SDHI_R126,
	SDHI_R127,
	SDHI_APP = 50
};

#endif /* _SH_SDHI_H_ */
