# Convert Remaining C Modules to C++ Classes — Design

- Date: 2026-06-06
- Scope: the remaining first-party and (now hard-forked) ZSWatch C modules in `src/`
- Goal: Refactor every remaining compiled C module into an object-oriented C++ class
  using the Google C++ Style Guide, behavior-preserving, following the pattern already
  proven on the PSU module (`src/psu/`).

## 1. Background

The PSU module was already converted to C++ (`psu::PsuController` + `psu_service` adapter +
`psu_channel.c` for zbus macros). The user wants the rest of the C codebase abstracted into
classes and changed to standard C++ as well. This is a permanent hard fork of ZSWatch (the
project is now a PSU monitor), so upstream-merge-ability is no longer a constraint.

## 2. Confirmed decisions

| Decision | Choice |
|---|---|
| ZSWatch upstream files | Convert them too (hard fork — no upstream merges) |
| Granularity | Every module becomes a class (uniform), even small ones |
| `watchface_anim.c` | Leave untouched (untracked, not in the build) |
| Generated UI + fonts + `psu_channel.c` | Excluded (regenerated / intentional C) |
| Structure | One spec + one plan, one task per module, leaf→main order |
| Naming | Google C++ Style, one namespace per domain |
| zbus/BT macros in `watchface_app` | Isolated in a C glue file (`watchface_app_glue.c`) |

## 3. The generalized pattern (same as PSU)

```
<module>.hpp / .cpp   C++ class: state + behavior, Google C++ style, named constants.
<module>_glue.c       ONLY when needed: a C file holding Zephyr registration macros that
                      do not compile cleanly as C++ (ZBUS_LISTENER_DEFINE, BT_CONN_CB_DEFINE),
                      forwarding to extern "C" hooks implemented on the C++ class.
```

- `SYS_INIT`, `K_WORK_DEFINE`, `K_WORK_DELAYABLE_DEFINE`, `K_TIMER_DEFINE` are proven to compile
  in C++ (already used in `psu_service.cpp`) → keep them in the `.cpp`.
- `ZBUS_LISTENER_DEFINE` and `BT_CONN_CB_DEFINE` use compound-literal / section-attribute
  expansions that are the same class of risk as `ZBUS_CHAN_DEFINE` (which we already isolated in
  `psu_channel.c`). For `watchface_app` they go in `watchface_app_glue.c`.
- `main()` stays a free function (C++ `int main(void)` is fine for Zephyr); its logic moves into
  an `App` class.

### Boundary facts (verified)
- Generated UI files (`ui.c`, `ui_helpers.c`, `ui_Screen1.c`) do NOT call our modules (only the
  already-handled `psu_*_event_cb`). No `extern "C"` needed for generated→our-code.
- The only cross-call into the conversion set from outside is `main.c` → `display_control_*` and
  `watchface_app_start`. Since `main` is being converted too, those become C++↔C++.
- Therefore the residual `extern "C"` surface is just: Zephyr-macro callbacks and `main()`.

## 4. Per-module breakdown

One namespace per domain; class names PascalCase; members `trailing_`; constants `kName`;
methods PascalCase.

| Module (old C) | New files | New class | State / responsibility | Retained C boundary |
|---|---|---|---|---|
| `src/led.c` | `src/led/led.{hpp,cpp}` | `led::Led` | GPIO LED blink via `K_TIMER`; auto-start | `SYS_INIT` entry (in `.cpp`) |
| `src/clock.c` | `src/clock/clock.{hpp,cpp}` | `timekeeping::Clock` | stateless POSIX-time wrapper (init/get_time/get_unix) | none |
| `src/notification_manager.c` | `src/notification/notification_manager.{hpp,cpp}` | `notify::NotificationManager` | fixed-size notification store + queries | none |
| `src/display_control.c` | `src/display/display_control.{hpp,cpp}` | `display::DisplayControl` | display power/brightness + LVGL render `K_WORK`; `is_on`, brightness | none |
| `src/ui/watchface_ui.c` | `src/ui/watchface.{hpp,cpp}` | `ui::Watchface` | LVGL watchface widget controller (show/remove/set_*) | none |
| `src/watchface_app.c` | `src/app/watchface_app.{hpp,cpp}` + `src/app/watchface_app_glue.c` | `app::WatchfaceApp` | coordinator: 4 zbus listeners, BT conn cbs, work dispatch, `SYS_INIT` | glue C file: `ZBUS_LISTENER_DEFINE`×4 + `BT_CONN_CB_DEFINE` → `extern "C"` hooks |
| `src/main.c` | `src/app/app.{hpp,cpp}` (keep `src/main.c`/`.cpp` thin) | `app::App` | init sequence + RGB PWM LED bring-up | `main()` |

