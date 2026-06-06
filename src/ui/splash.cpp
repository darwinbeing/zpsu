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
  lv_obj_remove_flag(overlay, static_cast<lv_obj_flag_t>(
                                  LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
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
