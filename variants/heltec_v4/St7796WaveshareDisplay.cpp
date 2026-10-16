// SPDX-License-Identifier: GPL-3.0-or-later
#include "St7796WaveshareDisplay.h"

#if defined(HELTEC_LORA_V4_ST7796) && defined(ESP32)

// MEASURED on this panel: 320x480 glass in a 320x480 GRAM -> NO colstart /
// rowstart offset. TFT_eSPI's ST7796_Rotation.h only injects an offset when
// CGRAM_OFFSET is defined, so defining it here would shift EVERY frame with no
// error at all. The sibling pager driver REQUIRES the flag (its glass is 222px
// inside a 320px GRAM) -- that requirement must NOT be carried over.
#if defined(CGRAM_OFFSET)
  #error "St7796WaveshareDisplay must NOT be built with -D CGRAM_OFFSET -- this panel's 320x480 \
glass fills the controller's 320x480 GRAM exactly (measured), so there is no colstart/rowstart \
offset. Defining it shifts every frame silently. The pager's ST7796LCDDisplay needs the flag for \
the opposite reason: its glass is 222px inside a 320px GRAM."
#endif

#if !defined(ST7796_DRIVER)
  #error "St7796WaveshareDisplay requires -D ST7796_DRIVER=1 in this env's platformio.ini build_flags."
#endif

#if !defined(LOAD_GLCD)
  #error "St7796WaveshareDisplay requires -D LOAD_GLCD=1 -- its DisplayDriver text API uses TFT_eSPI font 1."
#endif

#ifndef DISPLAY_ROTATION
  #define DISPLAY_ROTATION 1   // r=1 == MADCTL 0x20, MEASURED correct on this panel
#endif
#ifndef DISPLAY_SCALE_X
  #define DISPLAY_SCALE_X 1.0f // native res == logical res; kept for shape-parity with ST7796LCDDisplay
#endif
#ifndef DISPLAY_SCALE_Y
  #define DISPLAY_SCALE_Y 1.0f
#endif

St7796WaveshareDisplay::St7796WaveshareDisplay(RefCountedDigitalPin* peripher_power)
  : DisplayDriver(LOGICAL_WIDTH, LOGICAL_HEIGHT),
    _periph_power(peripher_power),
    _isOn(false),
    _color(0xFFFF),
    _brightness_pct(100) { }

bool St7796WaveshareDisplay::begin() {
  if (!_isOn) {
    // Reads TFT_WIDTH / TFT_HEIGHT / ST7796_DRIVER / TFT_* pins / USE_HSPI_PORT
    // from the build flags and brings up its own SPI3 bus (SPI2 belongs to the
    // SX1262 on this board).
    display.init();

    // MANDATORY: this panel outputs the bitwise complement of every colour.
    // TFT_eSPI's generic ST7796_Init.h never sends an inversion command
    // (0x20/0x21) at all, so without this the whole UI renders inverted
    // (white background / black logo instead of the dark theme) while every
    // other thing -- centering, timing, wake -- looks perfectly correct.
    // invertDisplay() sends the same command (doubled, per TFT_eSPI's own
    // comment, "otherwise it does not always work").
    display.invertDisplay(true);

    // Bring-up diagnostic: read the controller's own status registers back over
    // SPI. A healthy post-init ST7796 reports RDDPM(0x0A)=0x9C (booster on,
    // sleep out, display on) and RDDCOLMOD(0x0C)=0x55 (16bpp). All-0x00/0xFF
    // means the controller isn't responding at all (reset/power/SPI fault) --
    // which is the only thing that separates "panel never initialised" from
    // "panel fine, backlight dark"; those look identical on the glass.
    // MEASURED: this panel is 4-WIRE with NO MISO (TFT_MISO=-1), so a command
    // read returns nothing useful. Only emit the line on a bus that actually
    // has a return line, rather than logging a garbage value.
    #if TFT_MISO >= 0
      Serial.printf("[DISP] RDDPM=0x%02X MADCTL=0x%02X COLMOD=0x%02X RDDIM=0x%02X\n",
                    display.readcommand8(0x0A), display.readcommand8(0x0B),
                    display.readcommand8(0x0C), display.readcommand8(0x0D));
    #endif

    setDisplayRotation(DISPLAY_ROTATION);
    setLogicalSize((int)(display.width() / DISPLAY_SCALE_X), (int)(display.height() / DISPLAY_SCALE_Y));
    display.setAttribute(CP437_SWITCH, true);
display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_WHITE);
    display.setTextSize((uint8_t)(2 * DISPLAY_SCALE_X));

    // MEASURED backlight: plain ACTIVE-HIGH PWM on GPIO 6 (TFT_BL). No AW9364.
    // analogWrite() owns the LEDC channel here; UITask's applyBrightness()
    // re-attaches GPIO6 to its own LEDC channel later in boot (PIN_TFT_LEDA_CTL
    // is 6 in this env, exactly as on the stock V4 where it equalled TFT_BL).
    pinMode(TFT_BL, OUTPUT);
    setBrightness(_brightness_pct);

    if (_periph_power) _periph_power->claim();
    _isOn = true;
    Serial.printf("[DISP] St7796Waveshare %dx%d rot=%d BL=%d\n",
                  display.width(), display.height(), (int)DISPLAY_ROTATION, (int)TFT_BL);
  }
  return true;
}

