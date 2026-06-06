# PSU Boot Splash Screen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a self-contained C++ boot splash (`ui::Splash`) — a blue ring fills under "PSU MONITOR", then the overlay dissolves to reveal the watchface (~1.5s).

**Architecture:** A stateless `ui::Splash` class with a static `Show()` that builds a full-screen opaque overlay on `lv_layer_top()`, plays a ring-fill animation then an opacity fade, and deletes itself in the fade's completion callback. Hooked from `ui::Watchface::Show()`. Pure vector; touches no generated UI.

**Tech Stack:** Zephyr v4.2.1, LVGL 9.3.0 (arc + label + anim, all already enabled in `prj.conf`), C++20, board `rpi_pico`, Ninja build in `build_lcd2`.

**No automated test framework (bare-metal embedded).** Verification is build-based (compile + link + `zephyr.uf2`) plus a hardware visual check.

**Standard build command:**
```bash
cd /Users/litao/Developer/zpsu_mon && \
  export PATH="/Users/litao/anaconda3/bin:$PATH" && \
  export ZEPHYR_BASE=/Users/litao/Developer/zephyrproject/zephyr && \
  ninja -C build_lcd2
```
Timeout 600000 ms. Editing `CMakeLists.txt` triggers a Ninja reconfigure; if the new file isn't picked up, run `cmake -B build_lcd2 -S . 2>/dev/null` then re-run ninja.

**Reference:** the un-compiled `src/ui/watchface_anim.c::watchface_splash_show` uses the exact LVGL 9.x calls this plan mirrors (arc value animation via exec callback, opacity fade, `completed_cb`, async delete). Kconfig already has `CONFIG_LV_USE_ARC=y`, `CONFIG_LV_USE_LABEL=y`, `CONFIG_LV_FONT_MONTSERRAT_28=y` — no `prj.conf` change needed.

---

## Task 1: Create the `ui::Splash` module, wire it in, build

**Files:**
- Create: `src/ui/splash.hpp`, `src/ui/splash.cpp`
- Modify: `src/ui/watchface.cpp` (call `Splash::Show()` from `Watchface::Show()`)
- Modify: `CMakeLists.txt` (add `splash.cpp` to `app_SRCS`)

- [ ] **Step 1: Confirm the LVGL deps are enabled (no change expected)**

```bash
cd /Users/litao/Developer/zpsu_mon
grep -nE "CONFIG_LV_USE_ARC=y|CONFIG_LV_USE_LABEL=y|CONFIG_LV_FONT_MONTSERRAT_28=y" prj.conf
```
Expected: all three lines present. If `CONFIG_LV_USE_ARC=y` is missing, add it to `prj.conf` and note it; otherwise proceed with no `prj.conf` change.

- [ ] **Step 2: Create `src/ui/splash.hpp`**

```cpp
#ifndef UI_SPLASH_HPP_
#define UI_SPLASH_HPP_

namespace ui {

// Boot splash on the top layer: a blue ring fills under "PSU MONITOR", then the
// overlay dissolves to reveal the watchface (~1.5s). Self-deleting; call once
// after the watchface screen exists.
class Splash {
 public:
  static void Show();
};

}  // namespace ui

#endif  // UI_SPLASH_HPP_
```

- [ ] **Step 3: Create `src/ui/splash.cpp`**

