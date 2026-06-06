# PSU Boot Splash Screen — Design

- Date: 2026-06-06
- Target board: `rpi_pico` / Pico Display Pack 2 (320×240)
- LVGL: 9.3.0 (bundled with Zephyr v4.2.1)
- Goal: A short, pure-vector boot splash shown on power-up, then dissolving into
  the watchface.

## 1. Confirmed decisions

| Decision | Choice |
|---|---|
| Scope | A fresh, self-contained C++ splash module — only the boot splash (NOT the older `watchface_anim.c` monitoring suite) |
| Style | Branded ring + dissolve: a centered title with a filling loading ring above it; when the ring fills, the whole overlay dissolves to reveal the watchface |
| Title | "PSU MONITOR" (no subtitle) |
| Accent color | Blue `0x00AAFF` |
| Duration | ~1.5 s total (ring fill ~1.2 s, fade-out ~0.3 s) |
| Rendering | Pure vector (no font image / picture assets beyond a built-in text font) |

The existing `src/ui/watchface_anim.c` (untracked, never compiled) contains a
`watchface_splash_show()`; it is used only as an API reference for the exact
LVGL 9.x calls. We are NOT wiring that module in.

## 2. Architecture

New module `src/ui/splash.{hpp,cpp}`, namespace `ui`, stateless (static method
— same shape as `ui::Watchface` and `timekeeping::Clock`):

```cpp
namespace ui {

class Splash {
 public:
  // Shows the boot splash on the top layer, plays the ring-fill + dissolve
  // animation, and deletes itself when done. Call once, after the watchface
  // screen exists.
  static void Show();
};

}  // namespace ui
```

All LVGL objects are created on `lv_layer_top()` so the splash covers the
already-built watchface and never touches the SquareLine-generated `ui_Screen1`.
The overlay deletes itself at the end of the fade — no persistent state, no
teardown API needed.

## 3. Visual and timing (LVGL 9.3)

1. **Overlay:** an `lv_obj` on `lv_layer_top()`, full-screen (320×240), black
   background, fully opaque — covers the freshly-created watchface.
2. **Ring:** an `lv_arc` (~90×90) centered in the upper half: blue `0x00AAFF`
   indicator, dark-grey track, knob removed, range 0–100, start at the top.
3. **Title:** an `lv_label` "PSU MONITOR" centered below the ring, blue
   `0x00AAFF`, using the LVGL default Montserrat font (a larger Montserrat size
   is used if one is enabled in Kconfig, otherwise the default).
4. **Fill:** an `lv_anim` drives the arc value 0 → 100 over ~1.2 s (ease-out).
5. **Dissolve:** the fill's completion callback starts a second `lv_anim` fading
   the overlay opacity 255 → 0 over ~0.3 s; that fade's completion callback
   deletes the overlay → the watchface is revealed.

All tunables (size, position, color, durations, font) are file-scope named
constants. LVGL calls mirror the proven 9.x patterns in
`watchface_anim.c::watchface_splash_show` (arc value animation via exec
callback, `completed_cb`, opacity fade, `lv_obj_delete`).

## 4. Integration (one non-generated file + build)

- `src/ui/watchface.cpp` — in `Watchface::Show()`, after `ui_init();
  psu_ui_init();`, add `Splash::Show();` and `#include "splash.hpp"`. This is
  the moment in the boot flow when the watchface screen has just been built
  (`main` → `App::RunInit` → `DisplayControl::PowerOn(true)` →
  `WatchfaceApp::Start` → `general_work` OPEN_WATCHFACE → `Watchface::Show()`).
- `CMakeLists.txt` — add `${PROJECT_SOURCE_DIR}/src/ui/splash.cpp` to
  `app_SRCS` (`src/ui` is already in `include_directories`).

No other files change. The splash is purely additive.

## 5. Dependencies and budget

- Requires `CONFIG_LV_USE_ARC=y` (recorded as already enabled by the earlier
  anim design; the implementation confirms it at build time, and adds it to
  `prj.conf` if missing).
- Resource cost is negligible: a handful of vector objects + two short
  animations, all freed on completion. RAM/Flash essentially unchanged.

## 6. Verification

No automated test framework (bare-metal embedded). Acceptance:
1. Compiles and links (`ninja -C build_lcd2`, Anaconda toolchain, board
   `rpi_pico`); compare RAM/Flash to before (expect ~no change).
2. Hardware: flash `build_lcd2/zephyr/zephyr.uf2`; on power-up a blue ring
   fills under "PSU MONITOR" and after ~1.5 s the overlay dissolves into the
   watchface.

## 7. Risks

- **`CONFIG_LV_USE_ARC` not enabled** → arc APIs unavailable. Mitigation: verify
  at build; enable in `prj.conf` if needed (small, reversible).
- **Font size availability** — the title uses whatever Montserrat size is
  enabled; if only the 14 px default exists the title is small but functional.
  No hard dependency on a specific size.
- **Splash blocking the UI** — animations are non-blocking (LVGL timer-driven);
  the work-queue/`lvgl_update` rendering already runs, so the splash plays while
  the rest of the system initializes.
