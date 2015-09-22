/*
 * Intel Skylake I2S Machine Driver
 *
 * Copyright (C) 2014-2015, Intel Corporation. All rights reserved.
 *
 * Modified from:
 *   Intel Broadwell Wildcatpoint SST Audio
 *
 *   Copyright (C) 2013, Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <sound/soc-topology.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>

#include "../../codecs/rt286.h"

static struct snd_soc_jack skylake_headset;
/* Headset jack detection DAPM pins */
static struct snd_soc_jack_pin skylake_headset_pins[] = {
	{
		.pin = "Mic Jack",
		.mask = SND_JACK_MICROPHONE,
	},
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
};

static const struct snd_kcontrol_new skylake_controls[] = {
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Mic Jack"),
};

static const struct snd_soc_dapm_widget skylake_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
	SND_SOC_DAPM_MIC("DMIC2", NULL),
	SND_SOC_DAPM_MIC("SoC DMIC", NULL),
};

static const struct snd_soc_dapm_route skylake_rt286_map[] = {
	/* speaker */
	{"Speaker", NULL, "SPOR"},
	{"Speaker", NULL, "SPOL"},

	/* HP jack connectors - unknown if we have jack deteck */
	{"Headphone Jack", NULL, "HPO Pin"},

	/* other jacks */
	{"MIC1", NULL, "Mic Jack"},

	/* digital mics */
	{"DMIC1 Pin", NULL, "DMIC2"},
	{"DMIC AIF", NULL, "SoC DMIC"},

	/* CODEC BE connections */
	{ "AIF1 Playback", NULL, "ssp0 Tx"},
	{ "ssp0 Tx", NULL, "codec0_out"},
	{ "ssp0 Tx", NULL, "codec1_out"},

	{ "codec0_in", NULL, "ssp0 Rx" },
	{ "codec1_in", NULL, "ssp0 Rx" },
	{ "ssp0 Rx", NULL, "AIF1 Capture" },

	{ "dmic01_hifi", NULL, "DMIC01 Rx" },
	{ "DMIC01 Rx", NULL, "Capture" },

	{ "hif1", NULL, "iDisp Tx"},
	{ "iDisp Tx", NULL, "iDisp_out"},

};

static int skylake_rt286_codec_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	int ret = 0;

	ret = snd_soc_card_jack_new(rtd->card, "Headset",
		SND_JACK_HEADSET | SND_JACK_BTN_0,
		&skylake_headset,
		skylake_headset_pins, ARRAY_SIZE(skylake_headset_pins));

	if (ret)
		return ret;

	rt286_mic_detect(codec, &skylake_headset);
	return 0;
}


static int skylake_ssp0_fixup(struct snd_soc_pcm_runtime *rtd,
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
	snd_mask_set(&params->masks[SNDRV_PCM_HW_PARAM_FORMAT -
				    SNDRV_PCM_HW_PARAM_FIRST_MASK],
				    SNDRV_PCM_FORMAT_S16_LE);
	return 0;
}

static int skylake_rt286_hw_params(struct snd_pcm_substream *substream,
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

static struct snd_soc_ops skylake_rt286_ops = {
	.hw_params = skylake_rt286_hw_params,
};

/* skylake digital audio interface glue - connects codec <--> CPU */
static struct snd_soc_dai_link skylake_rt286_dais[] = {
	/* Front End DAI links */
	{
		.name = "Skl Audio Port",
		.stream_name = "Audio",
		.cpu_dai_name = "System Pin",
		.platform_name = "0000:00:1f.3",
		.nonatomic = 1,
		.dynamic = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.dpcm_playback = 1,
	},
	{
		.name = "Skl Audio Capture Port",
		.stream_name = "Audio Record",
		.cpu_dai_name = "System Pin",
		.platform_name = "0000:00:1f.3",
		.nonatomic = 1,
		.dynamic = 1,
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST, SND_SOC_DPCM_TRIGGER_POST},
		.dpcm_capture = 1,
	},
	{
		.name = "Skl Audio Reference cap",
		.stream_name = "refcap",
		.cpu_dai_name = "Reference Pin",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.platform_name = "0000:00:1f.3",
		.init = NULL,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		.nonatomic = 1,
		.dynamic = 1,
	},
	/*{
		.name = "Skl HDMI Port",
		.stream_name = "Hdmi",
		.cpu_dai_name = "System Pin",
		.codec_name = "snd-soc-dummy",
		.codec_dai_name = "snd-soc-dummy-dai",
		.platform_name = "0000:00:1f.3",
		.dpcm_playback = 1,
		.init = NULL,
		.ignore_suspend = 1,
		.nonatomic = 1,
		.dynamic = 1,
	},*/

	/* Back End DAI links */
	{
		/* SSP0 - Codec */
		.name = "SSP0-Codec",
		.be_id = 0,
		.cpu_dai_name = "SSP0 Pin",
		.platform_name = "0000:00:1f.3",
		.no_pcm = 1,
		.codec_name = "i2c-INT343A:00",
		.codec_dai_name = "rt286-aif1",
		.init = skylake_rt286_codec_init,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.be_hw_params_fixup = skylake_ssp0_fixup,
		.ops = &skylake_rt286_ops,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
	},
	{
		.name = "dmic01",
		.be_id = 1,
		.cpu_dai_name = "DMIC01 Pin",
		.codec_name = "dmic-codec",
		.codec_dai_name = "dmic-hifi",
		.platform_name = "0000:00:1f.3",
		.ignore_suspend = 1,
		.dpcm_capture = 1,
		.no_pcm = 1,
	},
	/*{
		.name = "iDisp",
		.be_id = 3,
		.cpu_dai_name = "iDisp Pin",
		.codec_name = "codec#002.2",
		.codec_dai_name = "intel-hdmi-hif1",
		.platform_name = "0000:00:1f.3",
		.dpcm_playback = 1,
		.ignore_suspend = 1,
		.no_pcm = 1,
	},*/
};

