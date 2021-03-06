/*
 * DMA support for Internal DMAC with SDHI SD/SDIO controller
 *
 * Copyright (C) 2016-17 Renesas Electronics Corporation
 * Copyright (C) 2016-17 Horms Solutions, Simon Horman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io-64-nonatomic-hi-lo.h>
#include <linux/mfd/tmio.h>
#include <linux/mmc/host.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/scatterlist.h>
#include <linux/sys_soc.h>

#include "renesas_sdhi.h"
#include "tmio_mmc.h"

#define DM_CM_DTRAN_MODE	0x820
#define DM_CM_DTRAN_CTRL	0x828
#define DM_CM_RST		0x830
#define DM_CM_INFO1		0x840
#define DM_CM_INFO1_MASK	0x848
#define DM_CM_INFO2		0x850
#define DM_CM_INFO2_MASK	0x858
#define DM_DTRAN_ADDR		0x880

/* DM_CM_DTRAN_MODE */
#define DTRAN_MODE_CH_NUM_CH0	0	/* "downstream" = for write commands */
#define DTRAN_MODE_CH_NUM_CH1	BIT(16)	/* "uptream" = for read commands */
#define DTRAN_MODE_BUS_WID_TH	(BIT(5) | BIT(4))
#define DTRAN_MODE_ADDR_MODE	BIT(0)	/* 1 = Increment address */

/* DM_CM_DTRAN_CTRL */
#define DTRAN_CTRL_DM_START	BIT(0)

/* DM_CM_RST */
#define RST_DTRANRST1		BIT(9)
#define RST_DTRANRST0		BIT(8)
#define RST_RESERVED_BITS	GENMASK_ULL(32, 0)

/* DM_CM_INFO1 and DM_CM_INFO1_MASK */
#define INFO1_CLEAR		0
#define INFO1_DTRANEND1_BIT20	BIT(20)
#define INFO1_DTRANEND1_BIT17	BIT(17)
#define INFO1_DTRANEND0		BIT(16)

/* DM_CM_INFO2 and DM_CM_INFO2_MASK */
#define INFO2_DTRANERR1		BIT(17)
#define INFO2_DTRANERR0		BIT(16)

/*
 * Specification of this driver:
 * - host->chan_{rx,tx} will be used as a flag of enabling/disabling the dma
 * - Since this SDHI DMAC register set has 16 but 32-bit width, we
 *   need a custom accessor.
 */

/* Definitions for sampling clocks */
static struct renesas_sdhi_scc rcar_gen3_scc_taps[] = {
	{
		.clk_rate = 0,
		.tap = 0x00000300,
	},
};

static const struct renesas_sdhi_of_data of_rcar_gen3_compatible = {
	.tmio_flags	= TMIO_MMC_HAS_IDLE_WAIT | TMIO_MMC_WRPROTECT_DISABLE |
			  TMIO_MMC_CLK_ACTUAL | TMIO_MMC_HAVE_CBSY |
			  TMIO_MMC_MIN_RCAR2,
	.capabilities	= MMC_CAP_SD_HIGHSPEED | MMC_CAP_SDIO_IRQ |
			  MMC_CAP_CMD23,
	.bus_shift	= 2,
	.scc_offset	= 0x1000,
	.taps		= rcar_gen3_scc_taps,
	.taps_num	= ARRAY_SIZE(rcar_gen3_scc_taps),
	/* Gen3 SDHI DMAC can handle 0xffffffff blk count, but seg = 1 */
	.max_blk_count	= 0xffffffff,
	.max_segs	= 1,
};

static const struct of_device_id renesas_sdhi_internal_dmac_of_match[] = {
	{ .compatible = "renesas,sdhi-r8a7795", .data = &of_rcar_gen3_compatible, },
	{ .compatible = "renesas,sdhi-r8a7796", .data = &of_rcar_gen3_compatible, },
	{ .compatible = "renesas,sdhi-r8a77965", .data = &of_rcar_gen3_compatible, },
	{ .compatible = "renesas,sdhi-r8a77990", .data = &of_rcar_gen3_compatible, },
	{ .compatible = "renesas,sdhi-r8a77995", .data = &of_rcar_gen3_compatible, },
	{},
};
MODULE_DEVICE_TABLE(of, renesas_sdhi_internal_dmac_of_match);