void St7796WaveshareDisplay::turnOn() { St7796WaveshareDisplay::begin(); }

void St7796WaveshareDisplay::turnOff() {
  if (_isOn) {
    const uint8_t keep = _brightness_pct;
    setBrightness(0);        // backlight pad actually low...
    _brightness_pct = keep;  // ...but turnOn()'s begin() must restore the pre-off level, not 0
    display.writecommand(TFT_DISPOFF);   // and stop the panel driving the crystals
    _isOn = false;
    if (_periph_power) _periph_power->release();
  }
}

void St7796WaveshareDisplay::panelSleep(bool sleep) {
  if (!_isOn) return;   // never brought up (or turned off): nothing to put to sleep
  if (sleep) {
    // ST7796 datasheet: no SLPIN within 120 ms of an SLPOUT.
    const uint32_t since = millis() - _wake_ms;
    if (_wake_ms && since < 120) delay(120 - since);
    display.writecommand(TFT_DISPOFF);
    display.writecommand(ST7796_SLPIN);
    delay(5);   // the bus stays quiet 5 ms after SLPIN
  } else {
    display.writecommand(ST7796_SLPOUT);
    _wake_ms = millis();
    delay(6);   // supply and clocks settle at least 5 ms before the next command
    display.writecommand(TFT_DISPON);
  }
}

void St7796WaveshareDisplay::clear() { display.fillScreen(TFT_BLACK); }

void St7796WaveshareDisplay::startFrame(ColorVal bkg) { display.fillScreen(bkg); }

void St7796WaveshareDisplay::setTextSize(int sz) { display.setTextSize((uint8_t)(sz * DISPLAY_SCALE_X)); }

void St7796WaveshareDisplay::setColor(ColorVal c) {
  _color = c;   // ColorVal IS RGB565 now (1.17 UIColor)
  display.setTextColor(_color);
}

void St7796WaveshareDisplay::setCursor(int x, int y) {
  display.setCursor((int16_t)(x * DISPLAY_SCALE_X), (int16_t)(y * DISPLAY_SCALE_Y));
}

void St7796WaveshareDisplay::print(const char* str) { display.print(str); }

void St7796WaveshareDisplay::fillRect(int x, int y, int w, int h) {
  display.fillRect(x * DISPLAY_SCALE_X, y * DISPLAY_SCALE_Y, w * DISPLAY_SCALE_X, h * DISPLAY_SCALE_Y, _color);
}

void St7796WaveshareDisplay::drawRect(int x, int y, int w, int h) {
  display.drawRect(x * DISPLAY_SCALE_X, y * DISPLAY_SCALE_Y, w * DISPLAY_SCALE_X, h * DISPLAY_SCALE_Y, _color);
}

void St7796WaveshareDisplay::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  // TFT_eSPI has no native XBM primitive -- same manual bit-unpack loop as the pager's driver.
  uint8_t byteWidth = (w + 7) / 8;
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      uint8_t byte = bits[j * byteWidth + i / 8];
      if (byte & (0x80 >> (i & 7))) {
        for (int dy = 0; dy < DISPLAY_SCALE_Y; dy++) {
          for (int dx = 0; dx < DISPLAY_SCALE_X; dx++) {
            display.drawPixel((int32_t)(x * DISPLAY_SCALE_X + i * DISPLAY_SCALE_X + dx),
                              (int32_t)(y * DISPLAY_SCALE_Y + j * DISPLAY_SCALE_Y + dy), _color);
          }
        }
      }
    }
  }
}

