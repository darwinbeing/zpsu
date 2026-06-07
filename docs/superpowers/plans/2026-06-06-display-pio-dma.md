# Display PIO + DMA SPI driver — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a switchable, write-only **PIO + TX-DMA** SPI controller in-project so the ST7789 can be driven fast *and* with the CPU freed during flush, sidestepping the Zephyr PL022 DMA RX limitation.

**Architecture:** A new out-of-tree Zephyr SPI-controller driver (child of `&pio0`) implements `spi_driver_api` using pico-sdk PIO + a single TX DMA channel (DMA completion IRQ → semaphore → calling thread sleeps). MIPI-DBI/ST7789V/LVGL bind to it unchanged via `spi-dev`. Everything is gated by `CONFIG_DISPLAY_SPI_PIO_DMA` (default off); the committed PL022+DMA@15.625 build is untouched.

**Tech Stack:** Zephyr 4.2, RP2040, pico-sdk (`hal_rpi_pico`: `hardware/pio.h`, `hardware/dma.h`), `pio_rpi_pico` helper, `spi_context`, devicetree out-of-tree binding via `DTS_ROOT`.

**Testing note:** No host test harness exists for this driver. "Verify" = build success and on-hardware behavior read from the `FLUSH BENCH` block (`CONFIG_APP_FLUSH_BENCH=y`). Reference implementation to mirror for boilerplate: `$ZEPHYR_BASE/drivers/spi/spi_rpi_pico_pio.c` (4-wire `spi_mode_0_0` program, `spi_context` usage, `SPI_DEVICE_DT_INST_DEFINE` registration).

**Build env (every build step):**
```bash
export ZEPHYR_BASE=/Users/litao/Developer/zephyrproject/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
export GNUARMEMB_TOOLCHAIN_PATH=/Users/litao/Developer/zephyrproject/arm-gnu-toolchain-13.2.Rel1-darwin-x86_64-arm-none-eabi
```

---

## File Structure

- Create `dts/bindings/spi/zpsu,pico-spi-pio-dma.yaml` — DT binding for the new controller.
- Create `Kconfig` symbol `DISPLAY_SPI_PIO_DMA` (in project `Kconfig`).
- Create `src/display/spi_pio_dma.c` — the driver (configure + transceive + DMA IRQ).
- Modify `CMakeLists.txt` — add `DTS_ROOT`, compile the driver conditionally.
- Create `boards/rpi_pico/pico_display_pack2_pio.dtsi` — PIO node + pinctrl + spi-dev repoint, `#include`d from the overlay when the Kconfig is set.
- Modify `boards/rpi_pico/pico_display_pack2.overlay` — conditionally include the PIO dtsi and disable `&spi0`.

---

## Task 1: Kconfig switch

**Files:**
- Modify: `Kconfig`

- [ ] **Step 1: Add the option under the ZSPSUMon menu**

In `Kconfig`, inside `menu "ZSPSUMon"` (after `APP_FLUSH_BENCH`), add:

```kconfig
    config DISPLAY_SPI_PIO_DMA
        bool "Drive the display SPI via PIO + TX-DMA (experimental)"
        default n
        select PINCTRL
        help
          Replace the PL022 hardware-SPI path with an in-project PIO state
          machine fed by a single TX DMA channel. Frees the CPU during flush
          and exposes a tunable clock/gap. When off, the display uses the
          default PL022 + DMA path.
```

- [ ] **Step 2: Build the default (option off) to confirm no regression**

Run:
```bash
west build -p always -b rpi_pico -s . -d build_lcd2 -- -DCONFIG_PICO_DISPLAY_PACK2=y
```
Expected: build succeeds; `grep CONFIG_DISPLAY_SPI_PIO_DMA build_lcd2/zephyr/.config` → `is not set`.

- [ ] **Step 3: Commit**

```bash
git add Kconfig
git commit -m "feat(display): add CONFIG_DISPLAY_SPI_PIO_DMA switch (default off)"
```

---

## Task 2: DT binding + DTS_ROOT

**Files:**
- Create: `dts/bindings/spi/zpsu,pico-spi-pio-dma.yaml`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the binding**

Create `dts/bindings/spi/zpsu,pico-spi-pio-dma.yaml`:

```yaml
# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

description: ST7789 SPI via RP2040 PIO + TX DMA (write-only)

compatible: "zpsu,pico-spi-pio-dma"

include: ["spi-controller.yaml", "raspberrypi,pico-pio-device.yaml", "reset-device.yaml"]

properties:
  clk-gpios:
    type: phandle-array
    required: true
    description: Output pin for SPI clock (SCK).

  mosi-gpios:
    type: phandle-array
    required: true
    description: Output pin for MOSI.

  clocks:
    required: true

  gap-cycles:
    type: int
    default: 0
    description: |
      Extra PIO delay cycles inserted per byte to give the panel recovery
      time at high clock. 0 = fully gapless stream.
```