static void
renesas_sdhi_internal_dmac_dm_write(struct tmio_mmc_host *host,
				    int addr, u64 val)
{
	writeq(val, host->ctl + addr);
}

static u32
renesas_sdhi_internal_dmac_dm_read(struct tmio_mmc_host *host, int addr)
{
	return readl(host->ctl + addr);
}

static void
renesas_sdhi_internal_dmac_enable_dma(struct tmio_mmc_host *host, bool enable)
{
	if (!host->chan_tx || !host->chan_rx)
		return;

	if (!enable)
		renesas_sdhi_internal_dmac_dm_write(host, DM_CM_INFO1,
						    INFO1_CLEAR);

	if (host->dma->enable) {
		host->dma_irq_mask = ~(host->dma_tranend1 | INFO1_DTRANEND0);
		host->dma->enable(host, enable);
		renesas_sdhi_internal_dmac_dm_write(host, DM_CM_INFO1_MASK,
						    host->dma_irq_mask);
	}
}

static void
renesas_sdhi_internal_dmac_abort_dma(struct tmio_mmc_host *host) {
	u64 val = RST_DTRANRST1 | RST_DTRANRST0;

	renesas_sdhi_internal_dmac_enable_dma(host, false);

	renesas_sdhi_internal_dmac_dm_write(host, DM_CM_RST,
					    RST_RESERVED_BITS & ~val);
	renesas_sdhi_internal_dmac_dm_write(host, DM_CM_RST,
					    RST_RESERVED_BITS | val);

	renesas_sdhi_internal_dmac_enable_dma(host, true);
}

static void
renesas_sdhi_internal_dmac_dataend_dma(struct tmio_mmc_host *host) {
	tasklet_schedule(&host->dma_complete);
}

static void
renesas_sdhi_internal_dmac_start_dma(struct tmio_mmc_host *host,
				     struct mmc_data *data)
{
	struct scatterlist *sg = host->sg_ptr;
	u32 dtran_mode = DTRAN_MODE_BUS_WID_TH | DTRAN_MODE_ADDR_MODE;

	/* This DMAC cannot handle if sg_len is not 1 */
	WARN_ON(host->sg_len > 1);

	if (!tmio_mmc_pre_dma_transfer(host, data, COOKIE_MAPPED))
		goto force_pio;

	if (data->flags & MMC_DATA_READ) {
		dtran_mode |= DTRAN_MODE_CH_NUM_CH1;
	} else {
		dtran_mode |= DTRAN_MODE_CH_NUM_CH0;
	}

	tmio_mmc_clear_transtate(host);
	renesas_sdhi_internal_dmac_enable_dma(host, true);

	/* set dma parameters */
	renesas_sdhi_internal_dmac_dm_write(host, DM_CM_DTRAN_MODE,
					    dtran_mode);
	renesas_sdhi_internal_dmac_dm_write(host, DM_DTRAN_ADDR,
					    sg_dma_address(sg));

	return;

force_pio:
	host->force_pio = true;
	renesas_sdhi_internal_dmac_enable_dma(host, false);
}

static void renesas_sdhi_internal_dmac_issue_tasklet_fn(unsigned long arg)
{
	struct tmio_mmc_host *host = (struct tmio_mmc_host *)arg;

	tmio_mmc_enable_mmc_irqs(host, TMIO_STAT_DATAEND |
				 TMIO_STAT_DATATIMEOUT);

	/* start the DMAC */
	renesas_sdhi_internal_dmac_dm_write(host, DM_CM_DTRAN_CTRL,
					    DTRAN_CTRL_DM_START);
}

static void renesas_sdhi_internal_dmac_complete_tasklet_fn(unsigned long arg)
{
	struct tmio_mmc_host *host = (struct tmio_mmc_host *)arg;

	spin_lock_irq(&host->lock);

	if (!host->data)
		goto out;

	renesas_sdhi_internal_dmac_enable_dma(host, false);
	if (host->data && host->data->host_cookie == COOKIE_MAPPED) {
		dma_unmap_sg(&host->pdev->dev, host->sg_ptr, host->sg_len,
			     mmc_get_dma_dir(host->data));
		host->data->host_cookie = COOKIE_UNMAPPED;
	}

	tmio_mmc_do_data_irq(host);
out:
	spin_unlock_irq(&host->lock);
}