uint16_t St7796WaveshareDisplay::getTextWidth(const char* str) {
  return (uint16_t)(display.textWidth(str) / DISPLAY_SCALE_X);
}

void St7796WaveshareDisplay::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_isOn || !pixels || w <= 0 || h <= 0) return;
  // startWrite()/endWrite() is the documented TFT_eSPI idiom and matches the WORKING
  // T-LoRa Pager driver (src/helpers/ui/ST7796LCDDisplay.cpp). setAddrWindow() opens and
  // closes its own transaction, but end_tft_write() no-ops while inTransaction is set, so
  // the nesting is safe. Removing this wrapper did NOT fix the boot loop -- it was never
  // the bug, and it is restored here for parity with the sibling driver that works.
  display.startWrite();
  display.setAddrWindow(x, y, w, h);   // colstart/rowstart are 0 on this panel
  display.pushColors(const_cast<uint16_t*>(pixels), (uint32_t)(w * h));   // swap=true matches LVGL RGB565
  display.endWrite();
}

void St7796WaveshareDisplay::endFrame() { /* no-op: pushColors' own endWrite() already closed the SPI transaction */ }

void St7796WaveshareDisplay::setDisplayRotation(uint8_t r) {
  // MEASURED on this panel: the correct landscape orientation is MADCTL=0x20 (MV
  // only). Of the four library rotations, r=1 selects exactly that.
  //
  // THE EARLIER RAW-MADCTL HACK WAS A REAL BUG, BUT NOT THE BOOT LOOP. The env
  // used to declare TFT_WIDTH=480 / TFT_HEIGHT=320 -- LANDSCAPE dimensions for a
  // natively 320x480 PORTRAIT panel -- and then wrote MADCTL=0x20 by hand instead
  // of calling setRotation(). That left TFT_eSPI's cached `rotation` member at 0
  // while the GRAM was genuinely in MV, so setWindow()'s
  // `if (rotation & 0x01) transpose(x0, y0)` never transposed and the library's
  // window arithmetic disagreed with the hardware's orientation.
  //
  // It did NOT corrupt the heap. The boot loop -- the TLSF abort in the first
  // allocator to walk the damaged free list, with the crash victim moving between
  // SPIFFS.begin() and UITask::begin() as the allocation pattern changed -- was
  // the panel reset line parked on GPIO26, which is the ESP32-S3's PSRAM
  // chip-select (CONFIG_SPIRAM_CS_IO=26 in the SDK sdkconfig). TFT_eSPI's init()
  // runs pinMode(26, OUTPUT), detaching the pad from SPICS in the GPIO matrix,
  // then holds CE# low for 20 ms. During that window PSRAM had no chip-select
  // and could not frame its own accesses; its free-list metadata lives in PSRAM,
  // so the "moving crash victim" was corrupted allocator metadata, not a buffer
  // overflow from the MADCTL skew. Fix: panel reset moved to GPIO3 (see
  // PIN_TFT_RST in platformio.ini). The raw-MADCTL hack had nothing to do with it.
  //
  // The fix for the rotation desync is not a cleverer raw register write -- it is
  // to stop lying to the library. The panel is declared at its NATIVE 320x480 in
  // the build flags and TFT_eSPI owns the rotation, so its cached state, the GRAM
  // orientation and the address-window math all agree.
  display.setRotation(r);
  setLogicalSize((int)(display.width() / DISPLAY_SCALE_X), (int)(display.height() / DISPLAY_SCALE_Y));
}

void St7796WaveshareDisplay::setBrightness(uint8_t pct) {
  if (pct > 100) pct = 100;
  // MEASURED: active-HIGH PWM backlight on GPIO 6 (TFT_BL). Map 0-100% onto the
  // 0-255 analogWrite() duty. Deliberately NOT LovyanGFX Light_PWM (its
  // `(ledcSetup(...) > 0 && ...)` init test inverts on arduino-esp32 2.x, where
  // ledcSetup() returns ESP_OK == 0 on success -- the channel never attaches
  // and the backlight stays dark) and NOT the pager's AW9364 stepped dimmer.
  const uint8_t duty = (uint8_t)(((uint16_t)pct * 255u + 50u) / 100u);
  if (TFT_BL >= 0) analogWrite(TFT_BL, duty);
  _brightness_pct = pct;
}

#endif
