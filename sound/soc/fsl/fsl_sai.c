// SPDX-License-Identifier: GPL-2.0+
//
// Freescale ALSA SoC Digital Audio Interface (SAI) driver.
//
// Copyright 2012-2016 Freescale Semiconductor, Inc.

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/pm_qos.h>
#include <sound/core.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm_params.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/syscon/imx6q-iomuxc-gpr.h>
#include <linux/pm_runtime.h>
#include <linux/busfreq-imx.h>

#include "fsl_dsd.h"
#include "fsl_sai.h"
#include "imx-pcm.h"

#define FSL_SAI_FLAGS (FSL_SAI_CSR_SEIE |\
		       FSL_SAI_CSR_FEIE)

#define FSL_SAI_VERID_0301	0x0301

static struct fsl_sai_soc_data fsl_sai_vf610 = {
	.imx = false,
	/*dataline is mask, not index*/
	.dataline = 0x1,
	.fifos = 1,
	.fifo_depth = 32,
	.flags = 0,
	.constrain_period_size = false,
};

static struct fsl_sai_soc_data fsl_sai_imx6sx = {
	.imx = true,
	.dataline = 0x1,
	.fifos = 1,
	.fifo_depth = 32,
	.flags = 0,
	.reg_offset = 0,
	.constrain_period_size = false,
};

static struct fsl_sai_soc_data fsl_sai_imx6ul = {
	.imx = true,
	.dataline = 0x1,
	.fifos = 1,
	.fifo_depth = 32,
	.flags = 0,
	.reg_offset = 0,
	.constrain_period_size = false,
};

static struct fsl_sai_soc_data fsl_sai_imx7ulp = {
	.imx = true,
	.dataline = 0x3,
	.fifos = 2,
	.fifo_depth = 16,
	.flags = SAI_FLAG_PMQOS,
	.reg_offset = 8,
	.constrain_period_size = false,
};

static struct fsl_sai_soc_data fsl_sai_imx8mq = {
	.imx = true,
	.dataline = 0xff,
	.fifos = 8,
	.fifo_depth = 128,
	.flags = 0,
	.reg_offset = 8,
	.constrain_period_size = false,
};

static struct fsl_sai_soc_data fsl_sai_imx8qm = {
	.imx = true,
	.dataline = 0xf,
	.fifos = 1,
	.fifo_depth = 64,
	.flags = 0,
	.reg_offset = 0,
	.constrain_period_size = true,
};

static const unsigned int fsl_sai_rates[] = {
	8000, 11025, 12000, 16000, 22050,
	24000, 32000, 44100, 48000, 64000,
	88200, 96000, 176400, 192000, 352800,
	384000, 705600, 768000, 1411200, 2822400,
};

static const struct snd_pcm_hw_constraint_list fsl_sai_rate_constraints = {
	.count = ARRAY_SIZE(fsl_sai_rates),
	.list = fsl_sai_rates,
};

static irqreturn_t fsl_sai_isr(int irq, void *devid)
{
	struct fsl_sai *sai = (struct fsl_sai *)devid;
	unsigned char offset = sai->soc->reg_offset;
	struct device *dev = &sai->pdev->dev;
	u32 flags, xcsr, mask;
	bool irq_none = true;

	/*
	 * Both IRQ status bits and IRQ mask bits are in the xCSR but
	 * different shifts. And we here create a mask only for those
	 * IRQs that we activated.
	 */
	mask = (FSL_SAI_FLAGS >> FSL_SAI_CSR_xIE_SHIFT) << FSL_SAI_CSR_xF_SHIFT;

	/* Tx IRQ */
	regmap_read(sai->regmap, FSL_SAI_TCSR(offset), &xcsr);
	flags = xcsr & mask;

	if (flags)
		irq_none = false;
	else
		goto irq_rx;

	if (flags & FSL_SAI_CSR_WSF)
		dev_dbg(dev, "isr: Start of Tx word detected\n");

	if (flags & FSL_SAI_CSR_SEF)
		dev_dbg(dev, "isr: Tx Frame sync error detected\n");

	if (flags & FSL_SAI_CSR_FEF) {
		dev_dbg(dev, "isr: Transmit underrun detected\n");
		/* FIFO reset for safety */
		xcsr |= FSL_SAI_CSR_FR;
	}

	if (flags & FSL_SAI_CSR_FWF)
		dev_dbg(dev, "isr: Enabled transmit FIFO is empty\n");

	if (flags & FSL_SAI_CSR_FRF)
		dev_dbg(dev, "isr: Transmit FIFO watermark has been reached\n");

	flags &= FSL_SAI_CSR_xF_W_MASK;
	xcsr &= ~FSL_SAI_CSR_xF_MASK;

	if (flags)
		regmap_write(sai->regmap, FSL_SAI_TCSR(offset), flags | xcsr);

irq_rx:
	/* Rx IRQ */
	regmap_read(sai->regmap, FSL_SAI_RCSR(offset), &xcsr);
	flags = xcsr & mask;

	if (flags)
		irq_none = false;
	else
		goto out;

	if (flags & FSL_SAI_CSR_WSF)
		dev_dbg(dev, "isr: Start of Rx word detected\n");

	if (flags & FSL_SAI_CSR_SEF)
		dev_dbg(dev, "isr: Rx Frame sync error detected\n");

	if (flags & FSL_SAI_CSR_FEF) {
		dev_dbg(dev, "isr: Receive overflow detected\n");
		/* FIFO reset for safety */
		xcsr |= FSL_SAI_CSR_FR;
	}

	if (flags & FSL_SAI_CSR_FWF)
		dev_dbg(dev, "isr: Enabled receive FIFO is full\n");

	if (flags & FSL_SAI_CSR_FRF)
		dev_dbg(dev, "isr: Receive FIFO watermark has been reached\n");

	flags &= FSL_SAI_CSR_xF_W_MASK;
	xcsr &= ~FSL_SAI_CSR_xF_MASK;

	if (flags)
		regmap_write(sai->regmap, FSL_SAI_RCSR(offset), flags | xcsr);

out:
	if (irq_none)
		return IRQ_NONE;
	else
		return IRQ_HANDLED;
}

static int fsl_sai_set_dai_tdm_slot(struct snd_soc_dai *cpu_dai, u32 tx_mask,
				u32 rx_mask, int slots, int slot_width)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(cpu_dai);

	sai->slots = slots;
	sai->slot_width = slot_width;

	return 0;
}

static int fsl_sai_set_dai_sysclk_tr(struct snd_soc_dai *cpu_dai,
		int clk_id, unsigned int freq, int fsl_dir)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(cpu_dai);
	unsigned char offset = sai->soc->reg_offset;
	bool tx = fsl_dir == FSL_FMT_TRANSMITTER;
	u32 val_cr2 = 0;

	switch (clk_id) {
	case FSL_SAI_CLK_BUS:
		val_cr2 |= FSL_SAI_CR2_MSEL_BUS;
		break;
	case FSL_SAI_CLK_MAST1:
		val_cr2 |= FSL_SAI_CR2_MSEL_MCLK1;
		break;
	case FSL_SAI_CLK_MAST2:
		val_cr2 |= FSL_SAI_CR2_MSEL_MCLK2;
		break;
	case FSL_SAI_CLK_MAST3:
		val_cr2 |= FSL_SAI_CR2_MSEL_MCLK3;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(sai->regmap, FSL_SAI_xCR2(tx, offset),
			   FSL_SAI_CR2_MSEL_MASK, val_cr2);

	return 0;
}

