/*
 * da7210.c  --  DA7210 ALSA Soc Audio driver
 *
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <sound/initval.h>
#include <asm/div64.h>

#include "da7210.h"
#include "da7210_regs.h"

#define DA7210_VERSION "2.0"

#define DA7210_ENABLE_VF 1

/* Codec private data */
struct da7210_priv {
    int a; 
    int b; 
}; /* currently not used */

/*
 * Register cache
 * We can't read the DA7210 register space when we
 * are using 2 wire for device control, so we cache them instead.
 */
static const u8 da7210_reg[] = {
    0x00,0x11,0x00,0x00,0x00,0x00,0x00,0x00, /*R0 - R7*/ 
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x08, /*R8 - RF*/
    0x00,0x00,0x00,0x00,0x08,0x10,0x10,0x54, /*R10 - R17*/
    0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*R18 - R1F*/
    0x00,0x00,0x00,0x02,0x00,0x76,0x00,0x00, /*R20 - R27*/
    0x04,0x00,0x00,0x30,0x2A,0x00,0x40,0x00, /*R28 - R2F*/
    0x40,0x00,0x40,0x00,0x40,0x00,0x40,0x00, /*R30 - R37*/
    0x40,0x00,0x40,0x00,0x40,0x00,0x00,0x00, /*R38 - R3F*/
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*R40 - R4F*/
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*R48 - R4F*/
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*R50 - R57*/
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*R58 - R5F*/
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*R60 - R67*/
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*R68 - R6F*/
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*R70 - R77*/
    0x00,0x00,0x00,0x00,0x00,0x54,0x54,0x00, /*R78 - R7F*/
    0x00,0x00,0x2C,0x00,0x00,0x00,0x00,0x00, /*R80 - R87*/
    0x00,                                    /*R88*/
};

/* 
 * Read from the da7210 register space using I2C. 
 * Use this to read non-cached & volatile registers.
 */
static u8 da7210_I2C_read_reg(struct i2c_client *client, int reg)
{
    struct i2c_adapter *adapter = client->adapter;
    int ret = 0;
    struct i2c_msg i2cmsg;
    u8 buf[1] = {0};
    
    /* register offset > 256 is not allowed */
    if (reg > 0xFF) {
        return -1;
    }
    
    buf[0] = reg;
    i2cmsg.addr  = client->addr;
    i2cmsg.len   = 1;
    i2cmsg.buf   = buf;
    i2cmsg.flags = 0;
    ret = i2c_transfer(adapter, &i2cmsg, 1);
    if (ret < 0) {
        printk(KERN_ALERT "master_xfer Failed!\n");
    }

    i2cmsg.addr  = client->addr;
    i2cmsg.len   = 1;
    i2cmsg.buf   = buf;
    
    /* To read the data on I2C set flag to I2C_M_RD */
    i2cmsg.flags = I2C_M_RD; 
    
    /* Start the i2c transfer by calling host i2c driver function */
    ret = i2c_transfer(adapter, &i2cmsg, 1);
    if (ret < 0) {
        printk(KERN_ALERT "master_xfer Failed!\n");
    }
    
    return buf[0];
}

/*
 * Read da7210 register cache
 */
static inline unsigned int da7210_read_reg_cache(struct snd_soc_codec *codec,
    unsigned int reg)
{
    u8 *cache = codec->reg_cache;
    BUG_ON(reg > ARRAY_SIZE(da7210_reg));
    return cache[reg];
}

/*
 * Write da7210 register cache
 */
static inline void da7210_write_reg_cache(struct snd_soc_codec *codec,
    unsigned int reg, unsigned int value)
{
    u8 *cache = codec->reg_cache;

    cache[reg] = value;
}

/*
 * Write to the da7210 register space
 */