```cpp
#include "splash.hpp"

#include <cstdint>

#include <lvgl.h>

namespace {

constexpr uint32_t kBgColor     = 0x000000;
constexpr uint32_t kAccentColor = 0x00AAFF;  // ring indicator + title
constexpr uint32_t kTrackColor  = 0x202020;  // ring background track

constexpr int32_t kRingSize     = 90;
constexpr int32_t kRingWidth    = 6;
constexpr int32_t kRingYOffset  = -25;       // ring sits in the upper half
constexpr int32_t kTitleYOffset = 55;        // title below the ring

constexpr uint32_t kFillDelayMs    = 100;
constexpr uint32_t kFillDurationMs = 1100;
constexpr uint32_t kFadeDelayMs    = 1200;   // == kFillDelayMs + kFillDurationMs
constexpr uint32_t kFadeDurationMs = 300;    // total ~1.5s

// lv_anim exec callback: drive the arc value.
void RingValueCb(void* obj, int32_t v) {
  lv_arc_set_value(static_cast<lv_obj_t*>(obj), v);
}

// lv_anim exec callback: drive the overlay opacity (cascades to children).
void OverlayOpaCb(void* obj, int32_t v) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj),
                       static_cast<lv_opa_t>(v), LV_PART_MAIN);
}

// lv_anim completion callback: delete the overlay once the fade is done.
void SplashDoneCb(lv_anim_t* a) {
  // Defer the delete so the animation engine is finished with the object.
  lv_obj_delete_async(static_cast<lv_obj_t*>(a->var));
}

}  // namespace

namespace ui {

void Splash::Show() {
  lv_display_t* disp = lv_display_get_default();
  const int32_t w = lv_display_get_horizontal_resolution(disp);
  const int32_t h = lv_display_get_vertical_resolution(disp);

  // Full-screen opaque overlay on the top layer (covers the watchface).
  lv_obj_t* overlay = lv_obj_create(lv_layer_top());
  lv_obj_set_size(overlay, w, h);
  lv_obj_center(overlay);
  lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(kBgColor), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);

  // Loading ring (upper area).
  lv_obj_t* ring = lv_arc_create(overlay);
  lv_obj_set_size(ring, kRingSize, kRingSize);
  lv_obj_align(ring, LV_ALIGN_CENTER, 0, kRingYOffset);
  lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
  lv_arc_set_rotation(ring, 270);
  lv_arc_set_bg_angles(ring, 0, 360);
  lv_arc_set_range(ring, 0, 100);
  lv_arc_set_value(ring, 0);
  lv_obj_set_style_arc_width(ring, kRingWidth, LV_PART_MAIN);
  lv_obj_set_style_arc_color(ring, lv_color_hex(kTrackColor), LV_PART_MAIN);
  lv_obj_set_style_arc_width(ring, kRingWidth, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ring, lv_color_hex(kAccentColor), LV_PART_INDICATOR);

  // Title below the ring.
  lv_obj_t* title = lv_label_create(overlay);
  lv_label_set_text(title, "PSU MONITOR");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(kAccentColor), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, kTitleYOffset);

  // (1) Fill the ring 0 -> 100.
  lv_anim_t fill;
  lv_anim_init(&fill);
  lv_anim_set_var(&fill, ring);
  lv_anim_set_exec_cb(&fill, RingValueCb);
  lv_anim_set_values(&fill, 0, 100);
  lv_anim_set_duration(&fill, kFillDurationMs);
  lv_anim_set_delay(&fill, kFillDelayMs);
  lv_anim_set_path_cb(&fill, lv_anim_path_ease_out);
  lv_anim_start(&fill);

  // (2) After the fill, dissolve the overlay to reveal the watchface.
  lv_anim_t fade;
  lv_anim_init(&fade);
  lv_anim_set_var(&fade, overlay);
  lv_anim_set_exec_cb(&fade, OverlayOpaCb);
  lv_anim_set_values(&fade, 255, 0);
  lv_anim_set_duration(&fade, kFadeDurationMs);
  lv_anim_set_delay(&fade, kFadeDelayMs);
  lv_anim_set_path_cb(&fade, lv_anim_path_ease_in_out);
  lv_anim_set_completed_cb(&fade, SplashDoneCb);
  lv_anim_start(&fade);
}

}  // namespace ui
```

- [ ] **Step 4: Hook it into `src/ui/watchface.cpp`**