static int fsl_sai_set_mclk_rate(struct snd_soc_dai *dai, int clk_id,
		unsigned int freq)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(dai);
	struct clk *p = sai->mclk_clk[clk_id], *pll = 0, *npll = 0;
	u64 ratio = freq;
	int ret;

	while (p && sai->pll8k_clk && sai->pll11k_clk) {
		struct clk *pp = clk_get_parent(p);

		if (clk_is_match(pp, sai->pll8k_clk) ||
		    clk_is_match(pp, sai->pll11k_clk)) {
			pll = pp;
			break;
		}
		p = pp;
	}

	if (pll) {
		npll = (do_div(ratio, 8000) ? sai->pll11k_clk : sai->pll8k_clk);
		if (!clk_is_match(pll, npll)) {
			ret = clk_set_parent(p, npll);
			if (ret < 0)
				dev_warn(dai->dev,
					 "failed to set parent %s: %d\n",
					 __clk_get_name(npll), ret);
		}
	}

	ret = clk_set_rate(sai->mclk_clk[clk_id], freq);
	if (ret < 0)
		dev_err(dai->dev, "failed to set clock rate (%u): %d\n",
			freq, ret);

	return ret;
}

static int fsl_sai_set_dai_bclk_ratio(struct snd_soc_dai *dai, unsigned int ratio)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(dai);

	sai->bitclk_ratio = ratio;
	return 0;
}

static int fsl_sai_set_dai_sysclk(struct snd_soc_dai *cpu_dai,
		int clk_id, unsigned int freq, int dir)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(cpu_dai);
	int ret;

	if (dir == SND_SOC_CLOCK_IN)
		return 0;

	if (freq > 0 && clk_id != FSL_SAI_CLK_BUS) {
		if (clk_id < 0 || clk_id >= FSL_SAI_MCLK_MAX) {
			dev_err(cpu_dai->dev, "Unknown clock id: %d\n", clk_id);
			return -EINVAL;
		}

		if (IS_ERR_OR_NULL(sai->mclk_clk[clk_id])) {
			dev_err(cpu_dai->dev, "Unassigned clock: %d\n", clk_id);
			return -EINVAL;
		}

		if (sai->mclk_streams == 0) {
			ret = fsl_sai_set_mclk_rate(cpu_dai, clk_id, freq);
			if (ret < 0)
				return ret;
		}
	}

	ret = fsl_sai_set_dai_sysclk_tr(cpu_dai, clk_id, freq,
					FSL_FMT_TRANSMITTER);
	if (ret) {
		dev_err(cpu_dai->dev, "Cannot set tx sysclk: %d\n", ret);
		return ret;
	}

	ret = fsl_sai_set_dai_sysclk_tr(cpu_dai, clk_id, freq,
					FSL_FMT_RECEIVER);
	if (ret)
		dev_err(cpu_dai->dev, "Cannot set rx sysclk: %d\n", ret);

	return ret;
}

static int fsl_sai_set_dai_fmt_tr(struct snd_soc_dai *cpu_dai,
				unsigned int fmt, int fsl_dir)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(cpu_dai);
	unsigned char offset = sai->soc->reg_offset;
	bool tx = fsl_dir == FSL_FMT_TRANSMITTER;
	u32 val_cr2 = 0, val_cr4 = 0;

	if (!sai->is_lsb_first)
		val_cr4 |= FSL_SAI_CR4_MF;

	sai->is_dsp_mode = false;
	/* DAI mode */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		/*
		 * Frame low, 1clk before data, one word length for frame sync,
		 * frame sync starts one serial clock cycle earlier,
		 * that is, together with the last bit of the previous
		 * data word.
		 */
		val_cr2 |= FSL_SAI_CR2_BCP;
		val_cr4 |= FSL_SAI_CR4_FSE | FSL_SAI_CR4_FSP;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		/*
		 * Frame high, one word length for frame sync,
		 * frame sync asserts with the first bit of the frame.
		 */
		val_cr2 |= FSL_SAI_CR2_BCP;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		/*
		 * Frame high, 1clk before data, one bit for frame sync,
		 * frame sync starts one serial clock cycle earlier,
		 * that is, together with the last bit of the previous
		 * data word.
		 */
		val_cr2 |= FSL_SAI_CR2_BCP;
		val_cr4 |= FSL_SAI_CR4_FSE;
		sai->is_dsp_mode = true;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		/*
		 * Frame high, one bit for frame sync,
		 * frame sync asserts with the first bit of the frame.
		 */
		val_cr2 |= FSL_SAI_CR2_BCP;
		sai->is_dsp_mode = true;
		break;
	case SND_SOC_DAIFMT_PDM:
		val_cr2 |= FSL_SAI_CR2_BCP;
		val_cr4 &= ~FSL_SAI_CR4_MF;
		sai->is_dsp_mode = true;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		/* To be done */
	default:
		return -EINVAL;
	}

	/* DAI clock inversion */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_IB_IF:
		/* Invert both clocks */
		val_cr2 ^= FSL_SAI_CR2_BCP;
		val_cr4 ^= FSL_SAI_CR4_FSP;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		/* Invert bit clock */
		val_cr2 ^= FSL_SAI_CR2_BCP;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		/* Invert frame clock */
		val_cr4 ^= FSL_SAI_CR4_FSP;
		break;
	case SND_SOC_DAIFMT_NB_NF:
		/* Nothing to do for both normal cases */
		break;
	default:
		return -EINVAL;
	}

	sai->slave_mode[tx] = false;

	/* DAI clock master masks */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		val_cr2 |= FSL_SAI_CR2_BCD_MSTR;
		val_cr4 |= FSL_SAI_CR4_FSD_MSTR;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		sai->slave_mode[tx] = true;
		break;
	case SND_SOC_DAIFMT_CBS_CFM:
		val_cr2 |= FSL_SAI_CR2_BCD_MSTR;
		break;
	case SND_SOC_DAIFMT_CBM_CFS:
		val_cr4 |= FSL_SAI_CR4_FSD_MSTR;
		sai->slave_mode[tx] = true;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(sai->regmap, FSL_SAI_xCR2(tx, offset),
			   FSL_SAI_CR2_BCP | FSL_SAI_CR2_BCD_MSTR, val_cr2);
	regmap_update_bits(sai->regmap, FSL_SAI_xCR4(tx, offset),
			   FSL_SAI_CR4_MF | FSL_SAI_CR4_FSE |
			   FSL_SAI_CR4_FSP | FSL_SAI_CR4_FSD_MSTR, val_cr4);

	return 0;
}

static int fsl_sai_set_dai_fmt(struct snd_soc_dai *cpu_dai, unsigned int fmt)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(cpu_dai);
	int ret;

	if (sai->masterflag[FSL_FMT_TRANSMITTER])
		fmt = (fmt & (~SND_SOC_DAIFMT_MASTER_MASK)) |
				sai->masterflag[FSL_FMT_TRANSMITTER];

	ret = fsl_sai_set_dai_fmt_tr(cpu_dai, fmt, FSL_FMT_TRANSMITTER);
	if (ret) {
		dev_err(cpu_dai->dev, "Cannot set tx format: %d\n", ret);
		return ret;
	}

	if (sai->masterflag[FSL_FMT_RECEIVER])
		fmt = (fmt & (~SND_SOC_DAIFMT_MASTER_MASK)) |
				sai->masterflag[FSL_FMT_RECEIVER];

	ret = fsl_sai_set_dai_fmt_tr(cpu_dai, fmt, FSL_FMT_RECEIVER);
	if (ret)
		dev_err(cpu_dai->dev, "Cannot set rx format: %d\n", ret);

	return ret;
}

static int fsl_sai_check_ver(struct device *dev)
{
	struct fsl_sai *sai = dev_get_drvdata(dev);
	unsigned char offset = sai->soc->reg_offset;
	unsigned int val;
	int ret;

	if (FSL_SAI_TCSR(offset) == FSL_SAI_VERID)
		return 0;

	if (sai->verid.loaded)
		return 0;

	ret = regmap_read(sai->regmap, FSL_SAI_VERID, &val);
	if (ret < 0)
		return ret;

	dev_dbg(dev, "VERID: 0x%016X\n", val);

	sai->verid.id = (val & FSL_SAI_VER_ID_MASK) >> FSL_SAI_VER_ID_SHIFT;
	sai->verid.extfifo_en = (val & FSL_SAI_VER_EFIFO_EN);
	sai->verid.timestamp_en = (val & FSL_SAI_VER_TSTMP_EN);

	ret = regmap_read(sai->regmap, FSL_SAI_PARAM, &val);
	if (ret < 0)
		return ret;

	dev_dbg(dev, "PARAM: 0x%016X\n", val);

	/* max slots per frame, power of 2 */
	sai->param.spf = 1 <<
		((val & FSL_SAI_PAR_SPF_MASK) >> FSL_SAI_PAR_SPF_SHIFT);

	/* words per fifo, power of 2 */
	sai->param.wpf = 1 <<
		((val & FSL_SAI_PAR_WPF_MASK) >> FSL_SAI_PAR_WPF_SHIFT);

	/* number of datalines implemented */
	sai->param.dln = val & FSL_SAI_PAR_DLN_MASK;

	dev_dbg(dev,
		"Version: 0x%08X, SPF: %u, WPF: %u, DLN: %u\n",
		sai->verid.id, sai->param.spf, sai->param.wpf, sai->param.dln
	);

	sai->verid.loaded = true;

	return 0;
}