static int da7210_write(struct snd_soc_codec *codec, unsigned int reg,
    unsigned int value)
{

    u8 data[2];
    
    /* data[0] da7210 register offset */
    /* data[1] register data */
    data[0] = reg & 0xff;
    data[1] = value & 0xff;
    
    /* I2C write debug Message*/
    /* printk(KERN_ALERT "Da7210: 12C write reg[%x]=0x%x\n",data[0],data[1]); */
    
    /* small delay */
    msleep(1);
    
    /* write the cache only if hardware write is successful */
    if (codec->hw_write(codec->control_data, data, 2) == 2) {
        if (data[0] < ARRAY_SIZE(da7210_reg)) {
            da7210_write_reg_cache(codec, data[0], data[1]);
        }
        return 0;
    }
    else {
            printk(KERN_ALERT "Da7210 Codec: I2C write Failed!\n");    
            return -EIO;
    }

    return 0;
}

/*
 * Read from the da7210 register space.
 */
static inline unsigned int da7210_read(struct snd_soc_codec *codec,
                       unsigned int reg)
{
    struct i2c_client *client = codec->control_data;

    switch (reg) {
    case DA7210_STATUS:
        return da7210_I2C_read_reg(client, reg);
        break;  

    default:
        return da7210_read_reg_cache(codec, reg);
    }
    return 0;
}

static const char *da7210_mic_bias_voltage[] = {"1.5", "1.6", "2.2", "2.3"};

static const struct soc_enum da7210_enum[] = {
    SOC_ENUM_SINGLE(DA7210_MIC_L, 4, 4, da7210_mic_bias_voltage),
};

/* Add non DAPM controls */
static const struct snd_kcontrol_new da7210_snd_controls[] = {
    /* Mixer Playback controls */
    SOC_DOUBLE_R("DAC Gain", DA7210_DAC_L,DA7210_DAC_R, 0, 0x37, 1),
    SOC_DOUBLE_R("HeadPhone Playback Volume", DA7210_HP_L_VOL,\
                                                   DA7210_HP_R_VOL, 0, 0x3f, 0),
    /* Mixer Capture controls */
    SOC_DOUBLE_R("Mic Capture Volume", DA7210_MIC_L, DA7210_MIC_R, 0, 0x07, 0),
    SOC_DOUBLE("In PGA Gain", DA7210_IN_GAIN, 0, 4, 0x0F, 0),
    SOC_SINGLE("Mic Bias", DA7210_MIC_L, 6, 1, 0),
    SOC_ENUM("Mic Bias Voltage", da7210_enum[0]), 
};

/* ----------------------------Capture Mixers--------------------------- */

/* In Mixer Left */
static const struct snd_kcontrol_new da7210_in_left_mixer_controls[] = {
    SOC_DAPM_SINGLE("MIC_L Switch",  DA7210_INMIX_L, 0, 1, 0),
    SOC_DAPM_SINGLE("MIC_R Switch",  DA7210_INMIX_L, 1, 1, 0),
    SOC_DAPM_SINGLE("Aux1_L Switch", DA7210_INMIX_L, 2, 1, 0),
    SOC_DAPM_SINGLE("Aux2 Switch",   DA7210_INMIX_L, 3, 1, 0),
    SOC_DAPM_SINGLE("DAC_L Switch",  DA7210_INMIX_L, 4, 1, 0),
};

/* In Mixer Right */
static const struct snd_kcontrol_new da7210_in_right_mixer_controls[] = {
    SOC_DAPM_SINGLE("MIC_R Switch",  DA7210_INMIX_R, 0, 1, 0),
    SOC_DAPM_SINGLE("MIC_L Switch",  DA7210_INMIX_R, 1, 1, 0),
    SOC_DAPM_SINGLE("Aux1_R Switch", DA7210_INMIX_R, 2, 1, 0),
    SOC_DAPM_SINGLE("Aux2 Switch",   DA7210_INMIX_R, 3, 1, 0),
    SOC_DAPM_SINGLE("DAC_R Switch",  DA7210_INMIX_R, 4, 1, 0),
    SOC_DAPM_SINGLE("INPGA_L Switch",DA7210_INMIX_R, 5, 1, 0),
};