static struct snd_soc_tplg_ops skl_ops = {
};

static int skl_topology_probe(struct snd_soc_component *component)
{
	int ret = 0;
	const struct firmware *fw;

	ret = request_firmware(&fw, "skl.tplg", component->dev);
	if (ret < 0) {
		return ret;
	}

	/*
	 * The complete tplg for SKL is loaded as index 0, we don't use
	 * any other index
	 */
	ret = snd_soc_tplg_component_load(component, &skl_ops, fw, 1);
	if (ret < 0) {
		printk(KERN_ERR "error %d loading topology file.\n",ret);
		return ret;
	}
	return 0;
}

static void skl_topology_remove(struct snd_soc_component *component)
{
	snd_soc_tplg_component_remove(component, 1);	
}

static const struct snd_soc_component_driver skl_tplg_component = {
	.name = "skl-board-topology",
	.probe = skl_topology_probe,
	.remove = skl_topology_remove,
};

static struct snd_soc_aux_component skylake_topology_components[] = {

	/* Board Topology Component */
	{
		.name = "skl_alc286s_i2s",
	},
};

/* skylake audio machine driver for SPT + RT286S */
static struct snd_soc_card skylake_rt286 = {
	.name = "skylake-rt286",
	.owner = THIS_MODULE,
	.aux_components = skylake_topology_components,
	.num_aux_components = ARRAY_SIZE(skylake_topology_components),
	.dai_link = skylake_rt286_dais,
	.num_links = ARRAY_SIZE(skylake_rt286_dais),
	.controls = skylake_controls,
	.num_controls = ARRAY_SIZE(skylake_controls),
	.dapm_widgets = skylake_widgets,
	.num_dapm_widgets = ARRAY_SIZE(skylake_widgets),
	.dapm_routes = skylake_rt286_map,
	.num_dapm_routes = ARRAY_SIZE(skylake_rt286_map),
	.fully_routed = true,
};

static int skylake_audio_probe(struct platform_device *pdev)
{
	int ret;
	skylake_rt286.dev = &pdev->dev;

	ret = snd_soc_register_component(&pdev->dev, &skl_tplg_component, NULL, 0);
	if (ret) {
		 dev_err(&pdev->dev, "registering soc card failed\n");
		 snd_soc_unregister_component(&pdev->dev);
	}	

	ret = snd_soc_register_card(&skylake_rt286);
	if (ret) {
		dev_err(&pdev->dev, "registering soc card failed\n");
		snd_soc_unregister_component(&pdev->dev);
	}

	return ret;
}

static int skylake_audio_remove(struct platform_device *pdev)
{
	snd_soc_unregister_card(&skylake_rt286);
	snd_soc_unregister_component(&pdev->dev);
	return 0;
}

static struct platform_driver skylake_audio = {
	.probe = skylake_audio_probe,
	.remove = skylake_audio_remove,
	.driver = {
		.name = "skl_alc286s_i2s",
	},
};

module_platform_driver(skylake_audio)

/* Module information */
MODULE_AUTHOR("Omair Mohammed Abdullah <omair.m.abdullah@intel.com>");
MODULE_DESCRIPTION("Intel SST Audio for Skylake");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:skl_alc286s_i2s");
