/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Write-only, 4-wire, SPI mode-0 controller driven by the RP2040 PIO.
 * Targets an ST7789 display. The transceive path streams TX bytes into the
 * PIO TX FIFO via the Zephyr DMA framework (dma_config()/dma_start() against
 * the dma_rpi_pico controller), mirroring the in-tree PL022 SPI driver
 * (zephyr/drivers/spi/spi_pl022.c). Completion is signalled through the DMA
 * callback, which wakes the transceive() thread.
 *
 * Structure mirrors the in-tree driver
 * zephyr/drivers/spi/spi_rpi_pico_pio.c, simplified to the write-only
 * mode-0 case (no RX, no SIO/half-duplex, no other SPI modes).
 */

#define DT_DRV_COMPAT zpsu_pico_spi_pio_dma

#define LOG_LEVEL CONFIG_SPI_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_pio_dma);

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/dt-bindings/dma/rpi-pico-dma-common.h>
#include <zephyr/sys/printk.h>
#include "spi_context.h"

#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>

#include <hardware/pio.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct spi_pico_pio_config {
	const struct device *piodev;
	const struct device *clk_dev;
	clock_control_subsys_t clk_id;
	struct gpio_dt_spec clk_gpio;
	struct gpio_dt_spec mosi_gpio;
	const struct pinctrl_dev_config *pcfg;
	uint32_t gap_cycles; /* TODO(dma-task): inter-byte PIO delay; wired up with TX-DMA */
	const struct device *dma_dev;
	uint8_t dma_channel;
};

struct spi_pico_pio_data {
	struct spi_context ctx;
	PIO pio;
	size_t sm;
	uint32_t tx_offset;
	struct k_sem done;
};

/* ------------ */
/* spi_mode_0_0 */
/* ------------ */
/*
 * 4-wire, mode-0, WRITE-ONLY program. Two cycles per bit (no IN, no delays):
 * drive MOSI with SCK low, then raise SCK so the panel samples on the rising
 * edge. 2 cycles/bit => max SCK = clk_sys/2 = 62.5 MHz (the ST7789's rated max).
 * No IN instruction, so there is no RX-FIFO/autopush concern at all.
 */
#define SPI_MODE_0_0_WRAP_TARGET 0
#define SPI_MODE_0_0_WRAP        1
#define SPI_MODE_0_0_CYCLES      2

RPI_PICO_PIO_DEFINE_PROGRAM(spi_mode_0_0, SPI_MODE_0_0_WRAP_TARGET, SPI_MODE_0_0_WRAP,
			    /*     .wrap_target */
			    0x6001, /*  0: out    pins, 1         side 0 */
			    0xb042, /*  1: nop                    side 1 */
				    /*     .wrap */
);