/*----------------------------Playback Mixers---------------------------*/
/* Out Mixer Left */
static const struct snd_kcontrol_new da7210_out_mixer_left_controls[] = {
    SOC_DAPM_SINGLE("AUX1_L Switch", DA7210_OUTMIX_L, 0, 1, 0),
    SOC_DAPM_SINGLE("AUX2 Switch",   DA7210_OUTMIX_L, 1, 1, 0),
    SOC_DAPM_SINGLE("IN_L Switch",   DA7210_OUTMIX_L, 2, 1, 0),
    SOC_DAPM_SINGLE("IN_R Switch",   DA7210_OUTMIX_L, 3, 1, 0),
    SOC_DAPM_SINGLE("DAC_L Switch",  DA7210_OUTMIX_L, 4, 1, 0),
};

/* Out Mixer Right */
static const struct snd_kcontrol_new da7210_out_mixer_right_controls[] = {
    SOC_DAPM_SINGLE("AUX1_R Switch", DA7210_OUTMIX_R, 0, 1, 0),
    SOC_DAPM_SINGLE("AUX2 Switch",   DA7210_OUTMIX_R, 1, 1, 0),
    SOC_DAPM_SINGLE("IN_L Switch",   DA7210_OUTMIX_R, 2, 1, 0),
    SOC_DAPM_SINGLE("IN_R Switch",   DA7210_OUTMIX_R, 3, 1, 0),
    SOC_DAPM_SINGLE("DAC_R Switch",  DA7210_OUTMIX_R, 4, 1, 0),
};

/* Mono Mixer */
static const struct snd_kcontrol_new da7210_mono_mixer_controls[] = {
    SOC_DAPM_SINGLE("IN_L Switch",  DA7210_OUT2, 3, 1, 0),
    SOC_DAPM_SINGLE("IN_R Switch",  DA7210_OUT2, 4, 1, 0),
    SOC_DAPM_SINGLE("DAC_L Switch", DA7210_OUT2, 5, 1, 0),
    SOC_DAPM_SINGLE("DAC_R Switch", DA7210_OUT2, 6, 1, 0),
};

static const struct snd_kcontrol_new da7210_headphone_left_control = \
                            SOC_DAPM_SINGLE("Switch", DA7210_STARTUP2, 3, 1, 1);
static const struct snd_kcontrol_new da7210_headphone_right_control = \
                            SOC_DAPM_SINGLE("Switch", DA7210_STARTUP2, 4, 1, 1);
static const struct snd_kcontrol_new da7210_MicIn_left_control = \
                            SOC_DAPM_SINGLE("Switch", DA7210_STARTUP3, 0, 1, 1);
static const struct snd_kcontrol_new da7210_MicIn_right_control = \
                            SOC_DAPM_SINGLE("Switch", DA7210_STARTUP3, 1, 1, 1);

