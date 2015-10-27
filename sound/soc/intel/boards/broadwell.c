/*
 * Intel Broadwell Wildcatpoint SST Audio
 *
 * Copyright (C) 2013, Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>

#include "../common/sst-dsp.h"
#include "../haswell/sst-haswell-ipc.h"

#include "../../codecs/rt286.h"

static struct snd_soc_jack broadwell_headset;
/* Headset jack detection DAPM pins */
static struct snd_soc_jack_pin broadwell_headset_pins[] = {
	{
		.pin = "Mic Jack",
		.mask = SND_JACK_MICROPHONE,
	},
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};

static const struct snd_kcontrol_new broadwell_controls[] = {
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
};

static const struct snd_soc_dapm_widget broadwell_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
	SND_SOC_DAPM_MIC("DMIC1", NULL),
	SND_SOC_DAPM_MIC("DMIC2", NULL),
	SND_SOC_DAPM_LINE("Line Jack", NULL),
};

static const struct snd_soc_dapm_route broadwell_rt286_map[] = {

	/* speaker */
	{"Speaker", NULL, "SPOR"},
	{"Speaker", NULL, "SPOL"},

	/* HP jack connectors - unknown if we have jack deteck */
	{"Headphone Jack", NULL, "HPO Pin"},

	/* other jacks */
	{"MIC1", NULL, "Mic Jack"},
	{"LINE1", NULL, "Line Jack"},

	/* digital mics */
	{"DMIC1 Pin", NULL, "DMIC1"},
	{"DMIC2 Pin", NULL, "DMIC2"},

	/* CODEC BE connections */
	{"SSP0 CODEC IN", NULL, "AIF1 Capture"},
	{"AIF1 Playback", NULL, "SSP0 CODEC OUT"},
};

static int broadwell_rt286_codec_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	int ret = 0;
	ret = snd_soc_card_jack_new(rtd->card, "Headset",
		SND_JACK_HEADSET | SND_JACK_BTN_0, &broadwell_headset,
		broadwell_headset_pins, ARRAY_SIZE(broadwell_headset_pins));
	if (ret)
		return ret;

	rt286_mic_detect(codec, &broadwell_headset);
	return 0;
}


static int broadwell_ssp0_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_CHANNELS);

	/* The ADSP will covert the FE rate to 48k, stereo */
	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;

	/* set SSP0 to 16 bit */
	params_set_format(params, SNDRV_PCM_FORMAT_S16_LE);
	return 0;
}

static int broadwell_rt286_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;

	ret = snd_soc_dai_set_sysclk(codec_dai, RT286_SCLK_S_PLL, 24000000,
		SND_SOC_CLOCK_IN);

	if (ret < 0) {
		dev_err(rtd->dev, "can't set codec sysclk configuration\n");
		return ret;
	}

	return ret;
}

static struct snd_soc_ops broadwell_rt286_ops = {
	.hw_params = broadwell_rt286_hw_params,
};

static int broadwell_rtd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sst_pdata *pdata = dev_get_platdata(rtd->platform->dev);
	struct sst_hsw *broadwell = pdata->dsp;
	int ret;

	/* Set ADSP SSP port settings */
	ret = sst_hsw_device_set_config(broadwell, SST_HSW_DEVICE_SSP_0,
		SST_HSW_DEVICE_MCLK_FREQ_24_MHZ,
		SST_HSW_DEVICE_CLOCK_MASTER, 9);
	if (ret < 0) {
		dev_err(rtd->dev, "error: failed to set device config\n");
		return ret;
	}

	return 0;
}

/* Front End DAI links */
static struct snd_soc_dai_link broadwell_system_pcm_link = {
	.name = "System PCM",
	.stream_name = "System Playback/Capture",
	.cpu_dai_name = "System Pin",
	.platform_name = "haswell-pcm-audio",
	.dynamic = 1,
	.codec_name = "snd-soc-dummy",
	.codec_dai_name = "snd-soc-dummy-dai",
	.init = broadwell_rtd_init,
	.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
	.dpcm_playback = 1,
	.dpcm_capture = 1,
};

static struct snd_soc_dai_link broadwell_offload0_link = {
	.name = "Offload0",
	.stream_name = "Offload0 Playback",
	.cpu_dai_name = "Offload0 Pin",
	.platform_name = "haswell-pcm-audio",
	.dynamic = 1,
	.codec_name = "snd-soc-dummy",
	.codec_dai_name = "snd-soc-dummy-dai",
	.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
	.dpcm_playback = 1,
};

static struct snd_soc_dai_link broadwell_offload1_link = {
	.name = "Offload1",
	.stream_name = "Offload1 Playback",
	.cpu_dai_name = "Offload1 Pin",
	.platform_name = "haswell-pcm-audio",
	.dynamic = 1,
	.codec_name = "snd-soc-dummy",
	.codec_dai_name = "snd-soc-dummy-dai",
	.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
	.dpcm_playback = 1,
};

static struct snd_soc_dai_link broadwell_loopback_pcm_link = {
	.name = "Loopback PCM",
	.stream_name = "Loopback",
	.cpu_dai_name = "Loopback Pin",
	.platform_name = "haswell-pcm-audio",
	.dynamic = 0,
	.codec_name = "snd-soc-dummy",
	.codec_dai_name = "snd-soc-dummy-dai",
	.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
	.dpcm_capture = 1,
};