static int fsl_sai_set_bclk(struct snd_soc_dai *dai, bool tx, u32 freq)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(dai);
	struct clk *p;
	struct clk *parent_pll =
		(freq % 11025 == 0) ? sai->pll11k_clk : sai->pll8k_clk;
	unsigned char offset = sai->soc->reg_offset;
	unsigned long clk_rate;
	unsigned int reg = 0;
	u32 ratio, savesub = freq, saveratio = 0, savediv = 0;
	u32 id;
	int ret = 0;

	/* Don't apply to slave mode */
	if (sai->slave_mode[tx])
		return 0;

	for (id = 0; id < FSL_SAI_MCLK_MAX; id++) {
		clk_rate = clk_get_rate(sai->mclk_clk[id]);
		if (!clk_rate)
			continue;

		/* Find parent pll */
		p = sai->mclk_clk[id];
		while (p && sai->pll8k_clk && sai->pll11k_clk) {
			struct clk *pp = clk_get_parent(p);

			if (clk_is_match(pp, sai->pll8k_clk) ||
				clk_is_match(pp, sai->pll11k_clk)) {
				break;
			}
			p = pp;
		}

		/* set fitting parent pll */
		ret = clk_set_parent(p, parent_pll);
		if (ret != 0) {
			dev_err(dai->dev,
				"could not set parent of clock %s to %s\n",
				__clk_get_name(p), __clk_get_name(parent_pll));
			continue;
		}

		/*
		 * clk_rate/freq needs to be between 2 and 512 and
		 * has to be an even number (see below)
		 * so we set it to maximum ratio, then scale the frequency
		 * below 300Mhz, but above stay above lowest rati
		 */
		clk_rate = freq * 512;
		while (clk_rate >= 300*1000*1000 && clk_rate >= freq*2)
			clk_rate /= 2;

		ret = clk_set_rate(sai->mclk_clk[id], clk_rate);

		ratio = clk_rate / freq;

		ret = clk_rate - ratio * freq;

		/*
		 * Drop the source that can not be
		 * divided into the required rate.
		 */
		if (ret != 0 && clk_rate / ret < 1000)
			continue;

		dev_dbg(dai->dev,
			"ratio %d for freq %dHz based on clock %ldHz\n",
			ratio, freq, clk_rate);

		if ((ratio % 2 == 0 && ratio >= 2 && ratio <= 512) ||
		    (ratio == 1 && sai->verid.id >= FSL_SAI_VERID_0301)) {

			if (ret < savesub) {
				saveratio = ratio;
				sai->mclk_id[tx] = id;
				savesub = ret;
			}

			if (ret == 0)
				break;
		}
	}

	if (saveratio == 0) {
		dev_err(dai->dev, "failed to derive required %cx rate: %d\n",
				tx ? 'T' : 'R', freq);
		return -EINVAL;
	}

	/*
	 * 1) For Asynchronous mode, we must set RCR2 register for capture, and
	 *    set TCR2 register for playback.
	 * 2) For Tx sync with Rx clock, we must set RCR2 register for playback
	 *    and capture.
	 * 3) For Rx sync with Tx clock, we must set TCR2 register for playback
	 *    and capture.
	 * 4) For Tx and Rx are both Synchronous with another SAI, we just
	 *    ignore it.
	 */
	if ((!tx || sai->synchronous[TX]) && !sai->synchronous[RX])
		reg = FSL_SAI_RCR2(offset);
	else if ((tx || sai->synchronous[RX]) && !sai->synchronous[TX])
		reg = FSL_SAI_TCR2(offset);

	if (reg) {
		regmap_update_bits(sai->regmap, reg, FSL_SAI_CR2_MSEL_MASK,
			   FSL_SAI_CR2_MSEL(sai->mclk_id[tx]));

		savediv = (saveratio == 1 ? 0 : (saveratio >> 1) - 1);
		regmap_update_bits(sai->regmap, reg, FSL_SAI_CR2_DIV_MASK, savediv);

		if (sai->verid.id >= FSL_SAI_VERID_0301) {
			regmap_update_bits(sai->regmap, reg, FSL_SAI_CR2_BYP,
				   (saveratio == 1 ? FSL_SAI_CR2_BYP : 0));
		}
	}

	if (sai->verid.id >= FSL_SAI_VERID_0301) {
		/* SAI is in master mode at this point, so enable MCLK */
		regmap_update_bits(sai->regmap, FSL_SAI_MCTL,
			FSL_SAI_MCTL_MCLK_EN, FSL_SAI_MCTL_MCLK_EN);
	}

	dev_dbg(dai->dev, "best fit: clock id=%d, ratio=%d, deviation=%d\n",
			sai->mclk_id[tx], saveratio, savesub);

	return 0;
}

