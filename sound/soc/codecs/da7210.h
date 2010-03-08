/*
 * da7210.h  --  audio driver for da7210
 *
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#ifndef _DA7210_H
#define _DA7210_H

struct da7210_setup_data {
   int i2c_bus;
   unsigned short i2c_address;
};


#define DA7210_DAI_IIS 0

extern struct snd_soc_dai da7210_dai[];
extern struct snd_soc_codec_device soc_codec_dev_da7210;

#endif