/* broadwell digital audio interface glue - connects codec <--> CPU */
static struct snd_soc_dai_link broadwell_rt286_dais[] = {
	/* Back End DAI links */
	{
		/* SSP0 - Codec */
		.name = "Codec",
		.be_id = 0,
		.cpu_dai_name = "snd-soc-dummy-dai",
		.platform_name = "snd-soc-dummy",
		.no_pcm = 1,
		.codec_name = "i2c-INT343A:00",
		.codec_dai_name = "rt286-aif1",
		.init = broadwell_rt286_codec_init,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.be_hw_params_fixup = broadwell_ssp0_fixup,
		.ops = &broadwell_rt286_ops,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
	},
};

static int broadwell_suspend(struct snd_soc_card *card){
	struct snd_soc_codec *codec;

	list_for_each_entry(codec, &card->codec_dev_list, card_list) {
		if (!strcmp(codec->component.name, "i2c-INT343A:00")) {
			dev_dbg(codec->dev, "disabling jack detect before going to suspend.\n");
			rt286_mic_detect(codec, NULL);
			break;
		}
	}
	return 0;
}

static int broadwell_resume(struct snd_soc_card *card){
	struct snd_soc_codec *codec;

	list_for_each_entry(codec, &card->codec_dev_list, card_list) {
		if (!strcmp(codec->component.name, "i2c-INT343A:00")) {
			dev_dbg(codec->dev, "enabling jack detect for resume.\n");
			rt286_mic_detect(codec, &broadwell_headset);
			break;
		}
	}
	return 0;
}

static int bdw_add_dai_link(struct snd_soc_card *card, struct snd_soc_dai_link *link)
{
	struct snd_soc_dai_link *src_link;
	printk("bdw_add_dai_link: dai link name %s, stream name %s\n", link->name, link->stream_name);

	if (!link->stream_name) {
		WARN_ON(1);
		return 0;
	}

	if (!strcmp(link->stream_name, broadwell_system_pcm_link.stream_name)) {
		printk("bdw_add_dai_link: add the system pcm link\n");
		src_link = &broadwell_system_pcm_link;
		link->init = broadwell_rtd_init;
	} else if (!strcmp(link->stream_name, broadwell_offload0_link.stream_name)) {
		printk("bdw_add_dai_link: add the offload0 link\n");
		src_link = &broadwell_offload0_link;
	} else if (!strcmp(link->stream_name, broadwell_offload1_link.stream_name)) {
		printk("bdw_add_dai_link: add the offload1 link\n");
		src_link = &broadwell_offload1_link;
	} else if (!strcmp(link->stream_name, broadwell_loopback_pcm_link.stream_name)) {
		printk("bdw_add_dai_link: add the loopback link\n");
		src_link = &broadwell_loopback_pcm_link;
	} else {
		printk("bdw_add_dai_link: dai link name %s, Invalid stream name %s\n", link->name, link->stream_name);
		return -EINVAL;
	}

	link->name = src_link->name;
	link->cpu_dai_name = src_link->cpu_dai_name;
	link->platform_name = src_link->platform_name;
	link->dynamic = src_link->dynamic;
	link->codec_name = src_link->codec_name;
	link->codec_dai_name = src_link->codec_dai_name;
	link->trigger[0] = src_link->trigger[0];
	link->trigger[1] = src_link->trigger[1];
	link->dpcm_playback = src_link->dpcm_playback;
	link->dpcm_capture = src_link->dpcm_playback;

	printk("\t dai link name %s, stream name %s\n", link->name, link->stream_name);
	printk("\t cpu_dai_name %s, platform_name %s\n", link->cpu_dai_name, link->platform_name);
	printk("\t codec_name %s, codec_dai_name %s\n", link->codec_name, link->codec_dai_name);
	printk("\t dynamic %d, dpcm_playback %d, dpcm_capture%d\n", link->dynamic, link->dpcm_playback,  link->dpcm_capture);
	return 0;
}


static struct snd_soc_aux_dev bdw_tplg_dev = {
	.name = "haswell-pcm-audio",
};

/* broadwell audio machine driver for WPT + RT286S */
static struct snd_soc_card broadwell_rt286 = {
	.name = "broadwell-rt286",
	.owner = THIS_MODULE,
	.dai_link = broadwell_rt286_dais,
	.num_links = ARRAY_SIZE(broadwell_rt286_dais),
	.aux_dev = &bdw_tplg_dev,
	.num_aux_devs = 1,
	.controls = broadwell_controls,
	.num_controls = ARRAY_SIZE(broadwell_controls),
	.dapm_widgets = broadwell_widgets,
	.num_dapm_widgets = ARRAY_SIZE(broadwell_widgets),
	.dapm_routes = broadwell_rt286_map,
	.num_dapm_routes = ARRAY_SIZE(broadwell_rt286_map),
	.fully_routed = true,
	.suspend_pre = broadwell_suspend,
	.resume_post = broadwell_resume,
	.add_dai_link = bdw_add_dai_link,
};

static int broadwell_audio_probe(struct platform_device *pdev)
{
	broadwell_rt286.dev = &pdev->dev;

	return devm_snd_soc_register_card(&pdev->dev, &broadwell_rt286);
}

static struct platform_driver broadwell_audio = {
	.probe = broadwell_audio_probe,
	.driver = {
		.name = "broadwell-audio",
	},
};

module_platform_driver(broadwell_audio)

/* Module information */
MODULE_AUTHOR("Liam Girdwood, Xingchao Wang");
MODULE_DESCRIPTION("Intel SST Audio for WPT/Broadwell");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:broadwell-audio");
