// SPDX-License-Identifier: GPL-3.0-or-later
#include "CardKbKeyboard.h"

#if defined(HAS_CARDKB) && defined(ESP32)

#include <Arduino.h>
#include <Wire.h>

namespace {
constexpr uint8_t  kCardKbAddress        = 0x5F;
constexpr uint32_t kPresentPollMs        = 12;
constexpr uint32_t kAbsentProbeMs        = 1000;
constexpr uint8_t  kDisconnectThreshold = 3;

uint8_t s_ring[8] = {};
uint8_t s_head = 0;
uint8_t s_tail = 0;
uint8_t s_failures = 0;
bool s_present = false;
uint32_t s_next_poll_ms = 0;

void ringPush(uint8_t key) {
  const uint8_t next = (uint8_t)((s_head + 1) & 7u);
  if (next != s_tail) {
    s_ring[s_head] = key;
    s_head = next;
  }
}

void ringClear() {
  s_head = s_tail = 0;
}
}  // namespace

// The CardKB gets its OWN I2C bus when PIN_CARDKB_SDA/SCL are provided.
//
// WHY (MEASURED, Waveshare V4-R2 + ST7796S): the board `Wire` on this build is
// GPIO38/4 -- the panel FT6336U touch line, whose pull-ups reference VCC and so
// idle at 4.23 V against a 3.6 V absolute maximum. Polling that bus for an absent
// 0x5F wedged SCL: `requestFrom(): i2cRead returned Error [STALL]` blocking the UI
// thread for 525 ms, every second, forever. The board reached [BOOT] ui ready and
// the splash animated, then went dark because the UI never got scheduled.
// The CardKB is physically wired to GPIO15/16, which nothing polled. This is also
// what makes goal 5 (CardKB v1.1) work at all -- it was never detectable before.
#if defined(PIN_CARDKB_SDA) && defined(PIN_CARDKB_SCL)
TwoWire s_cardkb_bus(1);   // hardware I2C1; the board bus is already taken by touch
bool s_bus_ready = false;
#endif

void cardKbPoll() {
  const uint32_t now = millis();
  if ((int32_t)(now - s_next_poll_ms) < 0) return;
  s_next_poll_ms = now + (s_present ? kPresentPollMs : kAbsentProbeMs);
  const bool was_present = s_present;

#if defined(PIN_CARDKB_SDA) && defined(PIN_CARDKB_SCL)
  if (!s_bus_ready) {
    s_cardkb_bus.begin(PIN_CARDKB_SDA, PIN_CARDKB_SCL, 100000);
    s_bus_ready = true;
  }
  TwoWire& bus = s_cardkb_bus;
#else
  // Legacy boards without dedicated pins: the board bus is shared with
  // touch/RTC/sensors, so never call begin() here -- ESP32Board owns it.
  TwoWire& bus = Wire;
#endif
  const int received = bus.requestFrom((int)kCardKbAddress, 1);
  if (received != 1) {
    while (bus.available()) (void)bus.read();
    if (s_failures < kDisconnectThreshold) ++s_failures;
    if (s_failures >= kDisconnectThreshold && s_present) {
      s_present = false;
      ringClear();
      s_next_poll_ms = now + kAbsentProbeMs;
    }
    return;
  }

  s_failures = 0;
  s_present = true;
  if (!was_present) s_next_poll_ms = now + kPresentPollMs;
  const int key = bus.read();
  if (key > 0) ringPush((uint8_t)key);
}

int cardKbReadKey() {
  if (s_tail == s_head) return 0;
  const uint8_t key = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) & 7u);
  return key;
}

bool cardKbPresent() {
  return s_present;
}

#endif
