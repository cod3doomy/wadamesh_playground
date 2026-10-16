// SPDX-License-Identifier: GPL-3.0-or-later
//
// No-touch provider for the Heltec V4-R2 + Waveshare 3.5" ST7796S env
// (HELTEC_LORA_V4_ST7796). See platformio.ini's env comment block.
//
// The Waveshare panel's touch controller is an FT6336U and this repo has drivers
// for GT911 and CHSC6x but NOT for FT5x06. Something still has to define the
// shared touch-input API, though: UITask calls it unconditionally — lvglTouchRead,
// kbApplyRotation(), UITask::begin() — outside the CAP_TOUCH guards, so leaving
// it undefined is a LINK error, not a silent no-op.
//
// The one file that could otherwise have provided it, helpers/input/TDeckTouch.cpp
// (a GT911 driver gated on HAS_TOUCH_UI alone), is excluded from this env's
// build_src_filter on purpose: heltecV4CapTouchBegin() there unconditionally does
// Wire.end() / Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL), which would re-route the
// board I2C bus off 38/4 and take the M5Stack CardKB's keyboard with it.
//
// Every entry point below is therefore a cheap, allocation-free no-op that
// reports "no touch". The UI and the CardKB work; touch is a follow-up.
#include <helpers/input/HeltecV4CapTouch.h>   // QUOTED form above is resolved against variants/

#if defined(HELTEC_LORA_V4_ST7796) && defined(ESP32) && !defined(ST7796_TOUCH_FT5X06)

#include <Arduino.h>
#include <helpers/ui/MomentaryButton.h>   // BUTTON_EVENT_NONE

bool heltecV4CapTouchBegin() { return false; }   // no FT5x06 driver yet — never succeeds

int heltecV4CapTouchCheck() { return BUTTON_EVENT_NONE; }

bool heltecV4CapTouchPopTap(uint16_t* x, uint16_t* y) {
  (void)x; (void)y;
  return false;
}

bool heltecV4CapTouchGetLive(uint16_t* x, uint16_t* y) {
  if (x) *x = 0;
  if (y) *y = 0;
  return false;   // "finger not down" — LVGL sees a permanently released pointer
}

bool heltecV4CapTouchPopSwipe(int8_t* x_dir, int8_t* y_dir) {
  if (x_dir) *x_dir = 0;
  if (y_dir) *y_dir = 0;
  return false;
}

// No controller to poll, so no background task is spawned: the UI stays on its
// inline path and just gets "no touch" from heltecV4CapTouchCheck() above.
bool heltecV4CapTouchStartBackgroundPoll(uint32_t period_ms) {
  (void)period_ms;
  return false;
}

bool heltecV4CapTouchIsAsyncPolling() { return false; }
bool heltecV4CapTouchIsSwiping() { return false; }
void heltecV4CapTouchSetSlowPoll(bool slow) { (void)slow; }
void heltecV4CapTouchSetRotation(uint8_t lvgl_rot) { (void)lvgl_rot; }
void heltecV4CapTouchSetPointRotation(uint8_t lvgl_rot) { (void)lvgl_rot; }
void heltecV4CapTouchGetRaw(uint16_t* rx, uint16_t* ry) {
  if (rx) *rx = 0;
  if (ry) *ry = 0;
}

// Shown in the Settings -> Diagnostics ring, so the tester can see WHY there is
// no touch instead of wondering whether the probe simply failed.
const char* heltecV4CapTouchDebug() { return "touch: none (FT5x06 driver not yet written)"; }

#endif  // HELTEC_LORA_V4_ST7796