Add the include near the other UI includes (after `#include "psu_service.h"`):
```cpp
#include "splash.hpp"
```
Change `Watchface::Show()` from:
```cpp
void Watchface::Show() {
  ui_init();
  psu_ui_init();
}
```
to:
```cpp
void Watchface::Show() {
  ui_init();
  psu_ui_init();
  Splash::Show();
}
```

- [ ] **Step 5: Add `splash.cpp` to the build**

In `CMakeLists.txt`, in the `set(app_SRCS ...)` list, add (next to the existing `${PROJECT_SOURCE_DIR}/src/ui/watchface.cpp` line):
```cmake
  ${PROJECT_SOURCE_DIR}/src/ui/splash.cpp
```
(`src/ui` is already in `include_directories`, so `#include "splash.hpp"` resolves.)

- [ ] **Step 6: Build**

Run the standard build command.
Expected: compiles + links, `Wrote ... zephyr.uf2`. Record RAM/FLASH "Used Size" and compare to the pre-change figures (FLASH ~27.18%, RAM ~84.22%) — expect ~no change.

If `lv_font_montserrat_28` is undefined at link, confirm `CONFIG_LV_FONT_MONTSERRAT_28=y` in `prj.conf` (it is). If an arc symbol is undefined, confirm `CONFIG_LV_USE_ARC=y`. Fix only mechanical issues; if stuck, report BLOCKED with the exact error.

- [ ] **Step 7: Commit**

```bash
cd /Users/litao/Developer/zpsu_mon
git add src/ui/splash.hpp src/ui/splash.cpp src/ui/watchface.cpp CMakeLists.txt
git commit -m "feat(ui): add PSU boot splash (ui::Splash)

Blue ring-fill under \"PSU MONITOR\" then dissolve to the watchface (~1.5s),
on lv_layer_top, self-deleting. Shown from Watchface::Show().

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Hardware verification (developer)

**Files:** none.

- [ ] **Step 1: Flash and observe**

Copy `build_lcd2/zephyr/zephyr.uf2` to the RP2040 (BOOTSEL mass-storage). On power-up, confirm: a blue ring fills under "PSU MONITOR" over ~1.2s, then the overlay fades (~0.3s) and the watchface appears. Confirm no flicker/leftover overlay and that the watchface is interactive afterward (ON/OFF, CV/CC, CC up/down still work).

---

## Self-Review Notes (author check)

- **Spec coverage:** module `ui::Splash` + static `Show()` (§2 → Task 1 Steps 2-3); branded ring + dissolve, blue `0x00AAFF`, title "PSU MONITOR" only, ~1.5s, pure vector on `lv_layer_top`, self-deleting (§3 → Step 3); hook in `Watchface::Show()` after `ui_init/psu_ui_init` (§4 → Step 4); CMake add (§4 → Step 5); deps already enabled (§5 → Step 1); verification build + hardware (§6 → Step 6 + Task 2).
- **Placeholder scan:** none — all code is complete and verbatim.
- **Type/name consistency:** `ui::Splash::Show()` declared in `splash.hpp`, defined in `splash.cpp`, called in `watchface.cpp`. The three anim callbacks (`RingValueCb`/`OverlayOpaCb`/`SplashDoneCb`) match the `lv_anim` exec/completed callback signatures (`void(void*,int32_t)` / `void(lv_anim_t*)`). Named constants used consistently; `kFadeDelayMs == kFillDelayMs + kFillDurationMs` so the fade starts exactly when the fill ends.
- **LVGL 9.x API:** mirrors the proven `watchface_anim.c::watchface_splash_show` (lv_arc_create/set_rotation/set_bg_angles/set_range/set_value, `lv_obj_remove_style(..., LV_PART_KNOB)`, `lv_anim_set_duration/_delay/_path_cb/_completed_cb`, `lv_obj_delete_async`). Differences from the reference are intentional: title-only (no subtitle), accent `0x00AAFF` (vs `0x00FFFF`), ring above title (vs title inside ring), ring 90px (vs 140px).