static int spi_pico_pio_configure(const struct spi_pico_pio_config *dev_cfg,
				  struct spi_pico_pio_data *data,
				  const struct spi_config *spi_cfg)
{
	const struct gpio_dt_spec *clk = &dev_cfg->clk_gpio;
	const struct gpio_dt_spec *mosi = &dev_cfg->mosi_gpio;
	pio_sm_config sm_config;
	uint32_t clock_freq;
	float clock_div;
	int rc;

	if (spi_context_configured(&data->ctx, spi_cfg)) {
		return 0;
	}

	if (spi_cfg->operation & SPI_OP_MODE_SLAVE) {
		LOG_ERR("Slave mode not supported");
		return -ENOTSUP;
	}

	/* Write-only, mode-0, MSB-first, 8-bit only. */
	if (SPI_MODE_GET(spi_cfg->operation) & (SPI_MODE_CPOL | SPI_MODE_CPHA)) {
		LOG_ERR("Only SPI mode 0 (CPOL=0, CPHA=0) supported");
		return -ENOTSUP;
	}

	if (SPI_MODE_GET(spi_cfg->operation) & SPI_MODE_LOOP) {
		LOG_ERR("Loopback not supported");
		return -ENOTSUP;
	}

	if (spi_cfg->operation & SPI_TRANSFER_LSB) {
		LOG_ERR("Only MSB-first supported");
		return -ENOTSUP;
	}

	if (spi_cfg->operation & SPI_HALF_DUPLEX) {
		LOG_ERR("Half-duplex not supported (write-only 4-wire driver)");
		return -ENOTSUP;
	}

#if defined(CONFIG_SPI_EXTENDED_MODES)
	if (spi_cfg->operation & (SPI_LINES_DUAL | SPI_LINES_QUAD | SPI_LINES_OCTAL)) {
		LOG_ERR("Multi-line modes not supported");
		return -ENOTSUP;
	}
#endif /* CONFIG_SPI_EXTENDED_MODES */

	if (SPI_WORD_SIZE_GET(spi_cfg->operation) != 8) {
		LOG_ERR("Only 8-bit word size supported");
		return -ENOTSUP;
	}

	rc = clock_control_on(dev_cfg->clk_dev, dev_cfg->clk_id);
	if (rc < 0) {
		LOG_ERR("Failed to enable the clock");
		return rc;
	}

	rc = clock_control_get_rate(dev_cfg->clk_dev, dev_cfg->clk_id, &clock_freq);
	if (rc < 0) {
		LOG_ERR("Failed to get clock frequency");
		return rc;
	}

	data->pio = pio_rpi_pico_get_pio(dev_cfg->piodev);

	if (!pio_can_add_program(data->pio, RPI_PICO_PIO_GET_PROGRAM(spi_mode_0_0))) {
		LOG_ERR("cannot add PIO program (instruction memory full)");
		return -EBUSY;
	}

	rc = pio_rpi_pico_allocate_sm(dev_cfg->piodev, &data->sm);
	if (rc < 0) {
		return rc;
	}

	data->tx_offset = pio_add_program(data->pio, RPI_PICO_PIO_GET_PROGRAM(spi_mode_0_0));

	const uint32_t max_freq = clock_freq / SPI_MODE_0_0_CYCLES;
	const uint32_t min_freq = clock_freq / (SPI_MODE_0_0_CYCLES * 65536U);
	if (spi_cfg->frequency > max_freq || spi_cfg->frequency < min_freq) {
		LOG_ERR("SPI frequency %u out of range [%u, %u] (PIO mode_0_0 = clk_sys/4)",
			spi_cfg->frequency, min_freq, max_freq);
		return -EINVAL;
	}

	clock_div = (float)clock_freq / (float)(SPI_MODE_0_0_CYCLES * spi_cfg->frequency);

	sm_config = pio_get_default_sm_config();
	sm_config_set_clkdiv(&sm_config, clock_div);
	/* SCK driven via side-set, MOSI via the OUT pin. */
	sm_config_set_sideset_pins(&sm_config, clk->pin);
	sm_config_set_sideset(&sm_config, 1, false, false);
	sm_config_set_out_pins(&sm_config, mosi->pin, 1);
	sm_config_set_out_shift(&sm_config, false /* MSB-first */, true /* autopull */, 8);
	/* TX-only program: no IN instruction, so the RX FIFO/ISR is never used. */
	sm_config_set_wrap(&sm_config,
			   data->tx_offset + RPI_PICO_PIO_GET_WRAP_TARGET(spi_mode_0_0),
			   data->tx_offset + RPI_PICO_PIO_GET_WRAP(spi_mode_0_0));

	pio_sm_set_pindirs_with_mask(data->pio, data->sm,
				     (BIT(clk->pin) | BIT(mosi->pin)),
				     (BIT(clk->pin) | BIT(mosi->pin)));
	pio_sm_set_pins_with_mask(data->pio, data->sm, 0,
				  BIT(clk->pin) | BIT(mosi->pin));
	pio_gpio_init(data->pio, mosi->pin);
	pio_gpio_init(data->pio, clk->pin);

	pio_sm_init(data->pio, data->sm, data->tx_offset, &sm_config);
	pio_sm_set_enabled(data->pio, data->sm, true);

	data->ctx.config = spi_cfg;

	return 0;
}