/* DAPM widgets */
static const struct snd_soc_dapm_widget da7210_dapm_widgets[] = {
    /* DAMP stream domain - DAC. Enabled when playback is started */
    SND_SOC_DAPM_DAC("DAC Left", "Playback", DA7210_STARTUP2, 5, 1),
    SND_SOC_DAPM_DAC("DAC Right", "Playback", DA7210_STARTUP2, 6, 1),

    /* DAMP stream domain - ADC. Enabled when capture is started */
    SND_SOC_DAPM_ADC("ADC Left", "Capture", DA7210_STARTUP3, 5, 1),
    SND_SOC_DAPM_ADC("ADC Right", "Capture", DA7210_STARTUP3, 6, 1),

    /* DAPM path domain - switches and mixers */ 
    /* Automatically set when mixer settings are changed by the user */
    SND_SOC_DAPM_SWITCH("HPL Enable", SND_SOC_NOPM, 0, 0,
        &da7210_headphone_left_control),
    SND_SOC_DAPM_SWITCH("HPR Enable", SND_SOC_NOPM, 0, 0,
        &da7210_headphone_right_control),
    SND_SOC_DAPM_SWITCH("MicL Enable", SND_SOC_NOPM, 0, 0,
        &da7210_MicIn_left_control),
    SND_SOC_DAPM_SWITCH("MicR Enable", SND_SOC_NOPM, 0, 0,
        &da7210_MicIn_right_control),

    SND_SOC_DAPM_MIXER("Out Mixer Left", SND_SOC_NOPM, 0, 0,
        &da7210_out_mixer_left_controls[0],
        ARRAY_SIZE(da7210_out_mixer_left_controls)),

    SND_SOC_DAPM_MIXER("Out Mixer Right", SND_SOC_NOPM, 0, 0,
        &da7210_out_mixer_right_controls[0],
        ARRAY_SIZE(da7210_out_mixer_right_controls)),

    SND_SOC_DAPM_MIXER("In Mixer Left", SND_SOC_NOPM, 0, 0,
        &da7210_in_left_mixer_controls[0],
        ARRAY_SIZE(da7210_in_left_mixer_controls)),

    SND_SOC_DAPM_MIXER("In Mixer Right", SND_SOC_NOPM, 0, 0,
        &da7210_in_right_mixer_controls[0],
        ARRAY_SIZE(da7210_in_right_mixer_controls)),

    /* DAPM Platform domain. Physically connected input and ouput pins */
    SND_SOC_DAPM_OUTPUT("HPL"),   /*Headphone Out left*/
    SND_SOC_DAPM_OUTPUT("HPR"),   /*Headphone out Right*/
    SND_SOC_DAPM_INPUT("MICL"),   /*MicIn left*/
    SND_SOC_DAPM_INPUT("MICR"),   /*MicIn Right*/
};

/* DAPM audio route definition */
static const struct snd_soc_dapm_route audio_map[] = {
    /*Out Mixer Left*/
    {"Out Mixer Left", "DAC_L Switch", "DAC Left"},

    /*Out Mixer Right*/
    {"Out Mixer Right", "DAC_R Switch", "DAC Right"},

    /*In Mixer Left*/
    {"In Mixer Left", "MIC_L Switch", "MicL Enable"},

    /*In Mixer Right*/
    {"In Mixer Right", "MIC_R Switch", "MicR Enable"},
    
    /*HPL*/
    {"HPL", NULL, "HPL Enable"},
    {"HPL Enable", "Switch", "Out Mixer Left"},
        
    /*HPR*/
    {"HPR", NULL, "HPR Enable"},
    {"HPR Enable", "Switch", "Out Mixer Right"},

    /*MICL*/
    {"ADC Left", NULL, "In Mixer Left"},
    {"MicL Enable", "Switch", "MICL"},
    
    /*MICR*/
    {"ADC Right", NULL, "In Mixer Right"},
    {"MicR Enable", "Switch", "MICR"},
};

static int da7210_add_controls(struct snd_soc_codec *codec)
{
    int err, i;

    for (i = 0; i < ARRAY_SIZE(da7210_snd_controls); i++) {
        err = snd_ctl_add(codec->card, snd_soc_cnew(&da7210_snd_controls[i],\
                                  codec, NULL));
        if (err < 0)
            return err;
    }
    return 0;
}

static int da7210_add_widgets(struct snd_soc_codec *codec)
{
    snd_soc_dapm_new_controls(codec, da7210_dapm_widgets,
                                               ARRAY_SIZE(da7210_dapm_widgets));
    snd_soc_dapm_add_routes(codec, audio_map, ARRAY_SIZE(audio_map));
    snd_soc_dapm_new_widgets(codec);
    return 0;
}

/*
 * Set PCM DAI word length.
 * Enable Voice Filter if Fs <= 16KHz 
 */