static int fsl_sai_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params,
		struct snd_soc_dai *cpu_dai)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(cpu_dai);
	unsigned char offset = sai->soc->reg_offset;
	bool tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	unsigned int channels = params_channels(params);
	u32 word_width = params_width(params);
	u32 rate = params_rate(params);
	u32 val_cr4 = 0, val_cr5 = 0;
	u32 slots = (channels == 1) ? 2 : channels;
	u32 slot_width = word_width;
	u32 pins, bclk;
	int ret, i, trce_mask = 0, dl_cfg_cnt, dl_cfg_idx = 0;
	struct fsl_sai_dl_cfg *dl_cfg;

	if (sai->slots)
		slots = sai->slots;

	pins = DIV_ROUND_UP(channels, slots);
	sai->is_dsd = fsl_is_dsd(params);
	if (sai->is_dsd) {
		pins = channels;
		dl_cfg = sai->dsd_dl_cfg;
		dl_cfg_cnt = sai->dsd_dl_cfg_cnt;
	} else {
		dl_cfg = sai->pcm_dl_cfg;
		dl_cfg_cnt = sai->pcm_dl_cfg_cnt;
	}

	for (i = 0; i < dl_cfg_cnt; i++) {
		if (dl_cfg[i].pins == pins) {
			dl_cfg_idx = i;
			break;
		}
	}

	if (dl_cfg_idx >= dl_cfg_cnt) {
		dev_err(cpu_dai->dev, "fsl,dataline%s invalid or not provided.\n",
			sai->is_dsd ? ",dsd" : "");
		return -EINVAL;
	}

	if (sai->slot_width)
		slot_width = sai->slot_width;

	bclk = rate*(sai->bitclk_ratio ? sai->bitclk_ratio : slots * slot_width);

	if (!IS_ERR_OR_NULL(sai->pinctrl)) {
		sai->pins_state = fsl_get_pins_state(sai->pinctrl, params, bclk);

		if (!IS_ERR_OR_NULL(sai->pins_state)) {
			ret = pinctrl_select_state(sai->pinctrl, sai->pins_state);
			if (ret) {
				dev_err(cpu_dai->dev,
					"failed to set proper pins state: %d\n", ret);
				return ret;
			}
		}
	}

	if (!sai->slave_mode[tx]) {
		ret = fsl_sai_set_bclk(cpu_dai, tx, bclk);
		if (ret)
			return ret;

		/* Do not enable the clock if it is already enabled */
		if (!(sai->mclk_streams & BIT(substream->stream))) {
			ret = clk_prepare_enable(sai->mclk_clk[sai->mclk_id[tx]]);
			if (ret)
				return ret;

			sai->mclk_streams |= BIT(substream->stream);
		}
	}

	if (!sai->is_dsp_mode)
		val_cr4 |= FSL_SAI_CR4_SYWD(slot_width);

	val_cr5 |= FSL_SAI_CR5_WNW(slot_width);
	val_cr5 |= FSL_SAI_CR5_W0W(slot_width);

	if (sai->is_lsb_first || sai->is_dsd)
		val_cr5 |= FSL_SAI_CR5_FBT(0);
	else
		val_cr5 |= FSL_SAI_CR5_FBT(word_width - 1);

	val_cr4 |= FSL_SAI_CR4_FRSZ(slots);

	/* Output Mode - data pins transmit 0 when slots are masked
	 * or channels are disabled
	 */
	val_cr4 |= FSL_SAI_CR4_CHMOD;

	/*
	 * For SAI master mode, when Tx(Rx) sync with Rx(Tx) clock, Rx(Tx) will
	 * generate bclk and frame clock for Tx(Rx), we should set RCR4(TCR4),
	 * RCR5(TCR5) and RMR(TMR) for playback(capture), or there will be sync
	 * error.
	 */

	if (!sai->slave_mode[tx]) {
		if (!sai->synchronous[TX] && sai->synchronous[RX] && !tx) {
			regmap_update_bits(sai->regmap, FSL_SAI_TCR4(offset),
				FSL_SAI_CR4_SYWD_MASK | FSL_SAI_CR4_FRSZ_MASK |
				FSL_SAI_CR4_CHMOD_MASK,
				val_cr4);
			regmap_update_bits(sai->regmap, FSL_SAI_TCR5(offset),
				FSL_SAI_CR5_WNW_MASK | FSL_SAI_CR5_W0W_MASK |
				FSL_SAI_CR5_FBT_MASK, val_cr5);
		} else if (!sai->synchronous[RX] && sai->synchronous[TX] && tx) {
			regmap_update_bits(sai->regmap, FSL_SAI_RCR4(offset),
				FSL_SAI_CR4_SYWD_MASK | FSL_SAI_CR4_FRSZ_MASK |
				FSL_SAI_CR4_CHMOD_MASK,
				val_cr4);
			regmap_update_bits(sai->regmap, FSL_SAI_RCR5(offset),
				FSL_SAI_CR5_WNW_MASK | FSL_SAI_CR5_W0W_MASK |
				FSL_SAI_CR5_FBT_MASK, val_cr5);
		}
	}

	if (sai->soc->dataline != 0x1) {

		if (dl_cfg[dl_cfg_idx].mask[tx] <= 1 || sai->is_multi_lane)
			regmap_update_bits(sai->regmap, FSL_SAI_xCR4(tx, offset),
				FSL_SAI_CR4_FCOMB_MASK, 0);
		else
			regmap_update_bits(sai->regmap, FSL_SAI_xCR4(tx, offset),
				FSL_SAI_CR4_FCOMB_MASK, FSL_SAI_CR4_FCOMB_SOFT);

		if (sai->is_multi_lane) {
			if (tx) {
				sai->dma_params_tx.maxburst =
						FSL_SAI_MAXBURST_TX * pins;
				sai->dma_params_tx.fifo_num = pins +
					   (dl_cfg[dl_cfg_idx].offset[tx] << 4);
			} else {
				sai->dma_params_rx.maxburst =
					FSL_SAI_MAXBURST_RX * pins;
				sai->dma_params_rx.fifo_num = pins +
					  (dl_cfg[dl_cfg_idx].offset[tx] << 4);
			}
		}

		snd_soc_dai_init_dma_data(cpu_dai, &sai->dma_params_tx,
				&sai->dma_params_rx);
	}

	if (__sw_hweight8(dl_cfg[dl_cfg_idx].mask[tx] & 0xFF) < pins) {
		dev_err(cpu_dai->dev, "channel not supported\n");
		return -EINVAL;
	}

	/*find a proper tcre setting*/
	for (i = 0; i < 8; i++) {
		trce_mask = (1 << (i + 1)) - 1;
		if (__sw_hweight8(dl_cfg[dl_cfg_idx].mask[tx] & trce_mask) == pins)
			break;
	}

	regmap_update_bits(sai->regmap, FSL_SAI_xCR3(tx, offset),
		   FSL_SAI_CR3_TRCE_MASK,
		   FSL_SAI_CR3_TRCE((dl_cfg[dl_cfg_idx].mask[tx] & trce_mask)));

	regmap_update_bits(sai->regmap, FSL_SAI_xCR4(tx, offset),
			   FSL_SAI_CR4_SYWD_MASK | FSL_SAI_CR4_FRSZ_MASK |
			   FSL_SAI_CR4_CHMOD_MASK,
			   val_cr4);
	regmap_update_bits(sai->regmap, FSL_SAI_xCR5(tx, offset),
			   FSL_SAI_CR5_WNW_MASK | FSL_SAI_CR5_W0W_MASK |
			   FSL_SAI_CR5_FBT_MASK, val_cr5);
	regmap_write(sai->regmap, FSL_SAI_xMR(tx),
			~0UL - ((1 << min(channels, slots)) - 1));
	return 0;
}

static int fsl_sai_hw_free(struct snd_pcm_substream *substream,
		struct snd_soc_dai *cpu_dai)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(cpu_dai);
	unsigned char offset = sai->soc->reg_offset;
	bool tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;

	regmap_update_bits(sai->regmap, FSL_SAI_xCR3(tx, offset),
				   FSL_SAI_CR3_TRCE_MASK, 0);

	if (!sai->slave_mode[tx] &&
			sai->mclk_streams & BIT(substream->stream)) {
		clk_disable_unprepare(sai->mclk_clk[sai->mclk_id[tx]]);
		sai->mclk_streams &= ~BIT(substream->stream);
	}

	return 0;
}


