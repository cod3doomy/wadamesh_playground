#include <Arduino.h>
#include "target.h"

// Set phase 1 when target.cpp globals start (no Serial - can crash before setup). Read g_boot_phase in setup().
namespace {
struct BootTrace {
  BootTrace() { set_boot_phase(1); }
} _boot_trace;
}

HeltecV4Board board;

#if defined(P_LORA_SCLK)
  #if defined(HELTEC_LORA_V4_R8)
    static SPIClass spi(HSPI);   // R8: LoRa on HSPI; the TFT + micro-SD own FSPI (SPI2)
  #elif defined(HELTEC_LORA_V4_ST7796)
    // ROOT CAUSE of "SPI3 MISO never reads GPIO18" (fixed 2026-10-16). Read this
    // before "simplifying" it back to a bare `SPIClass spi;`.
    //
    // On ESP32-S3, arduino-esp32 2.x defines the SPI host NUMBERS differently
    // from the classic ESP32 (verified by preprocessing this exact build:
    // HSPI == 1 and FSPI == 0, cores/esp32/esp32-hal-spi.h lines 28-30). And
    // `SPIClass`'s constructor default is `SPIClass(uint8_t spi_bus = HSPI)` --
    // NOT FSPI. So a bare `SPIClass spi;` on this board is host 1 == SPI3, the
    // very host TFT_eSPI takes with -D USE_HSPI_PORT=1 (also SPIClass(HSPI)).
    //
    // The previous comment here (and the one in platformio.ini under the
    // USE_HSPI_PORT flag) claimed the default ctor arg was FSPI == real SPI2.
    // It is not. Both objects therefore pointed at SPI3, and because
    // SPIClass::begin()'s `if (_spi) return;` guard is PER-OBJECT, not
    // PER-HOST, the radio's later `spi->begin(9, 11, 10)` (CustomSX1262::std_init,
    // reached from radio_init(), which runs AFTER display.begin() in main.cpp)
    // happily re-attached the matrix and executed
    //     spiAttachMISO(_spi, 11)  ->  pinMatrixInAttach(11, SPI3_Q_OUT_IDX)
    // stealing SPI3's MISO input away from the GPIO18 that TFT_eSPI's own
    // spi.begin(47, 18, 48, -1) had just attached. Result: SPI3 sampled MISO
    // from GPIO11 instead of GPIO18, so every SD read returned garbage/0x00
    // (sdSelectCard(): Select Failed) while the display readback through MISO
    // did the same.
    //
    // Fix: bind the radio to FSPI (SPI2) explicitly. SPI2 has no other user in
    // this build (nothing else touches the global `SPI` under
    // HELTEC_LORA_V4_ST7796), so SPI2 = LoRa only and SPI3 = ST7796 + TF slot,
    // which is what the docs have always claimed.
    static SPIClass spi(FSPI);   // LoRa on SPI2; the ST7796 + TF slot own SPI3
  #else
    static SPIClass spi;
  #endif
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
#else
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
#endif

WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
ClockFloorRTC        rtc_clock(fallback_clock);

#if ENV_INCLUDE_GPS
  #include <helpers/sensors/MicroNMEALocationProvider.h>
  MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
  EnvironmentSensorManager sensors = EnvironmentSensorManager(nmea);
#else
  EnvironmentSensorManager sensors;
#endif

#ifdef DISPLAY_CLASS
  #if defined(HELTEC_LORA_V4_R8)
    DISPLAY_CLASS display(&board.periph_power);   // LGFXDisplay claims Vext(40) on turnOn
  #else
    DISPLAY_CLASS display(NULL);
  #endif
  MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
#endif

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);

#if defined(HELTEC_LORA_V4_R8)
  // V4-R8 + Expansion Kit V2: bring up the shared TFT/SD bus on FSPI (SPI2) so
  // LovyanGFX (bus_shared=true) finds an already-begun bus and skips a double
  // spi_bus_initialize, and so the micro-SD mount (main.cpp, CS=3) can share it.
  // MISO=45 is needed for SD reads even though the ST7789 panel is write-only.
  #if PIN_TFT_LEDA_CTL >= 0
    pinMode(PIN_TFT_LEDA_CTL, OUTPUT);
    digitalWrite(PIN_TFT_LEDA_CTL, HIGH);
    delay(50);
  #endif
  SPI.begin(PIN_TFT_SCL, PIN_TFT_MISO, PIN_TFT_SDA, PIN_TFT_CS);
#endif

#if defined(P_LORA_SCLK)
  return radio.std_init(&spi);
#else
  return radio.std_init();
#endif
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}

#if defined(HELTEC_LORA_V4_R8)
SPIClass* heltecV4R8SharedSPI() {
  return &SPI;   // FSPI (SPI2), begun in radio_init() for the TFT + micro-SD
}
#endif