static int da7210_hw_params(struct snd_pcm_substream *substream,
    struct snd_pcm_hw_params *params)
{
    struct snd_soc_pcm_runtime *rtd = substream->private_data;
    //struct snd_soc_dai_link *dai = rtd->dai;
    struct snd_soc_device *socdev = rtd->socdev;
    struct snd_soc_codec *codec = socdev->codec;
    unsigned int dai_cfg1;
    unsigned int value;

    dai_cfg1 = da7210_read(codec, DA7210_DAI_CFG1);

    switch (params_format(params)) {
        case SNDRV_PCM_FORMAT_S16_LE:
            dai_cfg1 = (dai_cfg1 & 0xFC) | DA7210_DAI_WORD_S16_LE; 
            break;
        case SNDRV_PCM_FORMAT_S20_3LE:
            dai_cfg1 = (dai_cfg1 & 0xFC) | DA7210_DAI_WORD_S20_LE; 
            break;
        case SNDRV_PCM_FORMAT_S24_LE:
            dai_cfg1 = (dai_cfg1 & 0xFC) | DA7210_DAI_WORD_S24_LE; 
            break;
        case SNDRV_PCM_FORMAT_S32_LE:
            dai_cfg1 = (dai_cfg1 & 0xFC) | DA7210_DAI_WORD_S32_LE; 
            break;
        default:
            return -EINVAL;
    }

    da7210_write(codec, DA7210_DAI_CFG1, dai_cfg1);

#ifdef DA7210_ENABLE_VF
    switch (params_rate(params)) {
        case 8000:
        case 11025:
        case 12000:
        case 16000:
            if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {    
                value = da7210_read_reg_cache(codec,DA7210_DAC_HPF);
                da7210_write(codec, DA7210_DAC_HPF, ((value & 0x8F) |\
                                 DA7210_DAC_VOICE_F0_25 | DA7210_DAC_VOICE_EN));
            } else {
                value = da7210_read_reg_cache(codec,DA7210_ADC_HPF);
                da7210_write(codec, DA7210_ADC_HPF, ((value & 0x8F) |\
                                DA7210_ADC_VOICE_F0_25 | DA7210_ADC_VOICE_EN ));
            }
            break;

        case 22050:
        case 32000:
        case 44100: 
        case 48000:
        case 88100:
        case 96000:
            if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
                value = da7210_read_reg_cache(codec, DA7210_DAC_HPF);
                da7210_write(codec, DA7210_DAC_HPF, (value & ~DA7210_DAC_VOICE_EN));
            } else {
                value = da7210_read_reg_cache(codec, DA7210_ADC_HPF);
                da7210_write(codec, DA7210_ADC_HPF, (value & ~DA7210_ADC_VOICE_EN));
            }
            break;
        default:
            return -EINVAL;
    }
#endif
    return 0;
}

/*
 * Set DAI mode and Format
 */
static int da7210_set_dai_fmt(struct snd_soc_dai *codec_dai,
                      unsigned int fmt)
{
    struct snd_soc_codec *codec = codec_dai->codec;
    unsigned int dai_cfg1;
    unsigned int dai_cfg3;
    
    dai_cfg1 = da7210_read(codec, DA7210_DAI_CFG1);
    dai_cfg3 = da7210_read(codec, DA7210_DAI_CFG3);

    switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
        case SND_SOC_DAIFMT_CBS_CFS:
            dai_cfg1 = (dai_cfg1 & 0x7f) | DA7210_DAI_MODE_SLAVE;
            break;
        case SND_SOC_DAIFMT_CBM_CFM:
            dai_cfg1 = (dai_cfg1 & 0x7f) | DA7210_DAI_MODE_MASTER;
            break;
        default:
            return -EINVAL;
    }

    switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
        case SND_SOC_DAIFMT_I2S:
            dai_cfg3 = (dai_cfg3 & 0xFC) | DA7210_DAI_FORMAT_I2SMODE;
            break;
        case SND_SOC_DAIFMT_RIGHT_J:
            dai_cfg3 = (dai_cfg3 & 0xFC) | DA7210_DAI_FORMAT_RIGHT_J;
            break;
        case SND_SOC_DAIFMT_LEFT_J:
            dai_cfg3 = (dai_cfg3 & 0xFC) | DA7210_DAI_FORMAT_LEFT_J;
            break;
        default:
            return -EINVAL;
    }

    da7210_write(codec, DA7210_DAI_CFG1, dai_cfg1);
    da7210_write(codec, DA7210_DAI_CFG3, dai_cfg3);

    return 0;
}