static int fsl_sai_trigger(struct snd_pcm_substream *substream, int cmd,
		struct snd_soc_dai *cpu_dai)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(cpu_dai);
	unsigned char offset = sai->soc->reg_offset;
	bool tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	u8 channels = substream->runtime->channels;
	u32 slots = (channels == 1) ? 2 : channels;
	u32 xcsr, count = 100;
	u32 pins;
	int i = 0, j = 0, k = 0, dl_cfg_cnt, dl_cfg_idx = 0;
	struct fsl_sai_dl_cfg *dl_cfg;

	if (sai->slots)
		slots = sai->slots;

	pins = DIV_ROUND_UP(channels, slots);

	if (sai->is_dsd) {
		pins = channels;
		dl_cfg = sai->dsd_dl_cfg;
		dl_cfg_cnt = sai->dsd_dl_cfg_cnt;
	} else {
		dl_cfg = sai->pcm_dl_cfg;
		dl_cfg_cnt = sai->pcm_dl_cfg_cnt;
	}

	for (i = 0; i < dl_cfg_cnt; i++) {
		if (dl_cfg[i].pins == pins) {
			dl_cfg_idx = i;
			break;
		}
	}

	i = 0;

	/*
	 * Asynchronous mode: Clear SYNC for both Tx and Rx.
	 * Rx sync with Tx clocks: Clear SYNC for Tx, set it for Rx.
	 * Tx sync with Rx clocks: Clear SYNC for Rx, set it for Tx.
	 */
	regmap_update_bits(sai->regmap, FSL_SAI_TCR2(offset), FSL_SAI_CR2_SYNC,
		           sai->synchronous[TX] ? FSL_SAI_CR2_SYNC : 0);
	regmap_update_bits(sai->regmap, FSL_SAI_RCR2(offset), FSL_SAI_CR2_SYNC,
			   sai->synchronous[RX] ? FSL_SAI_CR2_SYNC : 0);

	/*
	 * It is recommended that the transmitter is the last enabled
	 * and the first disabled.
	 */
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:

		while (tx && i < channels) {
			if (dl_cfg[dl_cfg_idx].mask[tx] & (1 << j)) {
				regmap_write(sai->regmap, FSL_SAI_TDR0 + j * 0x4, 0x0);
				i++;
				k++;
			}
			j++;

			if (k%pins == 0)
				j = 0;
		}

		regmap_update_bits(sai->regmap, FSL_SAI_xCSR(tx, offset),
				   FSL_SAI_CSR_FRDE, FSL_SAI_CSR_FRDE);

		regmap_update_bits(sai->regmap, FSL_SAI_xCSR(tx, offset),
				   FSL_SAI_CSR_TERE, FSL_SAI_CSR_TERE);
		regmap_update_bits(sai->regmap, FSL_SAI_xCSR(tx, offset),
				   FSL_SAI_CSR_SE, FSL_SAI_CSR_SE);
		if (!sai->synchronous[TX] && sai->synchronous[RX] && !tx) {
			regmap_update_bits(sai->regmap, FSL_SAI_xCSR((!tx), offset),
				   FSL_SAI_CSR_TERE, FSL_SAI_CSR_TERE);
		} else if (!sai->synchronous[RX] && sai->synchronous[TX] && tx) {
			regmap_update_bits(sai->regmap, FSL_SAI_xCSR((!tx), offset),
				   FSL_SAI_CSR_TERE, FSL_SAI_CSR_TERE);
		}

		regmap_update_bits(sai->regmap, FSL_SAI_xCSR(tx, offset),
				   FSL_SAI_CSR_xIE_MASK, FSL_SAI_FLAGS);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		regmap_update_bits(sai->regmap, FSL_SAI_xCSR(tx, offset),
				   FSL_SAI_CSR_FRDE, 0);
		regmap_update_bits(sai->regmap, FSL_SAI_xCSR(tx, offset),
				   FSL_SAI_CSR_xIE_MASK, 0);

		/* Check if the opposite FRDE is also disabled */
		regmap_read(sai->regmap, FSL_SAI_xCSR(!tx, offset), &xcsr);
		if (!(xcsr & FSL_SAI_CSR_FRDE)) {
			/* Disable both directions and reset their FIFOs */
			regmap_update_bits(sai->regmap, FSL_SAI_TCSR(offset),
					   FSL_SAI_CSR_TERE, 0);
			regmap_update_bits(sai->regmap, FSL_SAI_RCSR(offset),
					   FSL_SAI_CSR_TERE, 0);

			/* TERE will remain set till the end of current frame */
			do {
				udelay(10);
				regmap_read(sai->regmap, FSL_SAI_xCSR(tx, offset), &xcsr);
			} while (--count && xcsr & FSL_SAI_CSR_TERE);

			regmap_update_bits(sai->regmap, FSL_SAI_TCSR(offset),
					   FSL_SAI_CSR_FR, FSL_SAI_CSR_FR);
			regmap_update_bits(sai->regmap, FSL_SAI_RCSR(offset),
					   FSL_SAI_CSR_FR, FSL_SAI_CSR_FR);

			/*
			 * For sai master mode, after several open/close sai,
			 * there will be no frame clock, and can't recover
			 * anymore. Add software reset to fix this issue.
			 * This is a hardware bug, and will be fix in the
			 * next sai version.
			 */
			if (!sai->slave_mode[tx]) {
				/* Software Reset for both Tx and Rx */
				regmap_write(sai->regmap,
					     FSL_SAI_TCSR(offset), FSL_SAI_CSR_SR);
				regmap_write(sai->regmap,
					     FSL_SAI_RCSR(offset), FSL_SAI_CSR_SR);
				/* Clear SR bit to finish the reset */
				regmap_write(sai->regmap, FSL_SAI_TCSR(offset), 0);
				regmap_write(sai->regmap, FSL_SAI_RCSR(offset), 0);
			}
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int fsl_sai_startup(struct snd_pcm_substream *substream,
		struct snd_soc_dai *cpu_dai)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(cpu_dai);
	bool tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	int ret;

	if (sai->is_stream_opened[tx])
		return -EBUSY;
	else
		sai->is_stream_opened[tx] = true;

	/* EDMA engine needs periods of size multiple of tx/rx maxburst */
	if (sai->soc->constrain_period_size)
		snd_pcm_hw_constraint_step(substream->runtime, 0,
					   SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
					   tx ? sai->dma_params_tx.maxburst :
					   sai->dma_params_rx.maxburst);

	ret = snd_pcm_hw_constraint_list(substream->runtime, 0,
			SNDRV_PCM_HW_PARAM_RATE, &fsl_sai_rate_constraints);

	return ret;
}

static void fsl_sai_shutdown(struct snd_pcm_substream *substream,
		struct snd_soc_dai *cpu_dai)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(cpu_dai);
	bool tx = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;

	if (sai->is_stream_opened[tx])
		sai->is_stream_opened[tx] = false;
}

static const struct snd_soc_dai_ops fsl_sai_pcm_dai_ops = {
	.set_bclk_ratio = fsl_sai_set_dai_bclk_ratio,
	.set_sysclk	= fsl_sai_set_dai_sysclk,
	.set_fmt	= fsl_sai_set_dai_fmt,
	.set_tdm_slot	= fsl_sai_set_dai_tdm_slot,
	.hw_params	= fsl_sai_hw_params,
	.hw_free	= fsl_sai_hw_free,
	.trigger	= fsl_sai_trigger,
	.startup	= fsl_sai_startup,
	.shutdown	= fsl_sai_shutdown,
};

static int fsl_sai_dai_probe(struct snd_soc_dai *cpu_dai)
{
	struct fsl_sai *sai = dev_get_drvdata(cpu_dai->dev);
	unsigned char offset = sai->soc->reg_offset;

	regmap_update_bits(sai->regmap, FSL_SAI_TCR1(offset),
				FSL_SAI_CR1_RFW_MASK(sai->soc->fifo_depth),
				sai->soc->fifo_depth - FSL_SAI_MAXBURST_TX);
	regmap_update_bits(sai->regmap, FSL_SAI_RCR1(offset),
				FSL_SAI_CR1_RFW_MASK(sai->soc->fifo_depth),
				FSL_SAI_MAXBURST_RX - 1);

	snd_soc_dai_init_dma_data(cpu_dai, &sai->dma_params_tx,
				&sai->dma_params_rx);

	snd_soc_dai_set_drvdata(cpu_dai, sai);

	return 0;
}

