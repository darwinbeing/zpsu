# Out-of-tree patches

These patch the Zephyr west workspace, which `west update` re-fetches fresh on every
build — so the patches must be re-applied each time. CI
(`.github/workflows/firmware.yml`) applies them automatically right after
`west update` / `west blobs fetch`. For local builds, apply them by hand.

| Patch | Target tree | Fixes |
|-------|-------------|-------|
| `st7789v_clear_gram.patch` | `zephyrproject/zephyr` | ST7789 power-up GRAM garbage on the Pico Display Pack 2 |
| `rp2040_flash_ramfunc.patch` | `zephyrproject/modules/hal/rpi_pico` | Zephyr #68728 (root cause): `hardware_flash/flash.c` helpers `flash_wait_ready`/`flash_enable_write`/`flash_put_cmd_addr` are `static inline` without the RAM attribute → when not inlined they sit in XIP flash and are called by `flash_write_partial_internal` while XIP is off → instruction-fetch HardFault → double fault → MCU lockup on the first NVS write. Forces them into RAM |
| `cyw43439_ap_set_channel.patch` | `zephyrproject/modules/hal/infineon` | CYW43439 SoftAP `WHD_WLAN_BADCHAN` (2020): the WHD 3.3.3 firmware rejects the AP `chanspec` iovar, so set the channel via `WLC_SET_CHANNEL` instead (as the Pico SDK cyw43-driver does) |

## Apply locally

Run from this repo root (so `$PWD` resolves to it). Note the **different target
trees** — st7789 patches the zephyr tree, the other two patch HAL modules:

```sh
git -C ~/zephyrproject/zephyr                apply "$PWD/patches/st7789v_clear_gram.patch"
git -C ~/zephyrproject/modules/hal/rpi_pico  apply "$PWD/patches/rp2040_flash_ramfunc.patch"
git -C ~/zephyrproject/modules/hal/infineon  apply "$PWD/patches/cyw43439_ap_set_channel.patch"
```