static int da7210_set_dai_clkdiv(struct snd_soc_dai *codec_dai,
                 int div_id, int div)
{
    /* For codec clock configuration */
    return 0;
}

static int da7210_set_dai_pll(struct snd_soc_dai *codec_dai,
        int pll_id, unsigned int freq_in, unsigned int freq_out)
{
    /* For codec PLL configuration in Slave Mode */
    return 0;
}


static int da7210_set_bias_level(struct snd_soc_codec *codec,
    enum snd_soc_bias_level level)
{
    /* For codec Power Management */
    return 0;
}

#define DA7210_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |\
            SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE)

struct snd_soc_dai da7210_dai[] = {
    {
        .name = "DA7210 IIS",
        .id = 0,
        /* playback capabilities */
        .playback = {
            .stream_name = "Playback",
            .channels_min = 1,
            .channels_max = 2,
            .rates = SNDRV_PCM_RATE_8000_96000,
            .formats = DA7210_FORMATS,
        },
        /* capture capabilities */
        .capture = {
            .stream_name = "Capture",
            .channels_min = 1,
            .channels_max = 2,
            .rates = SNDRV_PCM_RATE_8000_96000,
            .formats = DA7210_FORMATS,
        },
        /* pcm operations */
        .ops = {
             .hw_params = da7210_hw_params,
        },
        /* DAI operations */
        .dai_ops = {
             .set_fmt = da7210_set_dai_fmt,
             .set_clkdiv = da7210_set_dai_clkdiv,
             .set_pll = da7210_set_dai_pll,
        },
    },
};
EXPORT_SYMBOL_GPL(da7210_dai);

/*
 * Initialize the DA7210 driver
 * register the mixer and dsp interfaces with the kernel
 */
