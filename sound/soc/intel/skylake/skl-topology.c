/*
 *  skl-topology.c - Implements Platform component ALSA controls/widget
 *  handlers.
 *
 *  Copyright (C) 2014-2015 Intel Corp
 *  Author: Jeeja KP <jeeja.kp@intel.com>
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/slab.h>
#include <linux/types.h>
#include <linux/firmware.h>
#include <sound/soc.h>
#include <sound/soc-topology.h>
#include "skl-sst-dsp.h"
#include "skl-sst-ipc.h"
#include "skl-topology.h"
#include "skl.h"
#include "skl-tplg-interface.h"

/*
 * SKL DSP driver modelling uses only few DAPM widgets so for rest we will
 * ignore. This helpers checks if the SKL driver handles this widget type
 */
static int is_skl_dsp_widget_type(struct snd_soc_dapm_widget *w)
{
	switch (w->id) {
	case snd_soc_dapm_dai_link:
	case snd_soc_dapm_dai_in:
	case snd_soc_dapm_aif_in:
	case snd_soc_dapm_aif_out:
	case snd_soc_dapm_dai_out:
	case snd_soc_dapm_switch:
		return false;
	default:
		return true;
	}
}

static int skl_tplg_bind_unbind_pipes(struct skl_module_cfg *src_module,
	struct skl_module_cfg *sink_module, struct skl_sst *ctx, bool bind)
{
	int ret;

	if (!bind) {
		ret = skl_stop_pipe(ctx, src_module->pipe);
		if (ret < 0)
			return ret;

		ret = skl_unbind_modules(ctx, src_module, sink_module);
	} else {
		ret = skl_bind_modules(ctx, src_module, sink_module);
	}

	return ret;
}

/*
 * Each pipelines needs memory to be allocated. Check if we have free memory
 * from available pool. Then only add this to pool
 * This is freed when pipe is deleted
 * Note: DSP does actual memory management we only keep track for complete
 * pool
 */
static bool skl_tplg_is_pipe_mem_available(struct skl *skl,
				struct skl_module_cfg *mconfig)
{
	struct skl_sst *ctx = skl->skl_sst;

	dev_dbg(ctx->dev, "%s: module_id =%d instance=%d\n", __func__,
		 mconfig->id.module_id, mconfig->id.instance_id);

	if (skl->resource.mem + mconfig->pipe->memory_pages > skl->resource.max_mem) {
		dev_err(ctx->dev, "exceeds ppl memory available=%d > mem=%d\n",
				skl->resource.max_mem, skl->resource.mem);
		return false;
	}

	skl->resource.mem += mconfig->pipe->memory_pages;
	return true;
}

/*
 * Pipeline needs needs DSP CPU resources for computation, this is quantified
 * in MCPS (Million Clocks Per Second) required for module/pipe
 *
 * Each pipelines needs mcps to be allocated. Check if we have mcps for this
 * pipe. This adds the mcps to driver counter
 * This is removed on pipeline delete
 */
static bool skl_tplg_is_pipe_mcps_available(struct skl *skl,
				struct skl_module_cfg *mconfig)
{
	struct skl_sst *ctx = skl->skl_sst;

	dev_dbg(ctx->dev, "%s: module_id = %d instance=%d\n", __func__,
			mconfig->id.module_id, mconfig->id.instance_id);

	if (skl->resource.mcps + mconfig->mcps > skl->resource.max_mcps) {
		dev_err(ctx->dev, "exceeds ppl memory available=%d > mem=%d\n",
				skl->resource.max_mcps, skl->resource.mcps);
		return false;
	}

	skl->resource.mcps += mconfig->mcps;
	return true;
}

/*
 * A pipe can have multiple modules each of the will be a DAPM widget as
 * well. While managing a pipeline we need to get the list of all the
 * widgets in a pipelines, so this helper - skl_tplg_get_pipe_widget() helps
 * to get the SKL type widgets in that pipeline
 */
static int skl_tplg_get_pipe_widget(struct device *dev,
	struct snd_soc_dapm_widget *w, struct skl_pipe *pipe)
{
	struct skl_module_cfg *src_module = NULL;
	struct snd_soc_dapm_path *p = NULL;
	struct skl_pipe_module *p_module = NULL;

	p_module = devm_kzalloc(dev, sizeof(*p_module), GFP_KERNEL);
	if (!p_module)
		return -ENOMEM;

	p_module->w = w;
	list_add_tail(&p_module->node, &pipe->w_list);

	list_for_each_entry(p, &w->sinks, list_source) {
		if ((p->sink->priv == NULL)
			&& (!is_skl_dsp_widget_type(w)))
			continue;

		if ((p->sink->priv != NULL) && (p->connect)
			&& (is_skl_dsp_widget_type(p->sink))) {
			src_module = p->sink->priv;
			if (pipe->ppl_id == src_module->pipe->ppl_id) {
				dev_dbg(dev, "found widget=%s\n", p->sink->name);
				skl_tplg_get_pipe_widget(dev, p->sink, pipe);
			}
		}
	}
	return 0;
}

/*
 * Inside a pipe instance, we can have various modules. These modules need
 * to instantiated in DSP by invoking INIT_MODULE IPC, which is achieved by
 * skl_init_module() routine, so invoke that for all modules in a pipeline
 */
static int skl_tplg_init_pipe_modules(struct skl *skl, struct skl_pipe *pipe)
{
	struct skl_pipe_module *w_module;
	struct snd_soc_dapm_widget *w;
	struct skl_module_cfg *mconfig;
	struct skl_sst *ctx = skl->skl_sst;
	int ret = 0;

	dev_dbg(ctx->dev, "%s: pipe=%d\n", __func__, pipe->ppl_id);
	list_for_each_entry(w_module, &pipe->w_list, node) {
		w = w_module->w;
		dev_dbg(ctx->dev, "Pipe Module =%s\n", w->name);
		mconfig = w->priv;

		/* check resource available */
		if (!skl_tplg_is_pipe_mcps_available(skl, mconfig))
			return -ENOMEM;

		ret = skl_init_module(ctx, mconfig, NULL);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/*
 * Once all the modules in a pipe are instantiated, they need to be
 * connected.
 * On removal, before deleting a pipeline the modules need to disconnected.
 *
 * This is achieved by binding/unbinding these modules
 */
static int skl_tplg_bind_unbind_pipe_modules(struct skl_sst *ctx,
				struct skl_pipe *pipe, bool bind)
{
	struct skl_pipe_module *w_module;
	struct skl_module_cfg *src_module = NULL;
	struct skl_module_cfg *dst_module;
	int ret = 0;

	dev_dbg(ctx->dev, "%s: pipe=%d\n", __func__, pipe->ppl_id);
	list_for_each_entry(w_module, &pipe->w_list, node) {
		dst_module = w_module->w->priv;

		if (src_module == NULL) {
			src_module = dst_module;
			continue;
		}

		if (bind)
			ret = skl_bind_modules(ctx, src_module, dst_module);
		else
			ret = skl_unbind_modules(ctx, src_module, dst_module);
		if (ret < 0)
			return ret;

		src_module = dst_module;
	}
	return 0;
}