static int fsl_sai_dai_resume(struct snd_soc_dai *cpu_dai)
{
	struct fsl_sai *sai = snd_soc_dai_get_drvdata(cpu_dai);
	int ret;

	if (!IS_ERR_OR_NULL(sai->pinctrl) && !IS_ERR_OR_NULL(sai->pins_state)) {
		ret = pinctrl_select_state(sai->pinctrl, sai->pins_state);
		if (ret) {
			dev_err(cpu_dai->dev,
				"failed to set proper pins state: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static struct snd_soc_dai_driver fsl_sai_dai_template = {
	.probe = fsl_sai_dai_probe,
	.playback = {
		.stream_name = "CPU-Playback",
		.channels_min = 1,
		.channels_max = 32,
		.rate_min = 8000,
		.rate_max = 2822400,
		.rates = SNDRV_PCM_RATE_KNOT,
		.formats = FSL_SAI_FORMATS,
	},
	.capture = {
		.stream_name = "CPU-Capture",
		.channels_min = 1,
		.channels_max = 32,
		.rate_min = 8000,
		.rate_max = 2822400,
		.rates = SNDRV_PCM_RATE_KNOT,
		.formats = FSL_SAI_FORMATS,
	},
	.resume  = fsl_sai_dai_resume,
	.ops = &fsl_sai_pcm_dai_ops,
};

static const struct snd_soc_component_driver fsl_component = {
	.name	= "fsl-sai",
};

static struct reg_default fsl_sai_v2_reg_defaults[] = {
	{FSL_SAI_TCR1(0), 0},
	{FSL_SAI_TCR2(0), 0},
	{FSL_SAI_TCR3(0), 0},
	{FSL_SAI_TCR4(0), 0},
	{FSL_SAI_TCR5(0), 0},
	{FSL_SAI_TDR0, 0},
	{FSL_SAI_TDR1, 0},
	{FSL_SAI_TMR,  0},
	{FSL_SAI_RCR1(0), 0},
	{FSL_SAI_RCR2(0), 0},
	{FSL_SAI_RCR3(0), 0},
	{FSL_SAI_RCR4(0), 0},
	{FSL_SAI_RCR5(0), 0},
	{FSL_SAI_RMR,  0},
};

static struct reg_default fsl_sai_v3_reg_defaults[] = {
	{FSL_SAI_TCR1(8), 0},
	{FSL_SAI_TCR2(8), 0},
	{FSL_SAI_TCR3(8), 0},
	{FSL_SAI_TCR4(8), 0},
	{FSL_SAI_TCR5(8), 0},
	{FSL_SAI_TDR0, 0},
	{FSL_SAI_TDR1, 0},
	{FSL_SAI_TDR2, 0},
	{FSL_SAI_TDR3, 0},
	{FSL_SAI_TDR4, 0},
	{FSL_SAI_TDR5, 0},
	{FSL_SAI_TDR6, 0},
	{FSL_SAI_TDR7, 0},
	{FSL_SAI_TMR,  0},
	{FSL_SAI_RCR1(8), 0},
	{FSL_SAI_RCR2(8), 0},
	{FSL_SAI_RCR3(8), 0},
	{FSL_SAI_RCR4(8), 0},
	{FSL_SAI_RCR5(8), 0},
	{FSL_SAI_RMR,  0},
	{FSL_SAI_MCTL, 0},
	{FSL_SAI_MDIV, 0},
};

static bool fsl_sai_readable_reg(struct device *dev, unsigned int reg)
{
	struct fsl_sai *sai = dev_get_drvdata(dev);
	unsigned char offset = sai->soc->reg_offset;

	if (reg >= FSL_SAI_TCSR(offset) && reg <= FSL_SAI_TCR5(offset))
		return true;

	if (reg >= FSL_SAI_RCSR(offset) && reg <= FSL_SAI_RCR5(offset))
		return true;

	switch (reg) {
	case FSL_SAI_TFR0:
	case FSL_SAI_TFR1:
	case FSL_SAI_TFR2:
	case FSL_SAI_TFR3:
	case FSL_SAI_TFR4:
	case FSL_SAI_TFR5:
	case FSL_SAI_TFR6:
	case FSL_SAI_TFR7:
	case FSL_SAI_TMR:
	case FSL_SAI_RDR0:
	case FSL_SAI_RDR1:
	case FSL_SAI_RDR2:
	case FSL_SAI_RDR3:
	case FSL_SAI_RDR4:
	case FSL_SAI_RDR5:
	case FSL_SAI_RDR6:
	case FSL_SAI_RDR7:
	case FSL_SAI_RFR0:
	case FSL_SAI_RFR1:
	case FSL_SAI_RFR2:
	case FSL_SAI_RFR3:
	case FSL_SAI_RFR4:
	case FSL_SAI_RFR5:
	case FSL_SAI_RFR6:
	case FSL_SAI_RFR7:
	case FSL_SAI_RMR:
	case FSL_SAI_MCTL:
	case FSL_SAI_MDIV:
	case FSL_SAI_VERID:
	case FSL_SAI_PARAM:
	case FSL_SAI_TTCTN:
	case FSL_SAI_RTCTN:
	case FSL_SAI_TTCTL:
	case FSL_SAI_TBCTN:
	case FSL_SAI_TTCAP:
	case FSL_SAI_RTCTL:
	case FSL_SAI_RBCTN:
	case FSL_SAI_RTCAP:
		return true;
	default:
		return false;
	}
}

static bool fsl_sai_volatile_reg(struct device *dev, unsigned int reg)
{
	struct fsl_sai *sai = dev_get_drvdata(dev);
	unsigned char offset = sai->soc->reg_offset;

	if (reg == FSL_SAI_TCSR(offset) || reg == FSL_SAI_RCSR(offset))
		return true;

	if (sai->soc->reg_offset == 8 && (reg == FSL_SAI_VERID ||
				reg == FSL_SAI_PARAM))
		return true;

	switch (reg) {
	case FSL_SAI_TFR0:
	case FSL_SAI_TFR1:
	case FSL_SAI_TFR2:
	case FSL_SAI_TFR3:
	case FSL_SAI_TFR4:
	case FSL_SAI_TFR5:
	case FSL_SAI_TFR6:
	case FSL_SAI_TFR7:
	case FSL_SAI_RFR0:
	case FSL_SAI_RFR1:
	case FSL_SAI_RFR2:
	case FSL_SAI_RFR3:
	case FSL_SAI_RFR4:
	case FSL_SAI_RFR5:
	case FSL_SAI_RFR6:
	case FSL_SAI_RFR7:
	case FSL_SAI_RDR0:
	case FSL_SAI_RDR1:
	case FSL_SAI_RDR2:
	case FSL_SAI_RDR3:
	case FSL_SAI_RDR4:
	case FSL_SAI_RDR5:
	case FSL_SAI_RDR6:
	case FSL_SAI_RDR7:
	case FSL_SAI_TTCTN:
	case FSL_SAI_TTCTL:
	case FSL_SAI_TBCTN:
	case FSL_SAI_TTCAP:
	case FSL_SAI_RTCTN:
	case FSL_SAI_RTCTL:
	case FSL_SAI_RBCTN:
	case FSL_SAI_RTCAP:
		return true;
	default:
		return false;
	}
}

static bool fsl_sai_writeable_reg(struct device *dev, unsigned int reg)
{
	struct fsl_sai *sai = dev_get_drvdata(dev);
	unsigned char offset = sai->soc->reg_offset;

	if (reg >= FSL_SAI_TCSR(offset) && reg <= FSL_SAI_TCR5(offset))
		return true;

	if (reg >= FSL_SAI_RCSR(offset) && reg <= FSL_SAI_RCR5(offset))
		return true;

	switch (reg) {
	case FSL_SAI_TDR0:
	case FSL_SAI_TDR1:
	case FSL_SAI_TDR2:
	case FSL_SAI_TDR3:
	case FSL_SAI_TDR4:
	case FSL_SAI_TDR5:
	case FSL_SAI_TDR6:
	case FSL_SAI_TDR7:
	case FSL_SAI_TMR:
	case FSL_SAI_RMR:
	case FSL_SAI_MCTL:
	case FSL_SAI_MDIV:
	case FSL_SAI_TTCTL:
	case FSL_SAI_RTCTL:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config fsl_sai_v2_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,

	.max_register = FSL_SAI_RMR,
	.reg_defaults = fsl_sai_v2_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(fsl_sai_v2_reg_defaults),
	.readable_reg = fsl_sai_readable_reg,
	.volatile_reg = fsl_sai_volatile_reg,
	.writeable_reg = fsl_sai_writeable_reg,
	.cache_type = REGCACHE_FLAT,
};

static const struct regmap_config fsl_sai_v3_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,

	.max_register = FSL_SAI_MDIV,
	.reg_defaults = fsl_sai_v3_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(fsl_sai_v3_reg_defaults),
	.readable_reg = fsl_sai_readable_reg,
	.volatile_reg = fsl_sai_volatile_reg,
	.writeable_reg = fsl_sai_writeable_reg,
	.cache_type = REGCACHE_FLAT,
};

static const struct of_device_id fsl_sai_ids[] = {
	{ .compatible = "fsl,vf610-sai", .data = &fsl_sai_vf610 },
	{ .compatible = "fsl,imx6sx-sai", .data = &fsl_sai_imx6sx },
	{ .compatible = "fsl,imx6ul-sai", .data = &fsl_sai_imx6ul },
	{ .compatible = "fsl,imx7ulp-sai", .data = &fsl_sai_imx7ulp },
	{ .compatible = "fsl,imx8mq-sai", .data = &fsl_sai_imx8mq },
	{ .compatible = "fsl,imx8qm-sai", .data = &fsl_sai_imx8qm },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, fsl_sai_ids);

static unsigned int fsl_sai_calc_dl_off(unsigned long dl_mask)
{
	int fbidx, nbidx, offset;

	fbidx = find_first_bit(&dl_mask, 8);
	nbidx = find_next_bit(&dl_mask, 8, fbidx + 1);
	offset = nbidx - fbidx - 1;

	return (offset < 0 || offset >= 7 ? 0 : offset);
}

static int fsl_sai_read_dlcfg(struct platform_device *pdev, char *pn,
	struct fsl_sai_dl_cfg **rcfg, unsigned int soc_dl)
{
	int ret, elems, i, index, num_cfg;
	struct device_node *np = pdev->dev.of_node;
	struct fsl_sai_dl_cfg *cfg;
	u32 rx, tx, pins;

	*rcfg = NULL;

	elems = of_property_count_u32_elems(np, pn);

	/* consider default value "0 0x1 0x1" if property is missing */
	if (elems <= 0)
		elems = 3;

	if (elems % 3) {
		dev_err(&pdev->dev,
			"Number of elements in %s must be divisible to 3.\n", pn);
		return -EINVAL;
	}

	num_cfg = elems / 3;
	cfg = devm_kzalloc(&pdev->dev, num_cfg * sizeof(*cfg), GFP_KERNEL);
	if (cfg == NULL) {
		dev_err(&pdev->dev, "Cannot allocate memory for %s.\n", pn);
		return -ENOMEM;
	}

	for (i = 0, index = 0; i < num_cfg; i++) {
		ret = of_property_read_u32_index(np, pn, index++, &pins);
		if (ret)
			pins = 0;

		ret = of_property_read_u32_index(np, pn, index++, &rx);
		if (ret)
			rx = 1;

		ret = of_property_read_u32_index(np, pn, index++, &tx);
		if (ret)
			tx = 1;

		if ((rx & ~soc_dl) || (tx & ~soc_dl)) {
			dev_err(&pdev->dev,
				"%s: dataline cfg[%d] setting error, mask is 0x%x\n",
				 pn, i, soc_dl);
			return -EINVAL;
		}

		cfg[i].pins = pins;
		cfg[i].mask[0] = rx;
		cfg[i].offset[0] = fsl_sai_calc_dl_off(rx);
		cfg[i].mask[1] = tx;
		cfg[i].offset[1] = fsl_sai_calc_dl_off(tx);
	}

	*rcfg = cfg;
	return num_cfg;
}

static int fsl_sai_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	const struct of_device_id *of_id;
	struct fsl_sai *sai;
	struct regmap *gpr;
	struct resource *res;
	void __iomem *base;
	char tmp[8];
	int irq, ret, i;
	int index;
	struct regmap_config fsl_sai_regmap_config = fsl_sai_v2_regmap_config;
	unsigned long irqflags = 0;

	sai = devm_kzalloc(&pdev->dev, sizeof(*sai), GFP_KERNEL);
	if (!sai)
		return -ENOMEM;

	sai->pdev = pdev;

	of_id = of_match_device(fsl_sai_ids, &pdev->dev);
	if (!of_id || !of_id->data)
		return -EINVAL;

	sai->is_lsb_first = of_property_read_bool(np, "lsb-first");
	sai->soc = of_id->data;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	if (sai->soc->reg_offset == 8)
		fsl_sai_regmap_config = fsl_sai_v3_regmap_config;

	memcpy(&sai->cpu_dai_drv, &fsl_sai_dai_template,
	       sizeof(fsl_sai_dai_template));

	sai->regmap = devm_regmap_init_mmio_clk(&pdev->dev,
			NULL, base, &fsl_sai_regmap_config);
	if (IS_ERR(sai->regmap)) {
		dev_err(&pdev->dev, "regmap init failed\n");
		return PTR_ERR(sai->regmap);
	}

	/* No error out for old DTB cases but only mark the clock NULL */
	sai->bus_clk = devm_clk_get(&pdev->dev, "bus");
	if (IS_ERR(sai->bus_clk)) {
		dev_err(&pdev->dev, "failed to get bus clock: %ld\n",
				PTR_ERR(sai->bus_clk));
		sai->bus_clk = NULL;
	}

	for (i = 0; i < FSL_SAI_MCLK_MAX; i++) {
		sprintf(tmp, "mclk%d", i);
		sai->mclk_clk[i] = devm_clk_get(&pdev->dev, tmp);
		if (IS_ERR(sai->mclk_clk[i])) {
			dev_err(&pdev->dev, "failed to get mclk%d clock: %ld\n",
					i, PTR_ERR(sai->mclk_clk[i]));
			sai->mclk_clk[i] = NULL;
		}
	}

	sai->pll8k_clk = devm_clk_get(&pdev->dev, "pll8k");
	if (IS_ERR(sai->pll8k_clk))
		sai->pll8k_clk = NULL;

	sai->pll11k_clk = devm_clk_get(&pdev->dev, "pll11k");
	if (IS_ERR(sai->pll11k_clk))
		sai->pll11k_clk = NULL;

	if (of_find_property(np, "fsl,sai-multi-lane", NULL))
		sai->is_multi_lane = true;

	/*dataline mask for rx and tx*/
	ret = fsl_sai_read_dlcfg(pdev, "fsl,dataline", &sai->pcm_dl_cfg,
					sai->soc->dataline);
	if (ret < 0)
		return ret;

	sai->pcm_dl_cfg_cnt = ret;

	ret = fsl_sai_read_dlcfg(pdev, "fsl,dataline,dsd", &sai->dsd_dl_cfg,
					sai->soc->dataline);
	if (ret < 0)
		return ret;

	sai->dsd_dl_cfg_cnt = ret;

	if ((of_find_property(np, "fsl,i2s-xtor", NULL) != NULL) ||
	    (of_find_property(np, "fsl,txm-rxs", NULL) != NULL))
	{
		sai->masterflag[FSL_FMT_TRANSMITTER] = SND_SOC_DAIFMT_CBS_CFS;
		sai->masterflag[FSL_FMT_RECEIVER] = SND_SOC_DAIFMT_CBM_CFM;
	} else {
		if (!of_property_read_u32(np, "fsl,txmasterflag",
			&sai->masterflag[FSL_FMT_TRANSMITTER]))
			sai->masterflag[FSL_FMT_TRANSMITTER] <<= 12;
		if (!of_property_read_u32(np, "fsl,rxmasterflag",
			&sai->masterflag[FSL_FMT_RECEIVER]))
			sai->masterflag[FSL_FMT_RECEIVER] <<= 12;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "no irq for node %s\n", pdev->name);
		return irq;
	}

	/* SAI shared interrupt */
	if (of_property_read_bool(np, "fsl,shared-interrupt"))
		irqflags = IRQF_SHARED;

	ret = devm_request_irq(&pdev->dev, irq, fsl_sai_isr, irqflags, np->name,
			       sai);
	if (ret) {
		dev_err(&pdev->dev, "failed to claim irq %u\n", irq);
		return ret;
	}

	memcpy(&sai->cpu_dai_drv, &fsl_sai_dai_template,
	       sizeof(fsl_sai_dai_template));

	/* Sync Tx with Rx as default by following old DT binding */
	sai->synchronous[RX] = true;
	sai->synchronous[TX] = false;
	sai->cpu_dai_drv.symmetric_rates = 1;
	sai->cpu_dai_drv.symmetric_channels = 1;
	sai->cpu_dai_drv.symmetric_samplebits = 1;

	if (of_find_property(np, "fsl,sai-synchronous-rx", NULL) &&
	    of_find_property(np, "fsl,sai-asynchronous", NULL)) {
		/* error out if both synchronous and asynchronous are present */
		dev_err(&pdev->dev, "invalid binding for synchronous mode\n");
		return -EINVAL;
	}

	if (of_find_property(np, "fsl,sai-synchronous-rx", NULL)) {
		/* Sync Rx with Tx */
		sai->synchronous[RX] = false;
		sai->synchronous[TX] = true;
	} else if (of_find_property(np, "fsl,sai-asynchronous", NULL)) {
		/* Discard all settings for asynchronous mode */
		sai->synchronous[RX] = false;
		sai->synchronous[TX] = false;
		sai->cpu_dai_drv.symmetric_rates = 0;
		sai->cpu_dai_drv.symmetric_channels = 0;
		sai->cpu_dai_drv.symmetric_samplebits = 0;
	}

	platform_set_drvdata(pdev, sai);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);

	ret = fsl_sai_check_ver(&pdev->dev);
	if (ret < 0)
		dev_warn(&pdev->dev, "Error reading SAI version: %d\n", ret);

	if (of_find_property(np, "fsl,sai-mclk-direction-output", NULL) &&
	    of_device_is_compatible(np, "fsl,imx6ul-sai")) {
		gpr = syscon_regmap_lookup_by_compatible("fsl,imx6ul-iomuxc-gpr");
		if (IS_ERR(gpr)) {
			dev_err(&pdev->dev, "cannot find iomuxc registers\n");
			return PTR_ERR(gpr);
		}

		index = of_alias_get_id(np, "sai");
		if (index < 0)
			return index;

		regmap_update_bits(gpr, IOMUXC_GPR1, MCLK_DIR(index),
				   MCLK_DIR(index));
	}

	if (of_find_property(np, "fsl,sai-mclk-direction-output", NULL) &&
	    sai->verid.id >= FSL_SAI_VERID_0301) {
		regmap_update_bits(sai->regmap, FSL_SAI_MCTL,
				   FSL_SAI_MCTL_MCLK_EN, FSL_SAI_MCTL_MCLK_EN);
	}

	if (sai->verid.timestamp_en) {
		if (of_find_property(np, "fsl,sai-monitor-spdif", NULL) &&
		    of_device_is_compatible(np, "fsl,imx8mm-sai")) {
			sai->regmap_gpr = syscon_regmap_lookup_by_compatible("fsl,imx8mm-iomuxc-gpr");
			if (IS_ERR(sai->regmap_gpr))
				dev_warn(&pdev->dev, "cannot find iomuxc registers\n");

			sai->gpr_idx = of_alias_get_id(np, "sai");
			if (sai->gpr_idx < 0)
				dev_warn(&pdev->dev, "cannot find sai alias id\n");

			if (sai->gpr_idx > 0 && !IS_ERR(sai->regmap_gpr))
				sai->monitor_spdif = true;
		}

		if (sysfs_create_group(&pdev->dev.kobj, fsl_sai_get_dev_attribute_group(sai->monitor_spdif)))
			dev_err(&pdev->dev, "fail to create sys group\n");
	}

	pm_runtime_put_sync(&pdev->dev);

	sai->dma_params_rx.chan_name = "rx";
	sai->dma_params_tx.chan_name = "tx";
	sai->dma_params_rx.addr = res->start + FSL_SAI_RDR0;
	sai->dma_params_tx.addr = res->start + FSL_SAI_TDR0;
	sai->dma_params_rx.maxburst = FSL_SAI_MAXBURST_RX;
	sai->dma_params_tx.maxburst = FSL_SAI_MAXBURST_TX;

	sai->pinctrl = devm_pinctrl_get(&pdev->dev);

	regcache_cache_only(sai->regmap, true);

	ret = devm_snd_soc_register_component(&pdev->dev, &fsl_component,
					      &sai->cpu_dai_drv, 1);
	if (ret)
		return ret;

	if (sai->soc->imx)
		return imx_pcm_platform_register(&pdev->dev);
	else
		return devm_snd_dmaengine_pcm_register(&pdev->dev, NULL, 0);
}

