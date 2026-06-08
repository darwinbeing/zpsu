# Out-of-tree patches

These patch the Zephyr west workspace, which `west update` re-fetches fresh on every
build — so the patches must be re-applied each time. CI
(`.github/workflows/firmware.yml`) applies them automatically right after
`west update` / `west blobs fetch`. For local builds, apply them by hand.

| Patch | Target tree | Fixes |
|-------|-------------|-------|
| `st7789v_clear_gram.patch` | `zephyrproject/zephyr` | ST7789 power-up GRAM garbage on the Pico Display Pack 2 |
| `rp2040_flash_partial_write.patch` | `zephyrproject/zephyr` | RP2040 sub-page flash write infinite-loop (Zephyr #68728) that freezes the MCU on the first NVS credential write |
| `cyw43439_ap_set_channel.patch` | `zephyrproject/modules/hal/infineon` | CYW43439 SoftAP `WHD_WLAN_BADCHAN` (2020): the WHD 3.3.3 firmware rejects the AP `chanspec` iovar, so set the channel via `WLC_SET_CHANNEL` instead (as the Pico SDK cyw43-driver does) |

## Apply locally

Run from this repo root (so `$PWD` resolves to it). Note the **different target
trees** — the first two patch the zephyr tree, the third patches the hal_infineon
module:

```sh
git -C ~/zephyrproject/zephyr               apply "$PWD/patches/st7789v_clear_gram.patch"
git -C ~/zephyrproject/zephyr               apply "$PWD/patches/rp2040_flash_partial_write.patch"
git -C ~/zephyrproject/modules/hal/infineon apply "$PWD/patches/cyw43439_ap_set_channel.patch"
```