static int da7210_init(struct snd_soc_device *socdev)
{

    struct snd_soc_codec *codec = socdev->codec;
    int ret=0;

    codec->name = "DA7210";
    codec->owner = THIS_MODULE;
    codec->read = da7210_read;
    codec->write = da7210_write;
    codec->set_bias_level = da7210_set_bias_level;
    codec->dai = da7210_dai;
    codec->num_dai = ARRAY_SIZE(da7210_dai);
    codec->reg_cache_size = ARRAY_SIZE(da7210_reg);
    codec->reg_cache = kmemdup(da7210_reg, sizeof(da7210_reg), GFP_KERNEL);

    if (codec->reg_cache == NULL)
        return -ENOMEM;    

    /*
     * ADC settings 
     */

    /* Enable Left & Right MIC PGA and Mic Bias */
    da7210_write(codec, DA7210_MIC_L, DA7210_MIC_L_EN | DA7210_MICBIAS_EN);
    da7210_write(codec, DA7210_MIC_R, DA7210_MIC_R_EN);

    /* Enable Left and Right input PGA */
    da7210_write(codec, DA7210_INMIX_L, DA7210_IN_L_EN);
    da7210_write(codec, DA7210_INMIX_R, DA7210_IN_R_EN);

     /* Enable ADC Highpass Filter */
    da7210_write(codec, DA7210_ADC_HPF, DA7210_ADC_HPF_EN);

    /* Enable Left and Right ADC */
    da7210_write(codec, DA7210_ADC, DA7210_ADC_L_EN | DA7210_ADC_R_EN);

    /* 
     * DAC settings 
     */
                
    /* Enable DAC Highpass Filter */
    da7210_write(codec, DA7210_DAC_HPF, DA7210_DAC_HPF_EN);

    /* Enable Left and Right DAC */
    da7210_write(codec, DA7210_DAC_SEL,    \
                    DA7210_DAC_L_SRC_DAI_L | DA7210_DAC_L_EN \
                  | DA7210_DAC_R_SRC_DAI_R | DA7210_DAC_R_EN);

    /* Enable Left and Right out PGA */
    da7210_write(codec, DA7210_OUTMIX_L, DA7210_OUT_L_EN);
    da7210_write(codec, DA7210_OUTMIX_R, DA7210_OUT_R_EN);

    /* Enable Left and Right HeadPhone PGA */
    da7210_write(codec, DA7210_HP_CFG, DA7210_HP_2CAP_MODE | DA7210_HP_SENSE_EN \
                      | DA7210_HP_L_EN | DA7210_HP_MODE | DA7210_HP_R_EN);

    /* set DAI source to Left and Right ADC */
    da7210_write(codec, DA7210_DAI_SRC_SEL,
                    DA7210_DAI_OUT_R_SRC | DA7210_DAI_OUT_L_SRC);

    /* Enable DAI */
    da7210_write(codec, DA7210_DAI_CFG3, DA7210_DAI_OE | DA7210_DAI_EN);

    /* Diable PLL and bypass it */
    da7210_write(codec, DA7210_PLL, DA7210_PLL_FS_48000);

    /* Bypass PLL and set MCLK freq rang to 10-20MHz */
    da7210_write(codec, DA7210_PLL_DIV3, 
                DA7210_MCLK_RANGE_10_20_MHZ | DA7210_PLL_BYP);

    /* Enable standbymode */
    da7210_write(codec, DA7210_STARTUP2, DA7210_LOUT1_L_STBY |\
         DA7210_LOUT1_R_STBY | DA7210_LOUT2_STBY | DA7210_HP_L_STBY |\
                      DA7210_HP_R_STBY | DA7210_DAC_L_STBY | DA7210_DAC_R_STBY);
    da7210_write(codec, DA7210_STARTUP3, DA7210_LIN1_L_STBY |\
         DA7210_LIN1_R_STBY | DA7210_LIN2_STBY | DA7210_MIC_L_STBY |\
                      DA7210_MIC_R_STBY | DA7210_ADC_L_STBY | DA7210_ADC_R_STBY);
                  
    /* Activate all enabled subsystem */
    da7210_write(codec, DA7210_STARTUP1, DA7210_SC_MST_EN);


    /* Register pcms */
    ret = snd_soc_new_pcms(socdev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1);
    if (ret < 0) {
        goto pcm_err;
    }
       
    /* Add the dapm controls */
    da7210_add_controls(codec);
    da7210_add_widgets(codec);

    /* Register the SoC sound card */
    ret = snd_soc_register_card(socdev);
    if (ret < 0) {
        goto card_err;
    }
    return ret;

card_err:
    snd_soc_free_pcms(socdev);
    snd_soc_dapm_free(socdev);
pcm_err:
    kfree(codec->reg_cache);
    return ret;
}

/* If the i2c layer weren't so broken, we could pass this kind of data
   around */
static struct snd_soc_device *da7210_socdev;

#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)

static int da7210_i2c_probe(struct i2c_client *i2c,
                const struct i2c_device_id *id)
{
    struct snd_soc_device *socdev = da7210_socdev;
    struct snd_soc_codec *codec = socdev->codec;
    int ret;

    i2c_set_clientdata(i2c, codec);

    codec->control_data = i2c;

    ret = da7210_init(socdev);
    if (ret < 0)
        pr_err("Failed to initialise da7210 audio codec\n");

    return ret;
}

static int da7210_i2c_remove(struct i2c_client *client)
{
    struct snd_soc_codec *codec = i2c_get_clientdata(client);
    kfree(codec->reg_cache);
    return 0;
}