static int fsl_sai_remove(struct platform_device *pdev)
{
	struct fsl_sai *sai = dev_get_drvdata(&pdev->dev);

	pm_runtime_disable(&pdev->dev);

	if (sai->verid.timestamp_en)
		sysfs_remove_group(&pdev->dev.kobj,  fsl_sai_get_dev_attribute_group(sai->monitor_spdif));

	return 0;
}

#ifdef CONFIG_PM
static int fsl_sai_runtime_suspend(struct device *dev)
{
	struct fsl_sai *sai = dev_get_drvdata(dev);

	regcache_cache_only(sai->regmap, true);

	release_bus_freq(BUS_FREQ_AUDIO);

	if (sai->mclk_streams & BIT(SNDRV_PCM_STREAM_CAPTURE))
		clk_disable_unprepare(sai->mclk_clk[sai->mclk_id[0]]);

	if (sai->mclk_streams & BIT(SNDRV_PCM_STREAM_PLAYBACK))
		clk_disable_unprepare(sai->mclk_clk[sai->mclk_id[1]]);

	clk_disable_unprepare(sai->bus_clk);

	if (sai->soc->flags & SAI_FLAG_PMQOS)
		pm_qos_remove_request(&sai->pm_qos_req);

	regcache_cache_only(sai->regmap, true);
	regcache_mark_dirty(sai->regmap);

	return 0;
}