- [ ] **Step 2: Point DTS_ROOT at the project so the binding is found**

In `CMakeLists.txt`, immediately before `find_package(Zephyr ...)`, add:

```cmake
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
```

- [ ] **Step 3: Build (option still off) to confirm the binding parses**

Run the Task 1 Step 2 build command.
Expected: build succeeds (binding is only parsed when a node uses it; this just verifies DTS_ROOT didn't break anything).

- [ ] **Step 4: Commit**

```bash
git add dts/bindings/spi/zpsu,pico-spi-pio-dma.yaml CMakeLists.txt
git commit -m "feat(display): add zpsu,pico-spi-pio-dma binding + DTS_ROOT"
```

---

## Task 3: Driver — configure() only (PIO setup, registers, builds)

**Files:**
- Create: `src/display/spi_pio_dma.c`
- Modify: `CMakeLists.txt`

Mirror `$ZEPHYR_BASE/drivers/spi/spi_rpi_pico_pio.c` for: includes, `struct spi_pico_pio_config/data`, the `spi_mode_0_0` 4-wire program (`RPI_PICO_PIO_DEFINE_PROGRAM(spi_mode_0_0, 0, 1, 0x6101, 0x5101)`), `pio_rpi_pico_get_pio()` / `pio_rpi_pico_allocate_sm()`, `PINCTRL_DT_INST_DEFINE`, and `SPI_DEVICE_DT_INST_DEFINE`.

- [ ] **Step 1: Write the driver scaffold with configure() and a stub transceive()**

Create `src/display/spi_pio_dma.c` with:
- `#define DT_DRV_COMPAT zpsu_pico_spi_pio_dma`
- includes: `<zephyr/drivers/spi.h>`, `"spi_context.h"` (copy of the path used by the reference: `#include <zephyr/drivers/spi/rtio.h>` is NOT needed; use `spi_context.h` from `drivers/spi`), `<zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>`, `<hardware/pio.h>`, `<hardware/dma.h>`, `<zephyr/drivers/pinctrl.h>`.
- config struct: `const struct device *piodev; const struct device *clk_dev; clock_control_subsys_t clk_id; struct gpio_dt_spec clk_gpio, mosi_gpio; const struct pinctrl_dev_config *pcfg; uint32_t gap_cycles;`
- data struct: `struct spi_context ctx; PIO pio; size_t sm; uint tx_offset; int dma_chan; struct k_sem done;`
- `spi_pico_pio_configure()`: copy the reference's clock-range check + `pio_add_program` + `pio_sm_config` (sideset=clk pin, out=mosi pin, autopull threshold 8, `sm_config_set_clkdiv` from `clk_sys/(SPI_MODE_0_0_CYCLES*freq)`), then `pio_sm_init` + `pio_sm_set_enabled`. Honor `gap_cycles` by selecting a delay on the `out` instruction (see Task 8; for now pass program as-is, gap=0).
- `transceive()` stub: `return -ENOSYS;` (filled in Task 5).
- `spi_pico_pio_init()`: `pinctrl_apply_state(DEFAULT)`, `k_sem_init(&data->done,0,1)`, `spi_context_unlock_unconditionally(&data->ctx)`.
- registration macro at bottom:

```c
#define SPI_PIO_DMA_INIT(inst)                                                  \
    PINCTRL_DT_INST_DEFINE(inst);                                               \
    static const struct spi_pico_pio_config cfg_##inst = {                      \
        .piodev = DEVICE_DT_GET(DT_INST_PARENT(inst)),                          \
        .clk_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(inst)),                    \
        .clk_id = (clock_control_subsys_t)DT_INST_PHA_BY_IDX(inst, clocks, 0, clk_id), \
        .clk_gpio = GPIO_DT_SPEC_INST_GET(inst, clk_gpios),                     \
        .mosi_gpio = GPIO_DT_SPEC_INST_GET(inst, mosi_gpios),                   \
        .pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                           \
        .gap_cycles = DT_INST_PROP(inst, gap_cycles),                           \
    };                                                                          \
    static struct spi_pico_pio_data data_##inst = {                            \
        SPI_CONTEXT_INIT_LOCK(data_##inst, ctx),                                \
        SPI_CONTEXT_INIT_SYNC(data_##inst, ctx),                                \
    };                                                                          \
    SPI_DEVICE_DT_INST_DEFINE(inst, spi_pico_pio_init, NULL,                    \
                  &data_##inst, &cfg_##inst, POST_KERNEL,                       \
                  CONFIG_SPI_INIT_PRIORITY, &spi_pico_pio_api);
DT_INST_FOREACH_STATUS_OKAY(SPI_PIO_DMA_INIT)
```

(Match exact field/clk_id access to how `spi_rpi_pico_pio.c` reads them; copy that file's macro verbatim and adjust names.)

- [ ] **Step 2: Compile the driver conditionally**

In `CMakeLists.txt`, after the `target_sources(app PRIVATE ${app_SRCS})` line add:

```cmake
target_sources_ifdef(CONFIG_DISPLAY_SPI_PIO_DMA app PRIVATE
  ${PROJECT_SOURCE_DIR}/src/display/spi_pio_dma.c)
```

- [ ] **Step 3: Build with the option ON but no node yet — expect it to compile out**

Run:
```bash
west build -p always -b rpi_pico -s . -d build_pio -- -DCONFIG_PICO_DISPLAY_PACK2=y -DCONFIG_DISPLAY_SPI_PIO_DMA=y
```
Expected: build succeeds. Because no DT node has `compatible = "zpsu,pico-spi-pio-dma"` yet, `DT_INST_FOREACH_STATUS_OKAY` expands to nothing and the driver body is dead-stripped — this proves the file compiles.

- [ ] **Step 4: Commit**

```bash
git add src/display/spi_pio_dma.c CMakeLists.txt
git commit -m "feat(display): PIO+DMA SPI driver scaffold (configure, registration)"
```

---

## Task 4: Board integration dtsi + overlay (node appears, driver instantiates)

**Files:**
- Create: `boards/rpi_pico/pico_display_pack2_pio.dtsi`
- Modify: `boards/rpi_pico/pico_display_pack2.overlay`

- [ ] **Step 1: Write the PIO board fragment**

Create `boards/rpi_pico/pico_display_pack2_pio.dtsi`:

```dts
/* PIO + TX-DMA SPI path for the display. Included only when
 * CONFIG_DISPLAY_SPI_PIO_DMA is set. Frees GP18/GP19 from PL022 and gives
 * them to a PIO state machine; repoints MIPI-DBI at the PIO controller. */
&spi0 {
	status = "disabled";
};

&pinctrl {
	pio0_spi_default: pio0_spi_default {
		group1 {
			pinmux = <PIO0_P18>, <PIO0_P19>;  /* SCK, MOSI */
			drive-strength = <12>;
			slew-rate = <1>;
		};
	};
};

&pio0 {
	status = "okay";

	pio_spi0: pio_spi0 {
		compatible = "zpsu,pico-spi-pio-dma";
		reg = <0>;
		#address-cells = <1>;
		#size-cells = <0>;
		clocks = <&clocks RPI_PICO_CLKID_CLK_SYS>;
		clk-gpios = <&gpio0 18 GPIO_ACTIVE_HIGH>;
		mosi-gpios = <&gpio0 19 GPIO_ACTIVE_HIGH>;
		pinctrl-0 = <&pio0_spi_default>;
		pinctrl-names = "default";
		gap-cycles = <0>;
		status = "okay";
	};
};

&mipi_dbi {
	spi-dev = <&pio_spi0>;
};
```

- [ ] **Step 2: Conditionally include it from the overlay**

In `boards/rpi_pico/pico_display_pack2.overlay`, at the very end of the file add:

```dts
#if defined(CONFIG_DISPLAY_SPI_PIO_DMA)
#include "pico_display_pack2_pio.dtsi"
#endif
```

(The overlay already `#include`s the rp2040 pinctrl header that defines `PIO0_P18`/`PIO0_P19`.)

- [ ] **Step 3: Build with the option ON; the driver now instantiates**

Run the Task 3 Step 3 build command (`-d build_pio ... -DCONFIG_DISPLAY_SPI_PIO_DMA=y`).
Expected: build succeeds. Verify wiring:
```bash
grep -n "pio_spi0\|zpsu,pico-spi-pio-dma\|spi-dev" build_pio/zephyr/zephyr.dts | head
grep -n "spi@4003c000" -A2 build_pio/zephyr/zephyr.dts   # spi0 should be status=disabled
```
Expected: `pio_spi0` node present with our compatible; `mipi_dbi` `spi-dev` → `pio_spi0`; `spi0` disabled.

- [ ] **Step 4: Commit**

```bash
git add boards/rpi_pico/pico_display_pack2_pio.dtsi boards/rpi_pico/pico_display_pack2.overlay
git commit -m "feat(display): board DT for PIO+DMA path (disable spi0, repoint mipi-dbi)"
```

---

## Task 5: transceive() — TX DMA + completion IRQ (CPU-free)

**Files:**
- Modify: `src/display/spi_pio_dma.c`

- [ ] **Step 1: Add a shared DMA completion ISR + per-instance sem**

Add a single DMA IRQ handler (rp2040 DMA_IRQ_0 = IRQ 11). Use a file-scope pointer to the active instance's data (only one display controller exists), set before each transfer:

```c
static struct spi_pico_pio_data *active_dma_data;

static void spi_pio_dma_isr(const void *arg)
{
    ARG_UNUSED(arg);
    struct spi_pico_pio_data *d = active_dma_data;
    if (d && dma_channel_get_irq0_status(d->dma_chan)) {
        dma_channel_acknowledge_irq0(d->dma_chan);
        k_sem_give(&d->done);
    }
}
```

Connect it once in `spi_pico_pio_init()`:

```c
IRQ_CONNECT(DT_IRQN(DT_INST(0, raspberrypi_pico_dma)), 1, spi_pio_dma_isr, NULL, 0);
irq_enable(DT_IRQN(DT_INST(0, raspberrypi_pico_dma)));
```

If `DT_INST(0, raspberrypi_pico_dma)` is unavailable (node disabled), use the literal `DMA_IRQ_0` number: `IRQ_CONNECT(11, 1, spi_pio_dma_isr, NULL, 0); irq_enable(11);`.

- [ ] **Step 2: Claim a DMA channel in init**

In `spi_pico_pio_init()`, after PIO setup add:
```c
data->dma_chan = dma_claim_unused_channel(true);
```

- [ ] **Step 3: Implement transceive() (write-only, single TX DMA per chunk)**

Replace the stub with:

```c
static int spi_pico_pio_transceive(const struct device *dev,
                                   const struct spi_config *spi_cfg,
                                   const struct spi_buf_set *tx_bufs,
                                   const struct spi_buf_set *rx_bufs)
{
    struct spi_pico_pio_data *data = dev->data;
    const struct spi_pico_pio_config *cfg = dev->config;
    int rc;

    spi_context_lock(&data->ctx, false, NULL, NULL, spi_cfg);
    rc = spi_pico_pio_configure(dev, spi_cfg);   /* clkdiv/program if changed */
    if (rc < 0) { goto out; }

    spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);
    spi_context_cs_control(&data->ctx, true);

    active_dma_data = data;
    while (spi_context_tx_buf_on(&data->ctx)) {
        const size_t len = spi_context_max_continuous_chunk(&data->ctx);
        dma_channel_config c = dma_channel_get_default_config(data->dma_chan);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(data->pio, data->sm, true));
        channel_config_set_irq_quiet(&c, false);
        k_sem_reset(&data->done);
        dma_channel_set_irq0_enabled(data->dma_chan, true);
        dma_channel_configure(data->dma_chan, &c,
                              &data->pio->txf[data->sm],   /* dst: PIO TX FIFO */
                              data->ctx.tx_buf,            /* src */
                              len, true);                  /* trigger */
        rc = k_sem_take(&data->done, K_MSEC(1000));
        dma_channel_set_irq0_enabled(data->dma_chan, false);
        if (rc != 0) { rc = -EIO; break; }
        /* wait for the PIO FIFO+SM to drain so CS deasserts after last bit */
        while (!pio_sm_is_tx_fifo_empty(data->pio, data->sm)) { }
        while (data->pio->sm[data->sm].addr != data->tx_offset) { }
        spi_context_update_tx(&data->ctx, 1, len);
    }
    active_dma_data = NULL;
    spi_context_cs_control(&data->ctx, false);
out:
    spi_context_release(&data->ctx, rc);
    return rc;
}

static const struct spi_driver_api spi_pico_pio_api = {
    .transceive = spi_pico_pio_transceive,
    .release = spi_pico_pio_release,   /* simple: spi_context_unlock_unconditionally */
};
```

(`spi_pico_pio_release()`: `spi_context_unlock_unconditionally(&((struct spi_pico_pio_data *)dev->data)->ctx); return 0;`)

- [ ] **Step 4: Build with the option ON**

Run the Task 3 Step 3 build command.
Expected: build succeeds; `grep -E "DISPLAY_SPI_PIO_DMA" build_pio/zephyr/.config` → `=y`.

- [ ] **Step 5: Commit**

```bash
git add src/display/spi_pio_dma.c
git commit -m "feat(display): PIO+DMA transceive with completion IRQ (CPU-free)"
```

---

## Task 6: On-device bring-up at a conservative clock

**Files:** none (verification task). Uses `mipi-max-frequency` already at 15.625 in the committed dtsi.

- [ ] **Step 1: Build with PIO + benchmark at the known-safe clock**

Run:
```bash
west build -p always -b rpi_pico -s . -d build_pio -- \
  -DCONFIG_PICO_DISPLAY_PACK2=y -DCONFIG_DISPLAY_SPI_PIO_DMA=y -DCONFIG_APP_FLUSH_BENCH=y
```
Expected: build succeeds; UF2 at `build_pio/zephyr/zephyr.uf2`.

- [ ] **Step 2: Flash and observe (manual)**

Flash `build_pio/zephyr/zephyr.uf2`. Confirm: watchface renders with no garble, and the `FLUSH BENCH` block prints. Record full-frame ms and **CPU free %** — at 15.625 it should now show CPU free ≈100% (proving the PIO+TX-DMA architecture frees the CPU, unlike polling).

- [ ] **Step 3: Decision gate**

If garbled even at 15.625 → debug the driver (PIO program / DMA dst address / CS drain) before proceeding. If clean + CPU-free → proceed to clock sweep.

---

## Task 7: Clock sweep + gap tuning to find max stable

**Files:**
- Modify (iteratively): `boards/common/pico_display_pack2_lcd.dtsi` (`mipi-max-frequency`), `boards/rpi_pico/pico_display_pack2_pio.dtsi` (`gap-cycles`).

- [ ] **Step 1: Raise the clock and rebuild/flash, stepping through the rates**

For each candidate request value, set `mipi-max-frequency` and rebuild (Task 6 Step 1), flash, check garble + bench:
- `<62500000>` → 62.5 MHz
- `<32000000>` → 31.25 MHz
- `<24000000>` → 20.83 MHz (must beat this to justify the work)

Record the highest clean rate.

- [ ] **Step 2: If the target clock garbles, add recovery gaps**

Set `gap-cycles = <1>` (then `<2>`) in `pico_display_pack2_pio.dtsi`, rebuild/flash, retest the failing clock. Implement `gap-cycles` in the driver by adding delay to the `out` instruction's side-set delay field (max 0..2 with one side-set bit; for larger gaps add a `nop side 1 [n]` instruction to the program variant). Stop at the fastest stable (clock, gap).

- [ ] **Step 3: Record the result in the spec**

Append the measured (clock, gap, full-frame ms, CPU-free %) to `docs/superpowers/specs/2026-06-06-display-pio-dma-design.md` under a new "Results" heading.

- [ ] **Step 4: Commit the chosen settings**

```bash
git add boards/common/pico_display_pack2_lcd.dtsi boards/rpi_pico/pico_display_pack2_pio.dtsi docs/superpowers/specs/2026-06-06-display-pio-dma-design.md
git commit -m "perf(display): set PIO+DMA to fastest stable clock/gap on this HAT"
```

---

## Task 8: Finalize

- [ ] **Step 1: Confirm the default (option off) still builds clean**

```bash
west build -p always -b rpi_pico -s . -d build_lcd2 -- -DCONFIG_PICO_DISPLAY_PACK2=y
grep CONFIG_DISPLAY_SPI_PIO_DMA build_lcd2/zephyr/.config   # is not set
```

- [ ] **Step 2: Update memory**

Add/adjust `[[display-spi-dma]]` memory with the PIO+DMA result and how to enable it (`-DCONFIG_DISPLAY_SPI_PIO_DMA=y`).

- [ ] **Step 3: Decide merge vs keep-on-branch** (ask the user). If the PIO path wins decisively, optionally flip the overlay default; otherwise keep it opt-in.

---

## Self-review notes

- Spec coverage: binding (T2), driver configure (T3), TX-DMA + IRQ/CPU-free (T5), integration/switchability (T4), verification/benchmark (T6), clock+gap sweep + fallback (T7), default-off safety (T1/T8) — all covered.
- Known risk carried from spec: exact pico-sdk DMA symbol/IRQ macro names (`dma_channel_get_irq0_status`, `dma_channel_acknowledge_irq0`, `&pio->txf[sm]`) must be confirmed against `hal_rpi_pico` headers during T5; if a name differs, use the equivalent from `hardware/dma.h`.
- CS drain (T5 Step 3) mirrors the reference's `spi_pico_pio_sm_complete()` check so CS deasserts only after the last bit is shifted.