static bool renesas_sdhi_internal_dmac_dma_irq(struct tmio_mmc_host *host)
{
	unsigned int ireg, status;

	status = renesas_sdhi_internal_dmac_dm_read(host, DM_CM_INFO1);
	ireg = status & ~host->dma_irq_mask;

	if (ireg & INFO1_DTRANEND0) {
		renesas_sdhi_internal_dmac_dm_write(host, DM_CM_INFO1, ireg &
						    ~INFO1_DTRANEND0);
		tmio_mmc_set_transtate(host, TMIO_TRANSTATE_DEND);
		return true;
	}

	if (ireg & host->dma_tranend1) {
		renesas_sdhi_internal_dmac_dm_write(host, DM_CM_INFO1, ireg &
						    ~host->dma_tranend1);
		tmio_mmc_set_transtate(host, TMIO_TRANSTATE_DEND);
		return true;
	}
	return false;
}

static void
renesas_sdhi_internal_dmac_request_dma(struct tmio_mmc_host *host,
				       struct tmio_mmc_data *pdata)
{
	/* Each value is set to non-zero to assume "enabling" each DMA */
	host->chan_rx = host->chan_tx = (void *)0xdeadbeaf;

	host->dma_tranend1 = (host->sdhi_quirks & DTRAEND1_SET_BIT17) ?
		INFO1_DTRANEND1_BIT17 : INFO1_DTRANEND1_BIT20;

	tasklet_init(&host->dma_complete,
		     renesas_sdhi_internal_dmac_complete_tasklet_fn,
		     (unsigned long)host);
	tasklet_init(&host->dma_issue,
		     renesas_sdhi_internal_dmac_issue_tasklet_fn,
		     (unsigned long)host);
}

static void
renesas_sdhi_internal_dmac_release_dma(struct tmio_mmc_host *host)
{
	/* Each value is set to zero to assume "disabling" each DMA */
	host->chan_rx = host->chan_tx = NULL;
}

static const struct tmio_mmc_dma_ops renesas_sdhi_internal_dmac_dma_ops = {
	.start = renesas_sdhi_internal_dmac_start_dma,
	.enable = renesas_sdhi_internal_dmac_enable_dma,
	.request = renesas_sdhi_internal_dmac_request_dma,
	.release = renesas_sdhi_internal_dmac_release_dma,
	.abort = renesas_sdhi_internal_dmac_abort_dma,
	.dataend = renesas_sdhi_internal_dmac_dataend_dma,
	.dma_irq = renesas_sdhi_internal_dmac_dma_irq,
};

/*
 * Whitelist of specific R-Car Gen3 SoC ES versions to use this DMAC
 * implementation as others may use a different implementation.
 */
static const struct soc_device_attribute gen3_soc_whitelist[] = {
	/* generic ones */
	{ .soc_id = "r8a7795" },
	{ .soc_id = "r8a7796" },
	{ .soc_id = "r8a77965" },
	{ .soc_id = "r8a77990" },
	{ .soc_id = "r8a77995" },
	{ /* sentinel */ }
};

static int renesas_sdhi_internal_dmac_probe(struct platform_device *pdev)
{
	if (!soc_device_match(gen3_soc_whitelist))
		return -ENODEV;

#ifndef CONFIG_MMC_SDHI_PIO
	return renesas_sdhi_probe(pdev, &renesas_sdhi_internal_dmac_dma_ops);
#else
	return renesas_sdhi_probe(pdev, NULL);
#endif
}

static const struct dev_pm_ops renesas_sdhi_internal_dmac_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(tmio_mmc_host_runtime_suspend,
			   tmio_mmc_host_runtime_resume,
			   NULL)
};

static struct platform_driver renesas_internal_dmac_sdhi_driver = {
	.driver		= {
		.name	= "renesas_sdhi_internal_dmac",
		.pm	= &renesas_sdhi_internal_dmac_dev_pm_ops,
		.of_match_table = renesas_sdhi_internal_dmac_of_match,
	},
	.probe		= renesas_sdhi_internal_dmac_probe,
	.remove		= renesas_sdhi_remove,
};

module_platform_driver(renesas_internal_dmac_sdhi_driver);

MODULE_DESCRIPTION("Renesas SDHI driver for internal DMAC");
MODULE_AUTHOR("Yoshihiro Shimoda");
MODULE_LICENSE("GPL v2");
