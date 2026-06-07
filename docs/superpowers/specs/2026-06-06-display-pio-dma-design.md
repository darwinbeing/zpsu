# Display PIO + DMA SPI driver — design

- Date: 2026-06-06
- Status: approved (design); implementation pending
- Branch: `feat/display-pio-dma`

## Context

The ST7789 (Pico Display Pack 2.0, 320×240 RGB565) is driven through
LVGL → ST7789V → MIPI-DBI-SPI → an SPI controller. This session walked the
bottom transport through several options and measured each on the real HAT:

| Path | Max stable SCK | Full-frame | CPU during flush |
|------|----------------|-----------|------------------|
| PL022 polling (original) | ≥31.25 MHz | ~78 ms | 100% busy |
| PL022 + DMA, default pins | 15.625 MHz | ~108 ms | ~free |
| PL022 + DMA, 12mA/fast-slew pins | 20.83 MHz | ~80 ms | ~free |
| Pimoroni vendor (SPI) | 62.5 MHz | ~30 ms | 100% busy |

Key findings:
- Strong pin drive/slew raised the DMA ceiling 15.625 → 20.83, then it is
  exhausted (12mA/fast is the max). 31.25 still garbles under DMA.
- The Zephyr **PL022 DMA path garbles *lower* (20.83) than its own polling
  (≥31.25)** — so the limiter is the PL022 DMA implementation, not the panel
  or pins. The PL022 DMA path uses TX **and** RX channels and gates completion
  on the RX count; the RX path is the suspected weak link at high clock.
- Pimoroni reaches 62.5 MHz with **high-clock polling** (`spi_write_blocking`,
  single CS-held continuous transfer), not DMA and not PIO. Their PIO code is
  only for **parallel** displays. So neither polling nor PL022-DMA achieves
  "fast AND CPU-free"; the vendor chose "fast but CPU-busy".

## Goal

Achieve **both** high throughput **and** CPU freed during flush — the one
combination no path above delivers — by building a custom **write-only
PIO + TX-DMA** SPI controller that sidesteps the PL022 DMA RX limitation and
exposes a tunable waveform.

Success criteria (measured with `CONFIG_APP_FLUSH_BENCH`):
- Stable (no garble) at a clock **higher than 20.83 MHz**, ideally up to 62.5.
- CPU free during flush **≈100%** (DMA-driven, calling thread sleeps).
- Worst acceptable outcome: stable at ≥20.83 MHz with CPU free — still better
  than the committed PL022+DMA@15.625 baseline.

## Non-goals

- Replacing the LVGL / ST7789V / MIPI-DBI layers (they stay unchanged).
- Parallel (8080) interface — the HAT is SPI-wired (SCK=GP18, MOSI=GP19).
- RX / full-duplex — the display is write-only.
- Patching the Zephyr tree — the driver lives in this project (out-of-tree).

## Approach

A new out-of-tree Zephyr SPI-controller driver in the project, referencing
Zephyr's `spi_rpi_pico_pio.c` (for the `spi_context` / SPI-API skeleton and the
4-wire PIO program) and the pico-sdk PIO+DMA idiom. It implements the standard
`spi_driver_api`, so MIPI-DBI/ST7789/LVGL bind to it unchanged via `spi-dev`.

### Components

1. **DT binding** — `dts/bindings/spi/zpsu,pico-spi-pio-dma.yaml`
   - compatible `zpsu,pico-spi-pio-dma`; includes `spi-controller.yaml`,
     `raspberrypi,pico-pio-device.yaml`, `reset-device.yaml`.
   - props: `clk-gpios` (SCK, required), `mosi-gpios` (required), `clocks`,
     optional `gap-cycles` (tunable inter-byte delay, default 0).

