// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2018 Intel Corporation. All rights reserved.
 *
 * Author: Keyon Jie <yang.jie@linux.intel.com>
 */

#include <linux/device.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/sof.h>
#include "sof-priv.h"

#define SOF_PLATFORM "sof-audio"

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
/* free the duplicated strings of HDMI links, but no the links.
 * warning: SSP links cannot use this function since some strings
 * are not duplicated.
 */
void sof_free_hdmi_links(struct snd_soc_dai_link *links, int link_num)
{
	int i;

	if (!links || !link_num)
		return;

	for (i = 0; i < link_num; i++) {
		kfree(links[i].name);
		kfree(links[i].cpu_dai_name);
		kfree(links[i].codec_name);
		kfree(links[i].codec_dai_name);
		kfree(links[i].platform_name);
	}
}

/* set up HDMI backend dai links.
 *
 * template:
 *		.name = "iDisp1",
 *		.id = 3,
 *		.cpu_dai_name = "iDisp1 Pin",
 *		.codec_name = "ehdaudio0D2",
 *		.codec_dai_name = "intel-hdmi-hifi1",
 *		.platform_name = "sof-audio",
 *		//.init = broxton_hdmi_init,
 *		.dpcm_playback = 1,
 *		.no_pcm = 1,
 */
int sof_hdmi_bes_setup(struct device *dev,
		  struct snd_soc_dai_link *links,
		  int offset,
		  int link_num,
		  int codec_device)
{
	char name[32];
	int i, ret = 0;

	if (!links || !link_num)
		return -EINVAL;

	links += offset;
	for (i = 0; i < link_num; i++) {

		links[i].id = i + offset;

		snprintf(name, 32, "iDisp%d", i + 1);
		links[i].name = devm_kstrdup(dev, name, GFP_KERNEL);
		if (!links[i].name) {
			ret = -ENOMEM;
			goto err;
		}

		snprintf(name, 32, "iDisp%d Pin", i + 1);
		links[i].cpu_dai_name = devm_kstrdup(dev, name, GFP_KERNEL);
		if (!links[i].cpu_dai_name) {
			ret = -ENOMEM;
			goto err;
		}

		snprintf(name, 32, "ehdaudio0D%d", codec_device);
		links[i].codec_name = devm_kstrdup(dev, name, GFP_KERNEL);
		if (!links[i].codec_name) {
			ret = -ENOMEM;
			goto err;
		}

		snprintf(name, 32, "intel-hdmi-hifi%d", i + 1);
		links[i].codec_dai_name = devm_kstrdup(dev, name, GFP_KERNEL);
		if (!links[i].codec_dai_name) {
			ret = -ENOMEM;
			goto err;
		}

		links[i].platform_name = devm_kstrdup(dev,
						      SOF_PLATFORM,
						      GFP_KERNEL);
		if (!links[i].platform_name) {
			ret = -ENOMEM;
			goto err;
		}

		links[i].dpcm_playback = 1;
		links[i].no_pcm = 1;
	}

	return 0;
err:
	sof_free_hdmi_links(links, link_num);
	return ret;
}
EXPORT_SYMBOL(sof_hdmi_bes_setup);
#endif

int sof_bes_setup(struct device *dev, struct snd_sof_dsp_ops *ops,
		  struct snd_soc_dai_link *links, int link_num)
{
	char name[32];
	int i;

	if (!ops || !links)
		return -EINVAL;

	/* set up BE dai_links */
	for (i = 0; i < link_num; i++) {
		snprintf(name, 32, "NoCodec-%d", i);
		links[i].name = devm_kstrdup(dev, name, GFP_KERNEL);
		if (!links[i].name)
			return -ENOMEM;

		links[i].id = i;
		links[i].no_pcm = 1;
		links[i].cpu_dai_name = ops->dai_drv->drv[i].name;
		links[i].platform_name = "sof-audio";
		links[i].codec_dai_name = "snd-soc-dummy-dai";
		links[i].codec_name = "snd-soc-dummy";
		links[i].dpcm_playback = 1;
		links[i].dpcm_capture = 1;
	}

	return 0;
}
EXPORT_SYMBOL(sof_bes_setup);

/* register sof audio device */
int sof_create_audio_device(struct sof_platform_priv *priv)
{
	struct snd_sof_pdata *sof_pdata = priv->sof_pdata;
	struct device *dev = sof_pdata->dev;

	priv->pdev_pcm =
		platform_device_register_data(dev, "sof-audio", -1,
					      sof_pdata, sizeof(*sof_pdata));
	if (IS_ERR(priv->pdev_pcm)) {
		dev_err(dev, "Cannot register device sof-audio. Error %d\n",
			(int)PTR_ERR(priv->pdev_pcm));
		return PTR_ERR(priv->pdev_pcm);
	}

	return 0;
}
EXPORT_SYMBOL(sof_create_audio_device);