/*
 * DMA completion callback (mirrors PL022's callback role): the dma_rpi_pico
 * driver owns the DMA IRQ and dispatches here on TX-channel completion. We
 * only need to wake the transceive() thread.
 */
static void spi_pico_pio_dma_callback(const struct device *dma_dev, void *arg,
				      uint32_t channel, int status)
{
	const struct device *dev = arg;
	struct spi_pico_pio_data *data = dev->data;

	ARG_UNUSED(dma_dev);
	ARG_UNUSED(channel);
	ARG_UNUSED(status);

	k_sem_give(&data->done);
}

/*
 * Transfers up to this many bytes are CPU-written straight into the PIO TX FIFO
 * instead of going through DMA. For small transfers (the display's command/window
 * writes: SWRESET, CASET/RASET, RAMWR, gamma tables, etc.) the DMA setup plus the
 * thread sleep/wake costs far more CPU than just feeding the few bytes, so this
 * keeps the (CPU-sleeping) DMA path reserved for the large pixel blits where it
 * actually frees the CPU. Pixel strips are thousands of bytes, well above this.
 */
#define SPI_PIO_CPU_PUT_THRESHOLD 64

static int spi_pico_pio_transceive(const struct device *dev, const struct spi_config *spi_cfg,
				   const struct spi_buf_set *tx_bufs,
				   const struct spi_buf_set *rx_bufs)
{
	const struct spi_pico_pio_config *dev_cfg = dev->config;
	struct spi_pico_pio_data *data = dev->data;
	int rc;

	spi_context_lock(&data->ctx, false, NULL, NULL, spi_cfg);

	rc = spi_pico_pio_configure(dev_cfg, data, spi_cfg);
	if (rc < 0) {
		goto out;
	}

	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);
	spi_context_cs_control(&data->ctx, true);

	while (spi_context_tx_buf_on(&data->ctx)) {
		const size_t len = spi_context_max_continuous_chunk(&data->ctx);
		int64_t deadline;

		if (len <= SPI_PIO_CPU_PUT_THRESHOLD) {
			/* Small (command/window) transfer: CPU-write the bytes straight
			 * into the PIO TX FIFO. Identical byte semantics to the DMA path
			 * (8-bit writes to txf), but no DMA setup and no thread sleep. */
			const uint8_t *tx = data->ctx.tx_buf;

			for (size_t i = 0; i < len; i++) {
				while (pio_sm_is_tx_fifo_full(data->pio, data->sm)) {
				}
				*(volatile uint8_t *)&data->pio->txf[data->sm] = tx[i];
			}
		} else {
			struct dma_block_config block = {0};
			struct dma_config cfg = {0};

			block.source_address = (uint32_t)data->ctx.tx_buf;
			block.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
			block.dest_address = (uint32_t)&data->pio->txf[data->sm];
			block.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
			block.block_size = len;

			cfg.channel_direction = MEMORY_TO_PERIPHERAL;
			cfg.source_data_size = 1;
			cfg.dest_data_size = 1;
			cfg.source_burst_length = 1;
			cfg.dest_burst_length = 1;
			cfg.block_count = 1;
			cfg.head_block = &block;
			cfg.dma_slot =
				RPI_PICO_DMA_DREQ_TO_SLOT(pio_get_dreq(data->pio, data->sm, true));
			cfg.dma_callback = spi_pico_pio_dma_callback;
			cfg.user_data = (void *)dev;

			rc = dma_config(dev_cfg->dma_dev, dev_cfg->dma_channel, &cfg);
			if (rc < 0) {
				break;
			}

			k_sem_reset(&data->done);
			rc = dma_start(dev_cfg->dma_dev, dev_cfg->dma_channel);
			if (rc < 0) {
				break;
			}

			rc = k_sem_take(&data->done, K_MSEC(1000));
			if (rc != 0) {
				dma_stop(dev_cfg->dma_dev, dev_cfg->dma_channel);
				rc = -EIO;
				break;
			}
		}

		/* Wait for the SM to drain the FIFO and return to program start so
		 * CS deasserts only after the last bit. Bounded so it cannot hang. */
		deadline = k_uptime_get() + 100;
		while (!pio_sm_is_tx_fifo_empty(data->pio, data->sm)) {
			if (k_uptime_get() > deadline) {
				break;
			}
		}
		while (data->pio->sm[data->sm].addr != data->tx_offset) {
			if (k_uptime_get() > deadline) {
				break;
			}
		}

		spi_context_update_tx(&data->ctx, 1, len);
	}

	spi_context_cs_control(&data->ctx, false);