2. **Driver** — `src/display/spi_pio_dma.c` (compiled via app `CMakeLists.txt`)
   - `select PICOSDK_USE_PIO`, `PICOSDK_USE_DMA`, `PICOSDK_USE_CLAIM`, `PINCTRL`.
   - `configure()`: claim PIO SM, add program, set clkdiv from the requested
     SPI frequency (`spi_config->frequency`, derived from the panel's
     `mipi-max-frequency`): `clkdiv = clk_sys / (cycles * freq)`; sideset=SCK,
     out=MOSI.
   - `transceive()`: write-only; assert CS via `spi_context`, kick TX DMA,
     sleep on a semaphore released by the DMA completion IRQ, deassert CS.

3. **PIO program** — minimal TX program: shift one bit out on MOSI with SCK on
   side-set; clock rate via clkdiv; an assembled variant adds `gap-cycles` of
   delay per byte (autopull threshold 8) to mimic polling's recovery gaps if a
   fully gapless stream garbles at the target clock.

4. **DMA** — single **TX-only** channel (pico-sdk `dma_channel_configure`),
   `dreq = pio_get_dreq(pio, sm, true)`, 8-bit transfers, read = `tx_buf`,
   write = PIO TX FIFO. Completion IRQ → `k_sem_give`. **No RX channel.**

5. **Integration / switchability** (board overlay, behind Kconfig):
   - `&spi0 { status = "disabled"; }` to free GP18/GP19 from PL022.
   - `&pio0 { status = "okay"; pio_spi0: ... }` new controller node + pinctrl
     mapping GP18→SCK, GP19→MOSI.
   - `&mipi_dbi { spi-dev = <&pio_spi0>; }`.
   - All gated by `CONFIG_DISPLAY_SPI_PIO_DMA` (default n) so the committed
     PL022+DMA@15.625 remains the default, safe build.

### Data flow

LVGL flush → ST7789V → MIPI-DBI `spi_write` → our driver `transceive()` →
TX DMA streams `tx_buf` into the PIO TX FIFO → PIO SM shifts bits on
MOSI/SCK → DMA IRQ wakes the flush thread. CPU is free during the transfer;
DC/CS are GPIOs owned by MIPI-DBI, unchanged.

### Error handling

- `configure()` fails cleanly if PIO SM / DMA channel claim fails or the
  requested frequency is out of the achievable range (clk_sys/cycles bounds).
- `transceive()` uses a completion timeout on the semaphore; on timeout it
  stops the DMA, releases CS, returns `-EIO`.
- Out-of-tree driver is isolated behind the Kconfig switch; if it fails to
  build or run, the default PL022 path is unaffected.

## Testing / verification

1. Build with `CONFIG_DISPLAY_SPI_PIO_DMA=y` + `CONFIG_APP_FLUSH_BENCH=y`.
2. Flash; confirm the watchface renders with no garble.
3. Read the `FLUSH BENCH` block: target full-frame ms and CPU-free %.
4. Clock sweep: start at 62.5 MHz; if garble, step down (31.25 → 20.83) and/or
   raise `gap-cycles` to find the fastest stable (clock, gap) combo.
5. Compare against the recorded baselines (108 ms / 80 ms, CPU free).

## Risks & fallback

- **SI at high continuous clock**: a gapless PIO stream may garble like the
  PL022 DMA did. Mitigation: the PIO `gap-cycles` knob inserts tunable recovery
  delays (the lever neither PL022 nor polling expose). If even tuned it cannot
  beat ~20.83, the experiment concludes "no win" and we keep the committed
  PL022+DMA@15.625 (or adopt the validated 20.83 + strong-pins PL022 config).
- **pico-sdk DMA IRQ wiring under Zephyr**: needs an ISR connected without
  clashing with the Zephyr `dma_rpi_pico` driver (which we do not use here).
- **PIO/DMA resource claims**: no other PIO/DMA users in this project today.

## Open questions (resolve during implementation)

- Exact `PICOSDK_USE_DMA` Kconfig symbol and IRQ registration pattern.
- Whether MIPI-DBI's per-chunk CS toggling needs `SPI_HOLD_ON_CS` for a clean
  single continuous transfer at high clock.
