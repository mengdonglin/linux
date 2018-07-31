// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/slab.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/sof.h>
#include "sof-priv.h"

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
static const struct snd_soc_dapm_route sof_hdmi_map[] = {
	{ "hifi3", NULL, "iDisp3 Tx"},
	{ "iDisp3 Tx", NULL, "iDisp3_out"},
	{ "hifi2", NULL, "iDisp2 Tx"},
	{ "iDisp2 Tx", NULL, "iDisp2_out"},
	{ "hifi1", NULL, "iDisp1 Tx"},
	{ "iDisp1 Tx", NULL, "iDisp1_out"},

};
#endif

static struct snd_soc_card sof_nocodec_card = {
	.name = "sof-nocodec",

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	.dapm_routes = sof_hdmi_map,
	.num_dapm_routes = ARRAY_SIZE(sof_hdmi_map),
#endif
};

int sof_nocodec_setup(struct device *dev,
		      struct snd_sof_pdata *sof_pdata,
		      struct snd_soc_acpi_mach *mach,
		      const struct sof_dev_desc *desc,
		      struct snd_sof_dsp_ops *ops)
{
	struct snd_soc_dai_link *links;
	int dummy_link_num = 0, link_num;
	int ret;

	if (!mach)
		return -EINVAL;

	sof_pdata->drv_name = "sof-nocodec";

	mach->drv_name = "sof-nocodec";
	mach->sof_fw_filename = desc->nocodec_fw_filename;
	mach->sof_tplg_filename = desc->nocodec_tplg_filename;

#if !IS_ENABLED(CONFIG_SND_SOC_SOF_BYPASS_DSP)
	/* can only access SSPs via DSP */
	dummy_link_num = ops->dai_drv->num_drv;
#endif
	link_num = dummy_link_num;

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	link_num += SOF_HDMI_PINS;
#endif

	if (!link_num) {
		dev_err(dev, "No backends found for SOF\n");
		return -EINVAL;
	}

	links = devm_kzalloc(dev, sizeof(struct snd_soc_dai_link) *
			     link_num, GFP_KERNEL);
	if (!links)
		return -ENOMEM;


#if !IS_ENABLED(CONFIG_SND_SOC_SOF_BYPASS_DSP)
	/* create dummy BE dai_links for SSPs */
	ret = sof_bes_setup(dev, ops, links, dummy_link_num);
	if (ret) {
		dev_err(dev, "Fail to setup SOF nocodec backends %d\n", ret);
		goto err;
	}
#endif

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	/* set up HDMI backend dai links */
	ret = sof_hdmi_bes_setup(dev,
				 links,
				 dummy_link_num,
				 SOF_HDMI_PINS,
				 2);
	if (ret) {
		dev_err(dev, "Can't setup SOF nocodec HDMI backends %d\n", ret);
		goto err;
	}
#endif

	sof_nocodec_card.dai_link = links;
	sof_nocodec_card.num_links = link_num;

	return 0;

err:
	kfree(links);
	return ret;
}
EXPORT_SYMBOL(sof_nocodec_setup);

static int sof_nocodec_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &sof_nocodec_card;

	card->dev = &pdev->dev;

	return devm_snd_soc_register_card(&pdev->dev, card);
}

static int sof_nocodec_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver sof_nocodec_audio = {
	.probe = sof_nocodec_probe,
	.remove = sof_nocodec_remove,
	.driver = {
		.name = "sof-nocodec",
		.pm = &snd_soc_pm_ops,
	},
};
module_platform_driver(sof_nocodec_audio)

MODULE_DESCRIPTION("ASoC sof nocodec");
MODULE_AUTHOR("Liam Girdwood");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("platform:sof-nocodec");