static int fsl_sai_runtime_resume(struct device *dev)
{
	struct fsl_sai *sai = dev_get_drvdata(dev);
	unsigned char offset = sai->soc->reg_offset;
	int ret;

	ret = clk_prepare_enable(sai->bus_clk);
	if (ret) {
		dev_err(dev, "failed to enable bus clock: %d\n", ret);
		return ret;
	}

	if (sai->mclk_streams & BIT(SNDRV_PCM_STREAM_PLAYBACK)) {
		ret = clk_prepare_enable(sai->mclk_clk[sai->mclk_id[1]]);
		if (ret)
			goto disable_bus_clk;
	}

	if (sai->mclk_streams & BIT(SNDRV_PCM_STREAM_CAPTURE)) {
		ret = clk_prepare_enable(sai->mclk_clk[sai->mclk_id[0]]);
		if (ret)
			goto disable_tx_clk;
	}

	request_bus_freq(BUS_FREQ_AUDIO);

	if (sai->soc->flags & SAI_FLAG_PMQOS)
		pm_qos_add_request(&sai->pm_qos_req,
				PM_QOS_CPU_DMA_LATENCY, 0);

	regcache_cache_only(sai->regmap, false);
	regcache_mark_dirty(sai->regmap);

	regmap_write(sai->regmap, FSL_SAI_TCSR(offset), FSL_SAI_CSR_SR);
	regmap_write(sai->regmap, FSL_SAI_RCSR(offset), FSL_SAI_CSR_SR);
	usleep_range(1000, 2000);
	regmap_write(sai->regmap, FSL_SAI_TCSR(offset), 0);
	regmap_write(sai->regmap, FSL_SAI_RCSR(offset), 0);

	ret = regcache_sync(sai->regmap);
	if (ret)
		goto disable_rx_clk;

	return 0;

disable_rx_clk:
	if (sai->mclk_streams & BIT(SNDRV_PCM_STREAM_CAPTURE))
		clk_disable_unprepare(sai->mclk_clk[sai->mclk_id[0]]);
disable_tx_clk:
	if (sai->mclk_streams & BIT(SNDRV_PCM_STREAM_PLAYBACK))
		clk_disable_unprepare(sai->mclk_clk[sai->mclk_id[1]]);
disable_bus_clk:
	clk_disable_unprepare(sai->bus_clk);

	return ret;
}
#endif /* CONFIG_PM */

static const struct dev_pm_ops fsl_sai_pm_ops = {
	SET_RUNTIME_PM_OPS(fsl_sai_runtime_suspend,
			   fsl_sai_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

static struct platform_driver fsl_sai_driver = {
	.probe = fsl_sai_probe,
	.remove = fsl_sai_remove,
	.driver = {
		.name = "fsl-sai",
		.pm = &fsl_sai_pm_ops,
		.of_match_table = fsl_sai_ids,
	},
};
module_platform_driver(fsl_sai_driver);

MODULE_DESCRIPTION("Freescale Soc SAI Interface");
MODULE_AUTHOR("Xiubo Li, <Li.Xiubo@freescale.com>");
MODULE_ALIAS("platform:fsl-sai");
MODULE_LICENSE("GPL");
