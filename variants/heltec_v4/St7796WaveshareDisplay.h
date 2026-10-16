// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Waveshare 3.5" capacitive-touch LCD on a Heltec WiFi LoRa 32 V4 (V4-R2),
// driven via TFT_eSPI. New sibling of src/helpers/ui/ST7796LCDDisplay.{h,cpp}
// (which serves the T-LoRa Pager's 480x222 glass and is left untouched).
//
// EVERYTHING below is MEASURED on real hardware -- do not "correct" any of it
// back to a datasheet default:
//
//   * Panel ........... Waveshare 3.5" ST7796S, natively 320x480 PORTRAIT.
//                       TFT_WIDTH=320 / TFT_HEIGHT=480 are the NATIVE values and
//                       must stay that way. Rotations 0/2 are the portrait ones;
//                       rotations 1/3 are landscape. This build runs
//                       DISPLAY_ROTATION=1, i.e. MADCTL 0x20 (TFT_MAD_MV), for a
//                       480x320 landscape UI.
//                       DISCARDED THEORY -- do not resurrect it: "declare
//                       TFT_WIDTH=480 / TFT_HEIGHT=320 as a landscape pair and
//                       write raw MADCTL by hand instead of calling
//                       setRotation()". That is DEAD. It desynchronised TFT_eSPI's
//                       cached `rotation` member from the GRAM's real orientation,
//                       so setWindow()'s `if (rotation & 0x01) transpose(x0, y0)`
//                       never transposed. It was a real bug -- but it was NOT the
//                       heap corruption / boot loop. See the boot-loop note in
//                       platformio.ini (PIN_TFT_RST) for what that actually was.
//   * Bus ............. 4-wire SPI with NO MISO (TFT_MISO=-1). Write-only.
//   * SPI3 ............ the SX1262 owns SPI2 on this board, so the panel is on
//                       SPI3 (-D USE_HSPI_PORT=1).
//   * Inversion ....... this panel outputs the BITWISE COMPLEMENT of every
//                       colour, so inversion must be ON. TFT_eSPI's generic
//                       ST7796_Init.h never sends 0x20/0x21 at all, so
//                       begin() calls invertDisplay(true) explicitly -- the same
//                       requirement independently confirmed on the pager's ST7796
//                       panel batch.
//   * GRAM offset ..... NONE. This glass is 320x480 inside a 320x480 GRAM, so
//                       there is no colstart/rowstart to apply. Do NOT define
//                       CGRAM_OFFSET here (the pager's driver REQUIRES it, but
//                       only because ITS glass is 222px inside a 320px GRAM).
//                       Getting this wrong shifts every frame silently -- the
//                       .cpp turns it into a build failure.
//   * Backlight ....... plain ACTIVE-HIGH PWM on GPIO 6 (TFT_BL). NOT the AW9364
//                       stepped dimmer the pager driver drives.
//
// LovyanGFX's Light_PWM is deliberately NOT used anywhere here: on
// arduino-esp32 2.x its init macro is `(ledcSetup(...) > 0 && ...)`, but
// ledcSetup() returns ESP_OK == 0 on success -- the test inverts, the LEDC
// channel never attaches to the pin, and the backlight simply stays dark.
// We drive the pad with analogWrite() and let UITask's LEDC take it over once
// the UI is up (same hand-off the stock V4 does on PIN_TFT_LEDA_CTL).
#if defined(HELTEC_LORA_V4_ST7796) && defined(ESP32)

#include <helpers/ui/DisplayDriver.h>
#include <helpers/RefCountedDigitalPin.h>
#include <SPI.h>
#include <TFT_eSPI.h>

class St7796WaveshareDisplay : public DisplayDriver {
  TFT_eSPI display;
  RefCountedDigitalPin* _periph_power;
  bool _isOn;
  uint16_t _color;
  uint32_t _wake_ms = 0;     // last SLPOUT: the panel needs 120 ms before the next SLPIN
  uint8_t  _brightness_pct;  // exact requested pct (0-100), cached so getBrightness()
                             // survives the 0-100 -> 0-255 -> 0-100 round trip lossily

public:
  // MEASURED: the panel is natively 320x480 portrait; the UI runs the 480x320
  // landscape rotation. begin()/setDisplayRotation() overwrite this via
  // setLogicalSize() using TFT_eSPI's own (rotation-aware) width()/height().
  static const int LOGICAL_WIDTH = 480;
  static const int LOGICAL_HEIGHT = 320;

  // V4-R2 periph power (Vext, GPIO 36, ACTIVE LOW -- Heltec's later datasheet
  // says HIGH, meshcomod's stock PIN_VEXT_EN_ACTIVE=HIGH is wrong for early
  // V4-R2). target.cpp passes NULL on the plain V4, so this normally stays null
  // and the claim/release below no-ops; accepted for shape-parity with
  // LGFXDisplay, which is constructed the same way.
  St7796WaveshareDisplay(RefCountedDigitalPin* peripher_power = nullptr);

  bool begin();

  bool isOn() override { return _isOn; }
  void turnOn() override;
  void turnOff() override;
  // Panel sleep for screen-off (the UI's backlight-off): DISPOFF + SLPIN stop the
  // panel's oscillator, booster and liquid-crystal drive, which otherwise keep
  // refreshing the same image behind the dark backlight (power, and burn-in).
  // Frame memory is kept, so waking is SLPOUT + DISPON with no redraw.
  void panelSleep(bool sleep);
  void clear() override;
  void startFrame(ColorVal bkg = UIColor::window_bkg) override;
  void setTextSize(int sz) override;
  void setColor(ColorVal c) override;
  void setCursor(int x, int y) override;
  void print(const char* str) override;
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override;
  uint16_t getTextWidth(const char* str) override;
  void endFrame() override;

  // Extra methods beyond DisplayDriver, called directly on the concrete type
  // (same pattern as ST7796LCDDisplay / LGFXDisplay).
  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);
  void setDisplayRotation(uint8_t r);

  // PWM backlight hook, pct in / pct out by design (0-100), matching the
  // pct-based Settings convention; mapped onto analogWrite() 0-255 here.
  void setBrightness(uint8_t pct);
  uint8_t getBrightness() const { return _brightness_pct; }
};

#endif