Notes:
- `clock` and the new file paths use subdirectories to match the `src/psu/` precedent and keep
  each module self-contained. Final directory layout is adjustable during plan review.
- `Clock` is stateless → its methods are `static` (a class as the user requested; namespace
  `timekeeping` avoids clashing with `<time.h>`'s `clock`).
- `display::DisplayControl` exposes `LvglUpdate()` (was `lvgl_update`), used by `WatchfaceApp`.
- `app::App` keeps the existing `run_init_work`/system-workqueue init trick.

## 5. Behavior preservation

Pure structural refactor. All runtime behavior is preserved exactly, including:
- LED 1s blink timer; display power/brightness/LVGL render scheduling; notification storage
  semantics; the watchface work dispatch and the zbus listener `if (running)` guards; the
  `#if 0` dead battery/sensor paths in `watchface_app.c` (left as-is); the RGB PWM init in
  `main`; and the system-workqueue init submission from `main()`.
- All Zephyr init levels/priorities, channel/observer names, and event structs unchanged.

## 6. Excluded (unchanged), with rationale

- Generated UI: `src/ui/ui.c`, `ui_helpers.c`, `screens/ui_Screen1.c`, `ui_events.h`,
  `fonts/*` — regenerated by SquareLine / LVGL font converter; rewrites are overwritten.
- `src/psu/psu_channel.c` — intentionally C for the zbus channel macro.
- `src/ui/watchface_anim.c` — untracked, not in `app_SRCS` (not compiled).

## 7. Sequencing (each step builds green)

Leaf/utility modules first, coordinator and entry point last, so the build links at every commit:

1. `timekeeping::Clock`
2. `notify::NotificationManager`
3. `led::Led`
4. `display::DisplayControl`
5. `ui::Watchface`
6. `app::WatchfaceApp` (+ `watchface_app_glue.c`)
7. `app::App` / `main`

Each module: create class files, swap the source in `CMakeLists.txt` (`.c` out, `.cpp` [+glue]
in), update any now-C++ callers, delete the old `.c`/`.h`, build, commit.

## 8. Verification

No automated test framework (bare-metal embedded). Acceptance:
1. Each module task: incremental build compiles + links (`ninja -C build_lcd2`, Anaconda
   toolchain, board `rpi_pico`); confirm no name-mangling errors at any `extern "C"` boundary.
2. Final clean from-scratch rebuild links and produces `zephyr.uf2`; record RAM/FLASH (ample
   headroom: currently ~27% FLASH, ~84% RAM, so size is not a concern).
3. Hardware check: developer flashes `zephyr.uf2` and confirms watchface + PSU UI behavior.

## 9. Risks

- **`watchface_app` macro compilation under C++** (`ZBUS_LISTENER_DEFINE`, `BT_CONN_CB_DEFINE`):
  mitigated by isolating them in `watchface_app_glue.c` (proven `psu_channel.c` pattern). If, at
  build time, a macro turns out to compile fine in C++, it may stay in the `.cpp`; if not, it
  lives in the glue file. Either way the design holds.
- **Many ZSWatch headers referenced by `watchface_app`** are stubs / `#if 0`. Keep includes and
  dead code exactly as-is to preserve behavior and avoid scope creep.
- **Cross-module ordering**: doing leaf modules first avoids transient unresolved C++/C linkage.