out:
	spi_context_release(&data->ctx, rc);
	return rc;
}

static int spi_pico_pio_release(const struct device *dev, const struct spi_config *spi_cfg)
{
	struct spi_pico_pio_data *data = dev->data;

	ARG_UNUSED(spi_cfg);

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, spi_pico_pio_api) = {
	.transceive = spi_pico_pio_transceive,
	.release = spi_pico_pio_release,
};

static int spi_pico_pio_init(const struct device *dev)
{
	const struct spi_pico_pio_config *dev_cfg = dev->config;
	struct spi_pico_pio_data *data = dev->data;
	int rc;

	if (!device_is_ready(dev_cfg->dma_dev)) {
		LOG_ERR("DMA device not ready");
		return -ENODEV;
	}

	rc = pinctrl_apply_state(dev_cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (rc < 0) {
		LOG_ERR("Failed to apply pinctrl state");
		return rc;
	}

	k_sem_init(&data->done, 0, 1);

	rc = spi_context_cs_configure_all(&data->ctx);
	if (rc < 0) {
		LOG_ERR("Failed to configure CS pins: %d", rc);
		return rc;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

#define SPI_PICO_PIO_DMA_INIT(inst)                                                                 \
	PINCTRL_DT_INST_DEFINE(inst);                                                               \
	static struct spi_pico_pio_config spi_pico_pio_config_##inst = {                            \
		.piodev = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                      \
		.clk_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(inst)),                                \
		.clk_id = (clock_control_subsys_t)DT_INST_PHA_BY_IDX(inst, clocks, 0, clk_id),      \
		.clk_gpio = GPIO_DT_SPEC_INST_GET(inst, clk_gpios),                                 \
		.mosi_gpio = GPIO_DT_SPEC_INST_GET(inst, mosi_gpios),                               \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                                       \
		.gap_cycles = DT_INST_PROP(inst, gap_cycles),                                       \
		.dma_dev = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(inst, tx)),                      \
		.dma_channel = DT_INST_DMAS_CELL_BY_NAME(inst, tx, channel),                        \
	};                                                                                          \
	static struct spi_pico_pio_data spi_pico_pio_data_##inst = {                                \
		SPI_CONTEXT_INIT_LOCK(spi_pico_pio_data_##inst, ctx),                               \
		SPI_CONTEXT_INIT_SYNC(spi_pico_pio_data_##inst, ctx),                               \
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(inst), ctx)                             \
	};                                                                                          \
	SPI_DEVICE_DT_INST_DEFINE(inst, spi_pico_pio_init, NULL, &spi_pico_pio_data_##inst,         \
				  &spi_pico_pio_config_##inst, POST_KERNEL,                         \
				  CONFIG_SPI_INIT_PRIORITY, &spi_pico_pio_api);                     \
	BUILD_ASSERT(DT_INST_NODE_HAS_PROP(inst, clk_gpios), "Missing clk-gpios");                  \
	BUILD_ASSERT(DT_INST_NODE_HAS_PROP(inst, mosi_gpios), "Missing mosi-gpios");

DT_INST_FOREACH_STATUS_OKAY(SPI_PICO_PIO_DMA_INIT)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
