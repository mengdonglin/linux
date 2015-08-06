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

#define SKL_CH_FIXUP_MASK		(1 << 0)
#define SKL_RATE_FIXUP_MASK		(1 << 1)
#define SKL_FMT_FIXUP_MASK		(1 << 2)

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

static void skl_dump_mconfig(struct skl_sst *ctx,
					struct skl_module_cfg *mcfg)
{
	dev_dbg(ctx->dev, "Dumping config\n");
	dev_dbg(ctx->dev, "Input Format:\n");
	dev_dbg(ctx->dev, "channels = %d\n", mcfg->in_fmt.channels);
	dev_dbg(ctx->dev, "s_freq = %d\n", mcfg->in_fmt.s_freq);
	dev_dbg(ctx->dev, "ch_cfg = %d\n", mcfg->in_fmt.ch_cfg);
	dev_dbg(ctx->dev, "valid bit depth = %d\n", mcfg->in_fmt.valid_bit_depth);
	dev_dbg(ctx->dev, "Output Format:\n");
	dev_dbg(ctx->dev, "channels = %d\n", mcfg->out_fmt.channels);
	dev_dbg(ctx->dev, "s_freq = %d\n", mcfg->out_fmt.s_freq);
	dev_dbg(ctx->dev, "valid bit depth = %d\n", mcfg->out_fmt.valid_bit_depth);
	dev_dbg(ctx->dev, "ch_cfg = %d\n", mcfg->out_fmt.ch_cfg);
}

static void skl_tplg_update_params(struct skl_module_fmt *fmt,
			struct skl_pipe_params *params, int fixup)
{
	if (fixup & SKL_RATE_FIXUP_MASK)
		fmt->s_freq = params->s_freq;
	if (fixup & SKL_CH_FIXUP_MASK)
		fmt->channels = params->ch;
	if (fixup & SKL_FMT_FIXUP_MASK)
		fmt->valid_bit_depth = params->s_fmt;
}

/*
 * A pipeline may have modules which impact the pcm parameters, like SRC,
 * channel converter, format converter.
 * We need to calculate the output params by applying the 'fixup'
 * Topology will tell driver which type of fixup is to be applied by
 * supplying the fixup mask, so based on that we calculate the output
 *
 * Now In FE the pcm hw_params is source/target format. Same is applicable
 * for BE with its hw_params invoked.
 * here based on FE, BE pipeline and direction we calculate the input and
 * outfix and then apply that for a module
 */
static void skl_tplg_update_params_fixup(struct skl_module_cfg *m_cfg,
		struct skl_pipe_params *params, bool is_fe)
{
	int in_fixup, out_fixup;
	struct skl_module_fmt *in_fmt, *out_fmt;

	in_fmt = &m_cfg->in_fmt;
	out_fmt = &m_cfg->out_fmt;

	if (params->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (is_fe) {
			in_fixup = m_cfg->params_fixup;
			out_fixup = (~m_cfg->converter) & m_cfg->params_fixup;
		} else {
			out_fixup = m_cfg->params_fixup;
			in_fixup = (~m_cfg->converter) & m_cfg->params_fixup;
		}
	} else {
		if (is_fe) {
			out_fixup = m_cfg->params_fixup;
			in_fixup = (~m_cfg->converter) & m_cfg->params_fixup;
		} else {
			in_fixup = m_cfg->params_fixup;
			out_fixup = (~m_cfg->converter) & m_cfg->params_fixup;
		}
	}

	skl_tplg_update_params(in_fmt, params, in_fixup);
	skl_tplg_update_params(out_fmt, params, out_fixup);
}

/*
 * A module needs input and output buffers, which are dependent upon pcm
 * params, so once we have calculate params, we need buffer calculation as
 * well.
 */
static void skl_tplg_update_buffer_size(struct skl_sst *ctx,
				struct skl_module_cfg *mcfg)
{
	int multiplier = 1;

	if (mcfg->m_type == SKL_MODULE_TYPE_SRCINT)
		multiplier = 5;

	mcfg->ibs = (mcfg->in_fmt.s_freq / 1000) *
				(mcfg->in_fmt.channels) *
				(mcfg->in_fmt.bit_depth >> 3) *
				multiplier;

	mcfg->obs = (mcfg->out_fmt.s_freq / 1000) *
				(mcfg->out_fmt.channels) *
				(mcfg->out_fmt.bit_depth >> 3) *
				multiplier;
}

static void skl_tplg_update_module_params(struct snd_soc_dapm_widget *w,
							struct skl_sst *ctx)
{
	struct skl_module_cfg *m_cfg = w->priv;
	struct skl_pipe_params *params = m_cfg->pipe->p_params;
	int p_conn_type = m_cfg->pipe->conn_type;
	bool is_fe;

	if (!m_cfg->params_fixup)
		return;

	dev_dbg(ctx->dev, "Mconfig for widget=%s BEFORE updation\n", w->name);
	skl_dump_mconfig(ctx, m_cfg);

	if (p_conn_type == SKL_PIPE_CONN_TYPE_FE)
		is_fe = true;
	else
		is_fe = false;

	skl_tplg_update_params_fixup(m_cfg, params, is_fe);
	skl_tplg_update_buffer_size(ctx, m_cfg);

	dev_dbg(ctx->dev, "Mconfig for widget=%s AFTER updation\n", w->name);
	skl_dump_mconfig(ctx, m_cfg);
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

		/* apply fix/conversion to module params based on FE/BE params*/
		skl_tplg_update_module_params(w, ctx);
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