static const struct i2c_device_id da7210_i2c_id[] = {
    { "da7210", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, da7210_i2c_id);

/* I2C codec control layer */
static struct i2c_driver da7210_i2c_driver = {
    .driver = {
        .name = "DA7210 I2C Codec",
        .owner = THIS_MODULE,
    },
    .probe = da7210_i2c_probe,
    .remove =  __devexit_p(da7210_i2c_remove),
    .id_table = da7210_i2c_id, 
};

/*
 * Register the DA7210 I2C driver
 */
static int da7210_add_i2c_device(struct platform_device *pdev,
                 const struct da7210_setup_data *setup)
{
    struct i2c_board_info info;
    struct i2c_adapter *adapter;
    struct i2c_client *client;
    int ret;

    ret = i2c_add_driver(&da7210_i2c_driver);
    if (ret != 0) {
        dev_err(&pdev->dev, "can't add i2c driver\n");
        return ret;
    }

    memset(&info, 0, sizeof(struct i2c_board_info));
    info.addr = setup->i2c_address;
    strlcpy(info.type, "da7210", I2C_NAME_SIZE);

    adapter = i2c_get_adapter(setup->i2c_bus);
    if (!adapter) {
        dev_err(&pdev->dev, "can't get i2c adapter %d\n",
            setup->i2c_bus);
        goto err_driver;
    }

    client = i2c_new_device(adapter, &info);
    i2c_put_adapter(adapter);

    if (!client) {
        dev_err(&pdev->dev, "can't add i2c device at 0x%x\n",
            (unsigned int)info.addr);
        goto err_driver;
    }

    return 0;

err_driver:
    i2c_del_driver(&da7210_i2c_driver);
    return -ENODEV;
}

#endif

static int da7210_probe(struct platform_device *pdev)
{
    struct snd_soc_device *socdev = platform_get_drvdata(pdev);
    struct da7210_setup_data *setup;
    struct snd_soc_codec *codec;
    struct da7210_priv *da7210;
    int ret = 0;     

    pr_info("DA7210 Audio Codec %s\n", DA7210_VERSION);

    setup = socdev->codec_data;
    codec = kzalloc(sizeof(struct snd_soc_codec), GFP_KERNEL);
    if (codec == NULL)
        return -ENOMEM;

    da7210 = kzalloc(sizeof(struct da7210_priv), GFP_KERNEL);
    if (da7210 == NULL) {
        kfree(codec);
        return -ENOMEM;
    }

    codec->private_data = da7210;
    socdev->codec = codec;
    mutex_init(&codec->mutex);
    INIT_LIST_HEAD(&codec->dapm_widgets);
    INIT_LIST_HEAD(&codec->dapm_paths);
    da7210_socdev = socdev;

#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
    if (setup->i2c_address) {
        codec->hw_write = (hw_write_t)i2c_master_send;
        ret = da7210_add_i2c_device(pdev, setup);
        if (ret != 0) {
            kfree(codec->private_data);
            kfree(codec);
        }
    }
#else
        /* Add SPI interface here */
#endif
    return ret;
}

static int da7210_remove(struct platform_device *pdev)
{

    struct snd_soc_device *socdev = platform_get_drvdata(pdev);
    struct snd_soc_codec *codec = socdev->codec;
    
    snd_soc_free_pcms(socdev);
    snd_soc_dapm_free(socdev);
#if defined(CONFIG_I2C) || defined(CONFIG_I2C_MODULE)
    i2c_del_driver(&da7210_i2c_driver);
#endif
    kfree(codec->private_data);
    kfree(codec);

    return 0;
}

struct snd_soc_codec_device soc_codec_dev_da7210 = {
    .probe =     da7210_probe,
    .remove =     da7210_remove,
};
EXPORT_SYMBOL_GPL(soc_codec_dev_da7210);

MODULE_DESCRIPTION("ASoC DA7210 driver");
MODULE_LICENSE("GPL");
