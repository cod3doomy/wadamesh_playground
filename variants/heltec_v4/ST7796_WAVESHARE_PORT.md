# Heltec V4-R2 + Waveshare 3.5" capacitive-touch LCD — bring-up findings

**Status: WORKING.** Display, touch, radio RX and CardKB verified on real hardware
(2026-10-16). Touch is enabled and verified; SD is now enabled in firmware and builds,
but is **not yet proven on hardware** — see Phase 3. Two caveats are carried as open
items and **neither contradicts this status**:
Wi-Fi/BLE PHY double-init warning is unresolved with Wi-Fi/BLE itself disabled, and
companion-*web* reachability is **not** among the verified items. `ARDUINO_USB_MODE=1`
is a deliberate permanent setting, not an outstanding item - see open item 8.
Panel identity confirmed as Waveshare SKU **29318**, `3.5inch Capacitive Touch LCD`
(ST7796S + FT6336U + TF slot). Items still requiring hardware are marked **[HW]**.

**Read the "Hardware bring-up findings" section before touching anything here.** The
first two entries in this document — the "open heap corruption" blocker and the
`WADA_*` bring-up scaffolding around Wi-Fi/BLE — are both **closed and were wrong**.
The boot loop was never heap corruption; it was the panel reset line parked on the
ESP32-S3's PSRAM chip-select. Re-deriving that will cost another day. "Closed" for the
`WADA_*` work means only the specific toggles named below are gone — the bisect,
stack-profiling and boot-counter scaffolding is **still compiled in** and is simply
never enabled on this build.

**Target env: `heltec_v4_st7796_companion_radio_touch`** (`platformio.ini:253-557`).
The stock `heltec_v4_tft_companion_radio_usb_tcp_touch` env is **out of scope**.

⚠️ **This config is R2-only.** On the V4-R8 (octal PSRAM) GPIOs 33–37 are consumed by
PSRAM, and this env collides on **five** of them at once — `LCD_CS=33`, `GPS_EN=34`,
`EXPANSION_IO=35`, `VEXT=36`, `ADC_CTRL=37`. Because the collision is five pads wide,
the "override the R8 deltas" trick the `heltec_v4_r8` env uses to rescue its own
overlaps does **not** rescue this one: there is no small subset left to remap. Heltec
documents this as a hard incompatibility.

## Build environment prerequisite (Windows)

This repo **cannot build** unless `PLATFORMIO_CORE_DIR` points at a path with **no
spaces**. On a machine whose Windows username contains a space, PlatformIO expands
~212 include flags under that home directory, SCons passes them unquoted, and every
compile dies with:

```
xtensa-esp32s3-elf-g++: error: CreateProcess: No such file or directory
```

This is misleading — the compiler itself is fine and runs standalone. Ruled out along
the way: missing DLLs, platform version, `PATH`, parallel-job exhaustion (`-j 2` fails
identically), cold start, and the compiler binary itself. A minimal project builds fine
from a path *with* a space; it is the combination of a space **and** this project's 212
spaced include paths that breaks it.

⇒ Fix applied on this machine:

```
mklink /J C:\pio "C:\Users\Cody D\.platformio"
setx PLATFORMIO_CORE_DIR C:\pio
```

The junction reuses the existing packages, so nothing re-downloads. Applies to newly
launched shells only.

## Hardware bring-up findings (2026-10-14/15, all MEASURED)

### Root cause #1 — the boot loop: `TFT_RST=26` was the PSRAM chip-select

**This was the whole thing.** The original diagnosis in this document was "heap
corruption in TLSF, victim moves between runs". That was wrong, and it sent the
bring-up down several dead ends (`WADA_NO_WIFI_BRINGUP`, `WADA_NO_BLE_BRINGUP`, the
RX-log no-PSRAM toggle, `WADA_DISP_NO_PUSH`, `WADA_REBOOT_DIAG`).

⇒ Those **named** toggles are genuinely gone from the source — grep `main.cpp` for any
of them and you get nothing. What is **not** gone is the scaffolding around them:
`WADA_BISECT` (37 call sites in `main.cpp`), `WADA_STK`, `WADA_BC_REPORT` and
`WADA_WDT_GRACE_SEC` are all still defined and still compile in. They are inert — the
`#ifndef` fallbacks turn each into `do { } while (0)` unless the flag is set at build
time — but "all `WADA_*` removed" is **false** as stated. Treat this section as: the
Wi-Fi/BLE/PSRAM/diagnostic toggles are gone; the bisect, stack-profiling and
boot-counter instrumentation remains.

On the ESP32-S3 in the Heltec V4-R2, **GPIO26 is the PSRAM chip-select**. The SDK's
`sdkconfig` for this target has `CONFIG_SPIRAM_CS_IO=26`. Our pin budget had parked
the panel reset on 26, and TFT_eSPI's `init()` does:

```cpp
pinMode(26, OUTPUT);      // detaches the pad from SPICS in the GPIO matrix
digitalWrite(26, 0);      // ...and holds CE# low
delay(20);                // ...for 20 ms
```

Once `pinMode` runs, the pad leaves the SPI peripheral's view of the world — the
matrix swap that made it SPICS is gone, and nothing else drives it. For 20 ms PSRAM
had **no chip-select at all** and could not frame its own accesses. It is not a
transient scribble on a scratch buffer: PSRAM's free-list metadata lives *in* PSRAM,
so it was being corrupted on every boot, hard.

This is why **the crash victim appeared to move between runs**. Whichever allocation
next walked the free list died — `DataStore::begin()` → `VFSFileImpl` → `setvbuf` →
`malloc` this time, `MyMesh::begin()` → `loadPrefsInt` the next. A moving victim is the
signature of corrupted metadata, not of a specific buffer overflow, and it is exactly
what the "classic heap-corruption" note in `main.cpp` describes. It is also why the
`WADA_*` toggles appeared to *move* the crash: they changed allocation order, not
heap health.

⇒ **Fix: `TFT_RST=3`** (`PIN_TFT_RST=3` in the env), a free pin on this build.

**The evidence that killed the heap-corruption theory** — TFT_eSPI's ESP32-S3 pixel
path (`pushColors` → `pushPixels`) writes *only* to the SPI peripheral's 16-word
hardware FIFO via `WRITE_PERI_REG`. No `_buf`, no `malloc`, no DMA, no buffer of any
kind to overrun. There was no overflow to find.

### Root cause #2 — garbled UI: SPI clock marginality over the dupont jumpers

Distinct fault, different fix, same debugging session. MEASURED on the same board,
over dupont jumper leads:

| `SPI_FREQUENCY` | Result |
|---|---|
| 40 MHz | **Clean 8/8** from a freshly erased flash, post-cleanup. **CURRENT VALUE** |
| 20 MHz | **Clean 8/8** from a freshly erased flash, animated background enabled |
| 8 MHz | Clean 8/8 |
| 40 MHz (earlier session) | **Garbled** - 4 of the first boots after every flash erase |

`SPI_FREQUENCY=40000000` is the current value, verified over 8+ reboots with the full
UI, touch, CardKB and LoRa RX all live simultaneously.

**The "40 MHz garbles the display" row is RETRACTED as a causal claim.** It was inferred,
not proven. The bad episode and the good ones differed in **three variables at once** -
SPI clock, animated background on/off, and the `s_ui_rotation` pin - so the clock was
never isolated as the cause. Every measurement after the aurora/rotation cleanup came
back clean at 40 MHz.

One tell remains genuinely unexplained and is the strongest clue left: a 40 MHz splash
took **4440 ms** against **3811 ms** at 8 MHz, i.e. *slower* at the higher clock. That is
not what a marginal link does, and it is why the timing theory was never accepted.
**Treat the garbling cause as OPEN**, not as clock marginality.

Very likely a **wiring** limit, not a chip limit - or possibly not a digital-timing
fault at all. Short, hard-soldered display leads should permit considerably more.
**Re-test the clock after rework.**

**Test protocol this investigation proved necessary (it was fooled twice without it):**
erase the flash **first**, then run 8+ reboots. The fault had a persistence effect - bad
on the early boots, then good and staying good - so without an erase the result is a
false pass.

### Other findings

| Area | Finding |
|---|---|
| **Build environment** | This repo cannot build when the Windows username contains a space: PlatformIO expands ~212 include flags under that home dir, SCons passes them unquoted, and every compile dies with `CreateProcess: No such file or directory`. The compiler itself is fine. Fixed with a `C:\pio` junction to `%USERPROFILE%\.platformio` plus `setx PLATFORMIO_CORE_DIR C:\pio`. Affects every PlatformIO project on the machine. |
| **Flash headroom** | Measured **3,976,000 / 6,160,384 = 64.5%** of flash, ~2.1 MB free (post-touch build). The earlier **3,960,985 = 64.3%** figure was the *pre-touch* build and is stale. The `99.7%` claim in the env's flash-headroom comment in `platformio.ini` was **stale** too (written before `partitions_st7796.csv` widened the app slots). Corrected. |
| **`TFT_MISO`** | **Now `18`, was `-1`.** `-1` is correct *only* for a display register readback on a **4-wire** panel, where reads fail closed and a readback returning `COLMOD=0xFF` is the tell — any "SPI readback" evidence collected through MISO under `-1` is meaningless. But `-1` makes the TF card **deterministically unreadable** (see 0.1: SD's argument-less `spi.begin()` is a no-op behind TFT_eSPI's, and `spiAttachMISO(_spi,-1)` attaches nothing on the S3, so GPIO18 never reaches SPI3 — and every SD init command is a read). **DONE**, see Phase 3 step 2. |
| **Display geometry** | Declare the panel at its **NATIVE `TFT_WIDTH=320` / `TFT_HEIGHT=480`** and call **`setRotation(1)`**. Declaring it landscape and hand-writing MADCTL is wrong: TFT_eSPI caches its rotation state and desynchronises from the register, so the address window, the driver's assumed dimensions and the hardware all disagree. `setRotation()` keeps the cache and the register in step. |
| **UI rotation pinning** | `s_ui_rotation` must be pinned to **`LV_DISP_ROT_90`** in `UITask.cpp`. It is a **persisted pref** that *overrides* `DEFAULT_UI_ROTATION`, and `device_caps.h` sets `CAP_ROTATABLE 0` for this target, so the UI offers no way to change it back. An unpinned value therefore sticks forever. |
| **Pin map** | Verified by I2C scan: CardKB **ACK 0x5F** on GPIO15/16 and answers `requestFrom`; backlight responds on GPIO6. Panel touch bus is **0x38** on GPIO38/4 and is **live** — the FT6336U is driven and taps verified on it (see the overvoltage section: that issue is resolved). |
| **GPIO39 hidden pull-up** | `PIN_TOUCH_INT` is **undefined** in this env, so `RakTapV2Touch.cpp` defaults it to **39** and calls `pinMode(39, INPUT_PULLUP)` on every boot. GPIO39 is **`PIN_GPS_TX`**. So a pad the reserved-pin analysis below does not list gets an internal pull-up at boot. Harmless while the GNSS header is unpopulated, but GPIO39 is **not** an unused pad — count it in the pin inventory. |
| **CardKB bus** | Needs its **own I2C peripheral on GPIO15/16**, not a second `TwoWire` alias onto the board bus. Beyond the TXS0108E contention argument already recorded below: **polling the panel's touch line for an absent device wedged SCL and stalled the UI for 525 ms every second.** An isolated bus cannot do that. |
| **GPIO38 collision** | `PIN_GPS_RX=38` and `PIN_BOARD_SDA=38` claim the same pad. `EnvironmentSensorManager::begin()` calls `initBasicGPS()` first and takes GPIO38. **GPS cannot coexist with the panel's touch bus on this board** — mutually exclusive, not a bring-up-only setting. |
| **CardKB ACKing** | The repo comment claiming the CardKB "never ACKs a bare address probe" is **false** — it ACKs. `requestFrom` is still correct and is kept. |

### RESOLVED IN HARDWARE — the panel I2C pull-ups overvoltage

**Resolved. It was why touch and SD were disabled; touch has since been enabled and
verified, and `CAP_SD` has since been flipped to 1 (firmware builds; hardware proof
outstanding — see Phase 3).** The original fault: the panel's
I2C pull-ups are referenced to **VCC, not 3V3**. With VCC on 5 V this drove **GPIO38 to
4.23 V**, against the ESP32-S3's **3.6 V absolute maximum**. ESP32-S3 GPIOs are not
5V-tolerant.

Both `CAP_TOUCH` and `CAP_SD` in `src/ui-touch/device_caps.h` were originally held at 0
as a deliberate voltage safety hold. **Touch has since been enabled and verified**
(`CAP_TOUCH=1`, commit `08f7fda`); `CAP_SD` has since been enabled too (`CAP_SD=1`,
2026-10-16) and is now only ordinary bring-up risk, not an electrical hazard. The hold was lifted in hardware: the CardKB was moved to
3.3 V, so all SDA/SCL now measure 3.3 V against the 3.6 V absolute maximum. The touch
controller was present and ACKing at `0x38` the whole time - it was disabled purely on
voltage grounds, and that ground no longer applies.

Before enabling SD, re-measure GPIO38 at idle *with the bus loaded* rather than assuming
it is fixed. The panel's `3V3` pin is a *regulated output* - do **not** tie it to the
V4's 3.3 V rail.

### Firmware bugs found and fixed on the way

1. `main.cpp` (`heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` call) — **panics when PSRAM is present**; its own `if (psram_bytes)` guard passes precisely in the failing case.
2. `main.cpp` (the `esp_spiffs_init` "last resort format" fallback) — **defeated the SPIFFS-skip diagnostic**, so every boot died in the `esp_spiffs_init` path the diagnostic existed to avoid.
3. `GPIO17` (`PIN_SD_CS`) was **unconfigured and floating** while the display drove SPI3; now parked HIGH before display init.
4. Fourteen `ENV_INCLUDE_*` sensor drivers compiled against a bus holding **exactly one device**.
5. **`loopTask`'s default 8 KB stack was exhausted** (free stack 0 of 8192). Fixed with `ARDUINO_LOOP_STACK_SIZE=16384`. **This is a real bug and worth keeping — but it was NOT the boot loop.** `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` is off on this target, so that stack could never reach the corrupted PSRAM and could not have been the victim. **This is probably not Waveshare-specific** either: other boards run the same heavy `setup()` on 8 KB.
6. The Arduino-core Wi-Fi event-vector race (`WiFi.onEvent()` / `WiFi.mode(WIFI_STA)` push_back vs. `_arduino_event_task` walk) is a genuine core bug with a V4-R8 dump in-repo — but it was **not** what was crashing this build. Both transports run normally now; the
Wi-Fi/BLE bring-up gates that suppressed them are gone. (This does **not** imply the
whole `WADA_*` namespace was deleted — see root cause #1.)

### Bring-up workflow notes

- **Flashing requires manual download mode** once the board is stable: hold **BOOT** -> tap **RESET** -> release **BOOT**, then `pio run -e <env> -t upload --upload-port <COM>`, then tap **RESET** to run. PlatformIO's "Hard resetting via RTS pin" leaves the ESP32-S3 in ROM download mode (`boot:0x20`) and never starts the app.
- `pio` is **not on PATH**; it lives at `%USERPROFILE%\.platformio\penv\Scripts\pio.exe` (now added to the user PATH).
- `Win32_SerialPort` goes stale and omits the native-USB COM port -- use `GetPnpDevice` instead.
- `ARDUINO_USB_MODE=1` is a **deliberate permanent** setting for this env, not
  scaffolding. TinyUSB (`=0`, the board JSON default) enumerated as `PID_0002` and the
  boot log was not readable, making every fault blind; `=1` gives `PID_1001` "USB JTAG/
  serial debug unit" and reads reliably. **Do not revert.** Caveat: the mode-0 finding
  came from an async capture known to drop bytes, so it is suggestive rather than
  proven; prior TinyUSB trouble on this host is the stronger reason. See the fuller
  note in `platformio.ini`.

| Item | Existing state |
|---|---|
| LCD driver | **DONE** — `variants/heltec_v4/St7796WaveshareDisplay.{h,cpp}`, MEASURED |
| Touch driver | **DONE** - `src/helpers/input/RakTapV2Touch.cpp`, FT5x06 at `0x38`. Re-targeted, not written; tap placement verified on hardware |
| SD support | **CODE DONE, NOT YET PROVEN [HW]** - `mapSdStorage()` + `fmIsSd()` ship on 8 other boards; `HELTEC_LORA_V4_ST7796` has now been added to that block's gate with its own `TFT_eSPI::getSPIinstance()` handle, alongside `TFT_MISO=18` and `CAP_SD=1`. Build verified 2026-10-16; the card round trip is still outstanding — see Phase 3 |
| CardKB | **DONE** - `src/helpers/input/CardKbKeyboard.cpp`, 73 lines, protocol verified |

⇒ The project is porting and wiring, not development.

## Two decisions changed by research

**1. Touch and CardKB go on separate I2C peripherals, not one shared bus.**
The panel carries an onboard **TXS0108E** translator and **four 4.7 kΩ I²C pull-ups**,
split across both sides. TI does not recommend that part for open-drain buses with
competing external pull-ups — the classic symptom being *"works until I plug the
keyboard in."* Sharing would also invite NACK cross-talk, since the FT6336U closes its
I2C port in Monitor mode and would be answering alongside a device that must never miss
a keystroke. The ESP32-S3 has two I2C peripherals and the user has GPIO15/16 free, so
the CardKB gets a bus to itself. Touch stays at 100 kHz on the board bus with the
sensors — that costs the touch controller its optional 400 kHz headroom, which the UI
does not need at this poll rate.

**MEASURED afterwards, and decisive:** polling the panel's touch line for a device that
is not there **wedged SCL and stalled the UI for 525 ms every second**. Not a marginal
NACK — a hard stall on the shared bus. The split bus is what keeps the CardKB pollable
while the touch bus is unusable.

**2. Vext must be asserted, and the header 3V3 is a switched rail.** On the Heltec V4
the header `Ve` pins are switched Vext, not a straight LDO. The panel draws ~63 mA plus
backlight — feeding it from a switched rail browns out unless firmware enables Vext.

## Deliberately NOT done

- **No** log/message persistence to SD (mount-and-prove only, per user decision).
- **No** SD map-tile backend. `CAP_FILESYSTEM` stays `0`.
- **No** stock-TFT-env validation.
- **No** `TP_INT` / `TP_RST` wiring — the driver polls and nothing drives reset.
- **No** CardKB firmware flashing (M5Stack publishes no ATmega8A hex).

---

## Phase 0 — Wiring and flash baseline

### 0.1 Panel to V4-R2

| Panel pin | Signal | GPIO | Note |
|---|---|---|---|
| 1 | VCC | **5V** | silkscreen: 3.3V/5V in |
| 2 | 3V3 | **3V3** | **mandatory** — powers FT6336U + its pull-ups |
| 3 | GND | **GND** | |
| 5 | MOSI | **48** | SPI3 |
| 6 | SCLK | **47** | SPI3, **20 MHz** MEASURED |
| 4 | MISO | **18** | ✅ **CONFIRMED BY MEASUREMENT** (user, 2026-10-16) — the TF slot's MISO lands on GPIO18. Needed for SD; see 0.1 |
| 8 | LCD_CS | **33** | R2 only — one of five pads lost to R8 PSRAM |
| 9 | LCD_DC | **21** | Heltec `OLED RST` — harmless, no OLED fitted |
| 10 | LCD_RST | **3** | ⚠️ **NOT 26.** 26 is the PSRAM chip-select — see root cause #1 |
| 11 | LCD_BL | **6** | active-high PWM, MEASURED |
| 7 | SD_CS | **17** | **DONE** — in the env, and parked HIGH before display init |
| 12 | TP_SDA | **38** | I2C1, touch — live and verified; overvoltage now resolved |
| 13 | TP_SCL | **4** | I2C1, touch — live and verified; overvoltage now resolved |
| 14 | TP_INT | *unconnected* | driver polls |
| 15 | TP_RST | *unconnected* | power-on reset |

⚠️ **Identify header signals by the connector silkscreen order, not by a pin number quoted
elsewhere.** The 15-pin GH1.25 interface header, in physical order, is: 1 VCC, 2 3V3,
3 GND, **4 MISO**, 5 MOSI, 6 SCLK, **7 SD_CS**, 8 LCD_CS, 9 LCD_D/C, 10 LCD_RST, 11 LCD_BL,
12 TP_SDA, 13 TP_SCL, 14 TP_INT, 15 TP_RST. A research pass during the 2026-10-16 schematic
session reported MISO as *"pin 6"*, which contradicts both the schematic and the
silkscreen — that report is wrong. **MISO is the 4th signal.**

**GPIO39 is also claimed, invisibly.** `PIN_TOUCH_INT` is undefined in this env, so
`RakTapV2Touch.cpp` falls back to **39** and calls `pinMode(39, INPUT_PULLUP)` at
driver init. GPIO39 is `PIN_GPS_TX`. It is not in the panel's FPC and not in the table
above, but it is a pad this build actively reconfigures — see the findings table.

`17`/`18` confirmed as header pins by the user; they are unpopulated OLED pins per
`variants/heltec_v4/pins_arduino.h:60-61`. ⇒ Safe to reuse, and GPIO21 working as
`LCD_DC` independently proves no OLED is fitted. That observation alone only proved the
*header pins are free* — it did **not** prove the TF slot's MISO reaches GPIO18. That
separate claim is now closed: the user has since **CONFIRMED GPIO18 by measurement**
(2026-10-16). See below.

**MISO is two separate questions, and conflating them is what broke Phase 3.**
For the **display register readback**, the panel is effectively 4-wire write-only, so
`TFT_MISO=-1` is correct: reads fail closed rather than returning garbage, and a
readback returning `COLMOD=0xFF` is the tell. An earlier "SPI readback confirms…"
conclusion drawn through MISO was therefore unfounded.

For the **TF card**, `TFT_MISO=-1` is not merely inconvenient — it is fatal. `SDFS::begin()`
calls `spi.begin()` with **no arguments**; `SPIClass::begin()` opens with
`if(_spi) { return; }`, and TFT_eSPI has already run `spi.begin(47,-1,48,33)` during
display init, so SD's call is a **complete no-op**. On ESP32-S3 the resulting
`spiAttachMISO(_spi, -1)` hits the *"HSPI Does not have default pins on ESP32S3!"* branch
and attaches nothing: **GPIO18 is never routed into SPI3 at all.** Every SD init command
is a READ, so the mount fails **100% of the time** — not sometimes, not on some cards.

⇒ **`TFT_MISO=18` is required for SD.** The env's own comment block already describes
this correctly; it is this document that discarded it. One flag serves both uses: the
register readback starts working *and* the card becomes readable. Phase 3 step 1 owns
the detail.

### ⚠️ `TFT_MISO=18` alone was NOT enough — SPI3's MISO was being stolen (2026-10-16)

With `TFT_MISO=18` in place the card still failed (`sdSelectCard(): Select Failed`, raw
CMD0 R1 = `0x00`, and the `[MISOLOOP]` diagnostic returned `0x00` no matter what GPIO18
was driven to). The cause was **not** the flag, and **not** `SPIClass::begin()`'s
`if(_spi){return;}` short-circuit:

* On ESP32-S3, arduino-esp32 2.x numbers the hosts **`HSPI = 1` (SPI3) and `FSPI = 0`
  (SPI2)** — the opposite ordering to the classic ESP32. Verified by preprocessing this
  env (`#pragma message`), and `cores/esp32/esp32-hal-spi.h:28-30` confirms it.
* `SPIClass`'s constructor default is **`SPIClass(uint8_t spi_bus = HSPI)`**, *not*
  `FSPI`. So `static SPIClass spi;` in `target.cpp` — documented here and in
  `platformio.ini` as "the default ctor arg = FSPI = real SPI2" — was in fact **SPI3**,
  the same host `-D USE_HSPI_PORT=1` gives TFT_eSPI.
* `SPIClass::begin()`'s `if(_spi){return;}` guard is **per OBJECT, not per HOST**. So the
  radio's own `spi->begin(9, 11, 10)` (inside `CustomSX1262::std_init`, reached from
  `radio_init()`, which runs *after* `display.begin()` in `setup()`) did **not**
  short-circuit — it ran `spiAttachMISO(_spi, 11)` and executed
  `pinMatrixInAttach(11, SPI3_Q_OUT_IDX)`, overwriting the `in1[]` slot that
  `pinMatrixInAttach(18, SPI3_Q_OUT_IDX)` had just set. **SPI3 went on sampling GPIO11.**
* The GPIO matrix raises no error for this: it is "last writer wins" per signal, so the
  panel and SD silently inherited the radio's MISO pin.

⇒ **Fixed (2026-10-16):** `target.cpp` now constructs the radio's bus as
`SPIClass(FSPI)` for `HELTEC_LORA_V4_ST7796` (LoRa owns SPI2 alone; SPI3 is exclusively
the ST7796 + TF slot, as this document always claimed), and
`St7796WaveshareDisplay::begin()` additionally re-asserts
`spiAttachMISO(TFT_eSPI::getSPIinstance().bus(), TFT_MISO)` straight after `display.init()`.
Neither `SPI_FREQUENCY`, `DISPLAY_ROTATION`, `PIN_TFT_RST`, `PIN_SD_CS`, `TFT_MISO`,
`CAP_TOUCH` nor `CAP_SD` changed.

⚠️ **GPIO18 is no longer an unproven precondition — it is CONFIRMED (2026-10-16).**
The claim that the TF slot's MISO lands on GPIO18 came from a comment transcribing the
module schematic; the user has now **measured it and confirmed GPIO18**. GPIO18 still
appears in no wiring-table row of the original bring-up notes, so the firmware side is
where it had to be made real: `TFT_MISO=18` in the env (changed from `-1`), the
`HELTEC_LORA_V4_ST7796` gate in `main.cpp`, and `CAP_SD=1`.

The **card-side** half is now also read directly off the vendor schematic: socket pin 7
(D0) reaches the `MISO` net by direct trace, with no series component and nothing in
between. That routing is real but is **not** continuity-testable from an empty socket — see
the TF socket subsection in Phase 3.

### 0.2 CardKB — separate bus, and the pull-up decision **[HW]**

| CardKB HY2.0-4P | Colour | → Heltec |
|---|---|---|
| 1 | Black | **GND** |
| 2 | Red | **5V** (or 3V3 — see below) |
| 3 | Yellow | **GPIO15** (new — own bus, via I2C1) |
| 4 | White | **GPIO16** (new — own bus, via I2C1) |

⚠️ **Do NOT repoint `PIN_BOARD_SDA`/`PIN_BOARD_SCL` at 15/16.** Those flags also carry
every environment sensor (`ENV_INCLUDE_AHTX0`, `BME280`, `SHTC3`, `SHT4X`, `INA3221`, … —
the `ENV_INCLUDE_*` flag block in this env), and the sensors are physically wired to
38/4. Moving the board bus silently disconnects all of them.

⇒ CardKB needs **dedicated** flags (`PIN_CARDKB_SDA`/`PIN_CARDKB_SCL`) and its own
`TwoWire`. **This is DONE — not an outstanding item.** `PIN_CARDKB_SDA=15` /
`PIN_CARDKB_SCL=16` are set in the env, and `CardKbKeyboard.cpp` already declares its own
`TwoWire s_cardkb_bus(1)` and calls `s_cardkb_bus.begin(PIN_CARDKB_SDA, PIN_CARDKB_SCL,
100000)`; the bare `Wire` fallback survives only in an `#else` for legacy boards with no
dedicated pins. Both landed **before this branch's HEAD**.

The CardKB's ATmega8A enables its **own internal I2C pull-ups** (20–50 kΩ), pulling
toward its own VCC. ESP32-S3 GPIOs are not 5V-tolerant (abs max 3.6V). Bus high level is
`V = 3.3 + 1.7·R/(R + R_internal)`:

| External pull-up to 3V3 | R_int = 50 kΩ | R_int = 20 kΩ |
|---|---|---|
| 4.7 kΩ | 3.45V | **3.62V — over abs max** |
| 10 kΩ | 3.60V | **3.76V — over abs max** |

The commonly-cited "4.7–10 kΩ to 3V3" is **unsafe on its own**. Options:

- **Power CardKB from 3V3** (try first) ⇒ bus safe by construction. Risk: ATmega8A on an
  internal 8 MHz oscillator is out-of-spec at 3.3V, which plausibly explains M5Stack's
  one forum report of garbage I2C at 3V.
- **5V + ~1.8 kΩ to 3V3** ⇒ bus lands 3.37–3.45V, inside abs max. The ≥2 kΩ "too strong
  to sink" warning does not apply: 3.45V/1.5kΩ ≈ 2.3 mA < 3 mA spec.
- **PCA9306 level shifter** ⇒ removes the question.

⇒ Because CardKB is now on its own bus, **fit the external pull-ups explicitly**. Do not
rely on the panel's — they are on the other side of the translator now.

### 0.3 GPIO38 collision — resolve before power-up **[HW]**

`PIN_GPS_RX=38` and `PIN_BOARD_SDA=38` (both build flags in this env) name the same
pad. Today the collision is held off by `ENV_INCLUDE_GPS=0` — with it at 1,
`EnvironmentSensorManager::begin()` takes GPIO38 for `initBasicGPS()` and the touch bus
dies. Confirm the GNSS header is unpopulated before anyone flips that flag back on.
Note the V4's GNSS connector spans GPIO38–42, so it also sits under `PIN_GPS_TX=39`
(which the touch driver additionally pull-ups — see the findings table),
`PIN_GPS_RESET=42` and `PIN_GPS_EN=34`.

### 0.4 Flash decision — data-loss gate

`partitions_st7796.csv` **moves the `tiles` and `spiffs` offsets**. Its header warns:
*"Safe on a freshly erased device, but it orphans any existing map tile pack and spiffs
user data."*

`README.md:100-110` requires the NVS-preserving 4-component chain
(bootloader `0x0` / partitions `0x8000` / boot_app0 `0xe000` / app `0x10000`) and warns
merged images 0xFF-pad and wipe NVS.

⇒ Decide deliberately: accept losing Wi-Fi credentials for a clean erase, or re-enter
them afterwards. Do not assume the table swaps cleanly.

---

## Phase 1 — Verify the stock build flashes and boots (item 1)

1. `pio run -e heltec_v4_st7796_companion_radio_touch`
2. Flash per 0.4, then `scripts/build-upload-monitor.sh` (prints ELF/BIN SHA256).
3. Confirm on serial: correct partition table, NVS intact, **Vext asserted**, radio init,
   UI renders.

**Acceptance:** LCD shows the UI, radio enumerates, no boot errors.

**Watch:** measured headroom is comfortable — see Phase 2 step 11.

---

## Phase 2 — Port the FT6336U touch driver (item 2)

Driver exists: `src/helpers/input/RakTapV2Touch.cpp`, same controller family, same I2C
address `0x38`, already implementing the full 13-function `HeltecV4CapTouch` contract
(`src/helpers/input/HeltecV4CapTouch.h`) that LVGL consumes via `lvglTouchRead`
(`src/ui-touch/UITask.cpp:7543`).

1. **Re-gate the driver.** It is gated on `HAS_RAK_TAP_V2`, which would drag in RAK
   board/radio code. Introduce a neutral `HAS_FT5X06_TOUCH`, add it to `TDeckTouch.cpp`'s
   exclusion list, and drop `TDeckTouch.cpp` from this env's `build_src_filter` block in
   `platformio.ini` once only the FT5x06 driver compiles — otherwise two files define
   the same symbols.
2. **Re-target geometry.** `RAW_W` / `RAW_H` in `RakTapV2Touch.cpp` must become the panel's
   native **320x480 portrait**, then map to 480x320 landscape. The FT6336U reports in
   native portrait with the **y origin at the FPC end**, and the ST7796 has no rotation
   register - so this is 100% host-side math and is the one genuinely novel code in the
   project. Note the rotation is `DISPLAY_ROTATION=1`, not 2: on this natively portrait
   panel, 0/2 are portrait and 1/3 are landscape.
   `RAK_TS_FLIP_X`/`RAK_TS_FLIP_Y` do carry over, and **both were needed**: with geometry
   corrected, the top of the UI selected the bottom (`RAK_TS_FLIP_Y`) and X was still
   mirrored - together a 180 degree rotation (`RAK_TS_FLIP_X`). The wrong geometry
   *masked* the rotation, so the order was not optional.
3. **Point at the board bus** - SDA=38 / SCL=4 on **I2C0 (`Wire`)**, alongside the
   environment sensors. **DONE** (commit `08f7fda`): the FT6336U initialises at `0x38` on
   the first attempt and taps land where they are pressed. The GPIO38 overvoltage that put
   this on hold is resolved (CardKB on 3.3 V). The split-bus decision below means the
   driver uses `Wire` for touch only and does **not** re-route it, so the sensors are
   unaffected - that part of the original caution still holds and should stay true.
4. **Init timing - the chip NACKs for ~300 ms after reset** (`Trsi`, datasheet Table 3-5).
   The 3-attempt retry in `RakTapV2Touch.cpp` is adequate in practice - it succeeded on
   attempt 0 on every boot - but a cold boot may need the Waveshare sequence: 200 ms low,
   then ~300 ms before first I2C. Do **not** copy Waveshare's ESP-IDF component, which
   uses 10/10 ms - that only works because their carrier board is already warm.
5. **Monitor mode.** `CTRL(0x86)=0x01` by default auto-drops the chip to Monitor after
   ~10 idle units, and *"during this mode the serial port is closed."* Write
   `0xA5 PWR_MODE = 0x01` (Active) before any read, or expect NACKs/garbage at `0x38`.
6. **Validate identity:** `CIPHER(0xA3) == 0x64` identifies FT6236U/FT6336U, and
   `FOCALTECH_ID(0xA8) == 0x11`. Cheap way to prove you are talking to the right chip.
7. **Set `x_max/y_max = 320/480`.** LovyanGFX's `Touch_FT5x06` defaults to a square 319 —
   wrong for this panel.
8. **Copy LovyanGFX's double-read hardening.** It re-reads and compares up to 5× because
   *"values can change during readout"* — single reads can return torn X/Y pairs.
9. **Leave INT alone.** M5Stack's own experience: an INT-low only means the chip has
   data, not that a transaction is safe on a shared bus — and we no longer share it, so
   there is no reason to use INT at all.
10. **Flip the capability:** `CAP_TOUCH` 0 -> 1 at `#define CAP_TOUCH` in the ST7796 block
    of `src/ui-touch/device_caps.h`, and
    retire the no-op `variants/heltec_v4/St7796NoTouch.cpp`. **DONE** (commit `08f7fda`),
    and both reasons this was originally held are now gone: the GPIO38 overvoltage was
    fixed in hardware (CardKB moved to 3.3 V, so all SDA/SCL now measure 3.3 V against a
    3.6 V absolute maximum -- driving the FT6336U at 4.23 V was genuinely unsafe), and
    the shared-bus conflict was fixed by the dedicated CardKB bus on GPIO15/16.
    The FT5x06 driver already existed at `0x38`, gated to `HAS_RAK_TAP_V2`; the earlier
    belief that the repo had no FT5x06 driver was **wrong** -- it needed re-targeting,
    exactly as the status table predicted. `St7796NoTouch.cpp` is *suppressed*, not
    deleted: `-D ST7796_TOUCH_FT5X06=1` selects the real driver and unsetting the flag
    returns to a keyboard-only UI. Both providers define the same 13-function contract,
    so they cannot coexist at link time.

    **Two real defects, fixed in this order -- the order matters.**
    - *Geometry.* `RAW_W/RAW_H/SCR_W/SCR_H` were hardcoded to the RAK WisMesh 240x320.
      This panel is 320x480 raw portrait with 480x320 landscape output, so every
      coordinate was clamped into a 320x240 box. Now guarded by
      `#if defined(HELTEC_LORA_V4_ST7796)` with the RAK values in the `#else`.
    - *Rotation.* With geometry correct, the top of the UI selected the bottom
      (`RAK_TS_FLIP_Y`), and X was still mirrored -- together a 180 degree rotation
      (`RAK_TS_FLIP_X`).
    The wrong geometry **masked** the rotation, so the rotation was only diagnosable
    once the dimensions were right. Fixing them in the other order would have sent
    you chasing a rotation fault that does not exist yet.
11. **Flash budget — measured, and a non-issue.** A successful post-touch build reports
    **3,976,000 of 6,160,384 bytes = 64.5%**, leaving ~2.1 MB. The earlier
    **3,960,985 = 64.3%** reading was the **pre-touch** build and is stale — quoted here
    only so the two are not confused. The app0 slot in `partitions_st7796.csv` is
    `0x010000`–`0x5F0000` = 6,160,384 bytes, matching the reported maximum exactly. RAM is
    164,692 of 2,097,152 = 7.9%.

    ⚠️ The flash-headroom comment at the top of the env block in `platformio.ini`,
    claiming this env sits at **99.7%** and "leaves no room for the FT6336U touch
    driver", was **stale** — written against the smaller app slots before
    `partitions_st7796.csv` widened them. Do not trim `deploy/apps` or shrink `spiffs`
    on the strength of any of these figures.

**Colour order note:** Waveshare's two official demos **disagree** — the ESP-IDF BSP
forces BGR, the Arduino demo passes RGB. The repo's `TFT_RGB_ORDER=TFT_RGB` +
`TFT_INVERSION_ON=1` is MEASURED working and supersedes both. Don't change it on the
strength of the datasheets.

**Acceptance [HW]: MET (2026-10-16).** The FT6336U is detected at `0x38` on the first
attempt (`[TOUCH] 0x38 ft5x06 OK (retry 0)`), and taps land exactly where they are pressed.

Note the rotation here is `DISPLAY_ROTATION=1`, **not 2** — on this natively portrait
320x480 panel, rotations 0/2 are portrait and 1/3 are landscape, and 1 == MADCTL 0x20 is
MEASURED correct. `s_ui_rotation` is also pinned to `LV_DISP_ROT_90`, because a persisted
preference otherwise overrides `DEFAULT_UI_ROTATION` and `CAP_ROTATABLE=0` means the UI
cannot change it back. Both pins are load-bearing for the touch mapping above; if either is
removed, the flips must be re-derived.

---

## Phase 3 — SD mount and prove (item 3)

Scope is mount-and-prove only.

**Bus topology is confirmed:** the schematic shows the TF slot wired straight to the
same `MOSI`/`SCLK`/`MISO` nets as the ST7796, differing only by CS. It is *not* a second
SPI peripheral. DAT1/DAT2 are unconnected, so **4-bit SDIO is physically impossible**.

⇒ Use **`SD.h` over the display's own `SPIClass`**, obtained from
**`TFT_eSPI::getSPIinstance()`**. That is the same object the ST7796 drives and the same
one TFT_eSPI's own SD example passes to `SD.begin()`; there is no `fspi` symbol in this
repo and an earlier revision of this document told the reader to pass one. Do **not**
use `SD_MMC`.

⚠️ **The reasoning this document gave for the `SD_MMC` ban was wrong; the conclusion is
right.** It said `SD_MMC` *"does not apply"* on the grounds that it is a *different board*
(the *ESP32-S3-Touch-LCD-3.5*). The correct statement is the one already given above the
ban: `SD_MMC` is impossible **here** because DAT1/DAT2 are unconnected on this module's
TF socket — a hardware reason, not a board-identity one. See the TF socket subsection below.

**This is also what the vendor does.** Waveshare's demo for this module uses **SPI-mode SD
over the shared bus**, not `SD_MMC`: `fspi.begin(SCLK, MISO, MOSI, -1)` then
`SD.begin(SD_CS, fspi)`, with their own comment that SD init must come after LCD init.
`LCD_CS` and `SD_CS` are separate host pins.

⚠️ **Three things below are prerequisites, not steps to discover the hard way.** Do them
in this order or you will burn a hardware session.

1. ~~**Confirm GPIO18 actually reaches the TF slot's MISO.**~~ **[HW] RESOLVED
   (2026-10-16)** — the user CONFIRMED GPIO18 **by measurement**. The firmware side
   that this unblocked is **DONE**: `TFT_MISO=18` (was `-1`), `HELTEC_LORA_V4_ST7796`
   added to the Arduino-SD gate in `main.cpp` with a `TFT_eSPI::getSPIinstance()`
   handle, and `CAP_SD=1`. See steps 2-3 below and open item 1.
2. ~~**Set `TFT_MISO=18`**~~ **DONE.** With `TFT_MISO=-1` the card is
   not "harder to read" — it is **deterministically unreadable**, as traced in 0.1:
   SD's argument-less `spi.begin()` is a no-op, nothing routes GPIO18 into SPI3, and
   every init command is a read. The env's own comment already said `TFT_MISO=-1` is
   wrong for this panel; that comment was right and this document was wrong to discard
   it. The flag is now `-D TFT_MISO=18` in `platformio.ini` with the reasoning kept as
   the comment above it. `PIN_SD_CS=17` is **already set** in the env, and
   `variants/heltec_v4/HeltecV4Board.cpp` already parks the pad HIGH before display init
   — that work is **done**, do not redo it.
3. ~~**Add `HELTEC_LORA_V4_ST7796` to the Arduino-SD gate in `main.cpp`, and give it its
   own SPI handle.**~~ **DONE.** `CAP_SD=1` **on its own compiles no mount code for this
   target.** The whole `mapSdStorage()`/SD-mount block sits behind
   `#if defined(HAS_TDECK_GT911) || HAS_TDECK_PRO || HELTEC_LORA_V4_R8 || TLORA_PAGER ||
   HAS_THINKNODE_M9 || HAS_CROWPANEL_35`, and `HELTEC_LORA_V4_ST7796` is **not in that
   list**. Flipping the flag alone produces a build that still has no SD. And the
   existing `#else` arms there select **`tdeckSharedSPI()`** — the SX1262 **radio** bus
   (SCK 9 / MISO 11 / MOSI 10 / CS 8). That is the wrong bus *physically*, and on this
   target it is also an **undeclared symbol** — a link error. So the gate addition and a
   correct handle are one change, not two. Landed as:
   `SPIClass* _spi = nullptr;` in the declaration chain's
   `#elif defined(HELTEC_LORA_V4_ST7796)` arm, then
   `_spi = &TFT_eSPI::getSPIinstance();` in the selection chain ahead of the `#else`
   that picks `tdeckSharedSPI()`, then `CAP_SD=1` at `#define CAP_SD` in the ST7796
   block of `src/ui-touch/device_caps.h`.

   ⚠️ **`getSPIinstance()` returns a `SPIClass&`, not a pointer.** The handle has to
   be taken **by address** — `&TFT_eSPI::getSPIinstance()` — otherwise the assignment
   to `SPIClass*` does not compile. This correction is relative to the wording used
   above when this step was written.

   ⚠️ **`CAP_SD=1` opens more than `main.cpp`.** Flipping the capability also
   compiles the file-manager SD path in `src/ui-touch/UITask.cpp`, which for this
   target additionally needed `HELTEC_LORA_V4_ST7796` in two more gates: the
   `#include <SD.h>` / `sd_diskio.h` / `PIN_SD_CS` block near the top of that file
   (`SD`, `CARD_NONE` and `SdDriveAccess` are otherwise undeclared and it fails in
   ~30 places), and the `sdSharedSPI()` accessor chain, whose `#else` arm is
   `tdeckSharedSPI()`. `main.cpp` likewise needed the target in the SPIFFS→SD
   migration block and in the `SdNvsPrefs::useFile()` routing arm. All four are
   additive gate extensions — no existing board's behaviour changed.

4. **Init order is a vendor requirement:** Waveshare's demo states *"SD card
   initialization must be placed after the LCD."* Mount SD only after the panel is up. ⚠️ An
   earlier pass here claimed the schematic also made this a *hardware* requirement, via a Q2
   network keyed off `LCD_CS`. **That was wrong** — R16 is marked `NC`, so nothing couples
   `LCD_CS` to the card's CS. Follow the vendor's ordering as a demo convention, not as an
   electrical constraint on this module. See the TF socket subsection.
5. **Keep the SD clock well below the LCD clock.** The shared bus runs at 40 MHz
   MEASURED for the LCD - and far too fast for a card. Interleaving high-speed LCD DMA
   fills with card reads is the classic failure here. `include/SdFastClock.h` exists as
   a precedent for *raising* it deliberately, so the inverse is safe. (The dupont wiring
   is a suspected limit on the *display* clock, not a measured ceiling — see root cause
   #2 and note that the 40 MHz causal claim was retracted.)
6. ⚠️ **Transaction nesting is a real hazard and there is currently no mechanism for it.**
   SD's `beginTransaction`/`endTransaction` run on the **same `SPIClass` object the
   display drives**. TFT_eSPI is **not reentrant**, and `beginTransaction` **overwrites
   the shared clock divider** — so a card transaction changes the display's clock too,
   and a display push issued inside a card transaction inherits the card's. A panel
   refresh during an SD write is the realistic interleaving. The known fix on the other
   boards is a mutex around both sides; **this env does not have one.** Decide on it
   explicitly rather than discovering it as intermittent tearing. **STILL OPEN** — the
   mount branch in `main.cpp` documents the hazard at the call site, but locking was
   explicitly left out of scope and is not implemented.
7. **Keep `LCD_CS` (GPIO33) rigorously de-asserted between transactions.** If it idles
   low, the ST7796 drives `MISO` against the card during SD reads. ⚠️ Note that parking
   `PIN_SD_CS` HIGH is **not** the same thing — it de-asserts the *card*, not the
   *display*. Nothing in the current build guarantees `LCD_CS` is high while SD is
   talking, so this step needs an actual assertion (and TFT_eSPI must not be mid-push).
   **PARTLY DONE:** `variants/heltec_v4/HeltecV4Board.cpp` does **not** touch GPIO33 —
   the only existing parking there is `PIN_SD_CS`, the wrong pad — so the mount branch
   in `main.cpp` now asserts `digitalWrite(TFT_CS, HIGH)` immediately before the card
   transaction and again on the way out. That is a point-in-time assertion around the
   mount, **not** a continuous guarantee, and it is correct only because the mount runs
   after the panel is up (step 4) and not during a push. The runtime case — an SD write
   overlapping a later panel refresh — is still unguarded, and belongs to item 6.

   ⚠️ **Correction to an earlier pass: the card's CS *is* driven by `PIN_SD_CS` alone.**
   This step previously claimed the R15/Q2 network tied the card's CS to `LCD_CS`, so the
   card was selected at idle and contended with the GPIO. That claim is **withdrawn** — R16
   is marked `NC` (not fitted), so Q2 and R14 are electrically isolated from the card's CS
   and gate nothing. Parking `PIN_SD_CS` HIGH genuinely de-asserts the card, and this step
   remains a requirement about the *display* pad only. See the TF socket subsection.
8. **`fmIsSd()` is NOT a gap — do not edit it.** `UITask.cpp` already gates on
   `#if CAP_SD ... return fs == &SD;`, so setting `CAP_SD=1` selects the correct
   definition automatically. The *"Heltec V4 ST7796 has no Arduino SD global in FM
   path"* arm is simply the `#else` you are removing. Editing that line by hand would be
   editing the wrong one. **CONFIRMED** — that arm is no longer selected, and the
   surrounding `UITask.cpp` gates did need extending (see the ⚠️ note in step 3).
9. Report free/total space in Settings; one write → read → delete round trip.

### TF socket (J1, TF-07F) — schematic-verified pinout

Read directly off the vendor schematic for this module:

| Pin | Signal | Connection |
|---|---|---|
| 1 | D2 | NOT CONNECTED |
| 2 | CD/D3 | `SD_CS` net through R15 (0 Ω, fitted) — the card's chip select in SPI mode |
| 3 | CMD | `MOSI` net — shared with the LCD |
| 4 | VDD | 3V3, with C7 100nF decoupling |
| 5 | CLK | `SCLK` net — shared with the LCD |
| 6 | VSS | GND |
| 7 | D0 | `MISO` net — shared with the LCD |
| 8 | D1 | NOT CONNECTED |
| 9 | CD (card detect) | NOT CONNECTED |
| 10 | GND | GND |

⇒ What this settles:

- **4-bit SDIO / `SD_MMC` is physically impossible on this panel.** DAT1, DAT2 and the
  card-detect contact are all unconnected. Only 1-bit SPI mode is possible. (An earlier
  note in this document already said the DAT1/DAT2 part of this — it was correct.)
- **The TF slot is SPI-only and shares MOSI, SCLK and MISO with the ST7796.** There is no
  separate `SD_MISO` / `SD_MOSI` / `SD_SCLK` pin, and none is needed.
- **The card has NO card-detect line.** Do not look for `board.sdCardPresent()` support on
  this board; presence must be discovered by the mount attempt.
- The card is powered from the panel's own `3V3`, which is a **regulated output** — do not
  tie it to the Heltec V4's 3.3 V rail. (Same rule as the panel's `3V3` pin, noted in 0.1.)

⚠️ **Pin 7 (D0) IS routed to `MISO`, by direct trace** — no series component, no diode,
nothing in between. **This corrects an earlier finding in this document:** a session
concluded from an empty-socket continuity test that the TF MISO was unroutable. That
conclusion is **withdrawn**.

The reason that reading misled: **DAT0 is a recessed spring contact.** With no card
inserted there is nothing to touch, so a continuity test from the `MISO` net to an empty
socket reads as open by construction. To verify it, insert a card and measure from the
card's DAT0 pad (card pin 7) back to the header `MISO`.

⚠️ **The card's CS *is* a plain `SD_CS` connection. The "active circuit" finding that used
to sit here is RETRACTED.** Re-read at high magnification, the card-CS network is:

```
SD_CS ──[R15 0R]──┬──→ J1 pin 2 (CD/D3) = card CS
                 │
LCD_CS ──●──[Q2 gate]      Q2 source → GND
         │                 Q2 drain ──[R16 "NC"]──┘
    [R14 10K] to GND (gate pull-down)
```

**R16 is marked `NC` — NOT FITTED.** An unpopulated 0-ohm link is an open circuit, so
**Q2 (and R14) are electrically isolated from the card's CS.** They gate nothing.

⇒ **Corrected facts:**
- The card's CS is simply `SD_CS` passed through `R15` (0 Ω, fitted).
- There is **no contention**, **no Q2 effect**, and the card is **not** selected at idle.
- MEASURED on the real board and consistent with this: card CS reads **3.3 V at idle**,
  because `HeltecV4Board.cpp` parks `PIN_SD_CS` HIGH. That is the correct deselected state.

**What was retracted, and why it was wrong.** An earlier pass in this document claimed that
whenever `LCD_CS` is high, Q2 pulls the card's CS low so the card is selected at idle
regardless of what the SD CS GPIO does; that two drivers then contend on the CS node and the
MOSFET wins; and that R15/R14/Q2 form an active CS-multiplex circuit which makes the vendor's
LCD-before-SD ordering a *hardware* requirement. **All of that is withdrawn.** The single
error behind it was reading R16 as a fitted link instead of an absent one — a
low-visibility mistake, because in this schematic `NC` means the part is **not populated**,
and the symbol looks the same either way. Recording it here so the next reader does not
re-derive it.

**What remains genuinely unresolved** is *why* the Q1/Q2/R14 network is drawn at all, and
what polarity the vendor intended for it. That is not the current fault — see the failure
note below.

**Acceptance [HW]:** card mounts, size reports, round trip succeeds, File Manager shows
the volume, LCD unaffected throughout. **NOT MET — see the failure note below.**

⚠️ **Current failure [HW] (2026-10-16, MEASURED on the real board):** with a card inserted
and `TFT_MISO=18` in place, the mount fails with **`sdSelectCard(): Select Failed`** — the
card does not answer `CMD0`. Readings taken at idle:

| Node | Measured | What it means |
|---|---|---|
| Card VDD | 3.3 V | powered — rules out brownout |
| Card CS | 3.3 V | correct deselected state (`HeltecV4Board.cpp` parks `PIN_SD_CS` HIGH) |
| MISO | 0 V | expected — card deselected means MISO is high-Z |
| MOSI | 0 V | expected — same |

⚠️ **Idle DMM readings cannot observe a fault that only occurs during the transaction.**
Every figure above is a *correct* idle state, and none of them can see what happens once
`CMD0` is actually clocked out. The outstanding **decisive test** is end-to-end continuity
from the card's **DAT0 pad** back to **GPIO18**, with the card inserted and power off (see
the recessed-spring-contact note above). SD remains **CODE DONE, NOT YET PROVEN [HW]** —
none of this marks it working.

---

## Phase 4 — UI and LoRa verification (item 4)

**Status: DONE (2026-10-16).** Verified on hardware: the full UI renders and is
touch-navigable, and LoRa **RX is confirmed** - messages arrive from a second node and
appear in the chat view. Goal 4 is complete.

**Not yet confirmed:** LoRa **TX** end-to-end, and companion-web reachability
(`MULTI_TRANSPORT_COMPANION=1`). Neither blocks goal 4 as scoped; both are worth a pass
if the companion link is needed. Wi-Fi/BLE is disabled on this build, and enabling it
still logs `addApbChangeCallback(): duplicate func` (PHY double-init) — a known open
item, not a display/touch/radio fault, and not a contradiction of the DONE status above.

Walk every tab — Chats, Contacts, Home, Map, Sensors, Settings — with touch live. Tab
registry `src/ui-touch/UITask.cpp:2280-2302`; sensors tab is conditional on
`HAS_EXPANSION_KIT` plus a detected sensor.

LoRa, both directions:
- **TX:** `composerSendFromPanel()` (`:11091`) → `the_mesh.sendMessage()` (`:68393`).
  Confirm the TX counter increments and the message lands in the DM/channel ring.
- **RX:** receive from a second node; confirm `LvChatPanel` (`:2688-2689`) updates.

Use `scripts/test_companion_serial.py --port <COM>` for protocol-level checks
(`CMD_DEVICE_QUERY`, `CMD_GET_STATS`, `CMD_SEND_TXT_MSG`). pyserial and manual — no
harness automation exists.

**Acceptance [HW]:** every tab renders and is touch-navigable; TX and RX confirmed
against a second node; companion web reachable (`MULTI_TRANSPORT_COMPANION=1`).

---

## Phase 5 — CardKB verification (item 5)

Driver is complete and the protocol is confirmed against M5Stack's firmware:

- Bare AVR TWI slave, **no register map**. `Wire.requestFrom(0x5F, 1)` is the entire
  protocol, returns exactly one byte; idle is `0x00`, never a short read.
- One ASCII-ish code per event. No modifier byte, no key-up, no auto-repeat, no rollover.
  Shift/Sym/Fn resolve **on the keyboard**; state is visible only via the RGB LED.
- Key map matches `cardKbUiKey()` (`UITask.cpp:48394-48411`) already expects: arrows
  `0xB4–0xB7`, enter `0x0D`, esc `0x1B`, backspace `0x08` (`0x7F` under Shift), ASCII
  `0x20–0x7E`, Fn block `0x80–0xAF`.
- Runtime-only presence: 12 ms present / 1000 ms absent, 3 failed reads to declare
  disconnect (`CardKbKeyboard.cpp:35-71`).

Steps:
1. Wire per 0.2. **The bus work is DONE** — `PIN_CARDKB_SDA=15` / `PIN_CARDKB_SCL=16` are
   already in the env and `CardKbKeyboard.cpp` already has its own `TwoWire s_cardkb_bus(1)`
   on those pads; the `Wire` fallback only survives in an `#else` for legacy boards. Do
   **not** repoint `PIN_BOARD_SDA`/`PIN_BOARD_SCL`, which carry the sensors. What is left
   here is physical: fit the external pull-ups explicitly — the panel's 4.7 kΩ pair is on
   the other side of its translator and is no longer in circuit for this bus.
2. `cardKbPresent()` should flip true. Type into the composer; arrows navigate. Confirm
   the on-screen keyboard stays suppressed via `externalKeyboardTyping()` in `UITask.cpp`.

### CardKB cautions

- **Reads are destructive.** `requestEvent()` clears `hadPressed` after one read, and the
  repo has **two poll sites** — `bleKbdUiTick()` in `UITask.cpp` (its `cardKbPoll()` call)
  and console mode (`cardKbPoll()` in the console loop, a few lines below the
  `m9KeyboardPoll()` call above it). Confirm they are mutually exclusive; concurrent
  polling silently drops ~50% of keys.
- **~1.8 s of boot delay.** CardKB `setup()` runs LED fades *before* `Wire.begin(0x5f)`,
  so it will not respond for ~2 s. The 1000 ms re-probe covers this.
- **100 kHz is correct** for this bus and it is now isolated, so nothing can raise it.
- **The repo's "never ACKs a bare address probe" claim is unverified and probably wrong.**
  Meshtastic's current detector does exactly that and works; AVR TWI hardware ACKs a
  matched address unconditionally. `requestFrom` is robust either way — do **not**
  "optimise" it.
- **Do not flash CardKB firmware.** No ATmega8A hex is published, and community forks
  (UART/PS2) remove I2C. Two boards bricked from choosing the wrong oscillator.

---

## Open items, in risk order


1. **SD mount-and-prove on real hardware** - the firmware half is **DONE and builds**
   (2026-10-16): `TFT_MISO=18`, the `HELTEC_LORA_V4_ST7796` gate in `main.cpp` with a
   `TFT_eSPI::getSPIinstance()` handle, and `CAP_SD=1`. What remains is **[HW]**:
   flash it, confirm the card mounts, that size reports, one write → read → delete
   round trip, and that the LCD is unaffected throughout. Mount and prove only - no
   log persistence, no tile backend.
2. ~~**GPIO18 reach is UNVERIFIED**~~ — **CLOSED (2026-10-16): the user CONFIRMED GPIO18
   by measurement.** Removed from this list; see wiring table row 4 in 0.1 and Phase 3
   step 1. (The old text read: every SD fix assumed the TF slot's MISO lands on GPIO18,
   that claim came from a comment transcribing the schematic rather than from anything
   provable in the repo, and it needed a continuity check first.)
3. **SPI transaction nesting** - SD and the display share one `SPIClass`; TFT_eSPI is not
   reentrant and `beginTransaction` overwrites the shared clock divider. Nothing currently
   serialises the two, and parking `PIN_SD_CS` HIGH does **not** de-assert `LCD_CS`
   (GPIO33). Needs a real mechanism, not just awareness. The mount branch now asserts
   `TFT_CS` HIGH around the card transaction, which covers the mount itself but **not** a
   panel refresh that lands during a later SD write.
4. **Re-measure GPIO38 under load** before enabling SD. The overvoltage that used to be the
   top blocker is resolved (CardKB on 3.3 V, all lines now 3.3 V), but confirm it with the
   bus loaded rather than idle. **[HW]**
5. **Re-test the SPI clock after wiring rework** - 40 MHz is the current verified-working
   value, clean 8/8 on dupont jumpers, not a proven ceiling. Short soldered leads should
   go higher. The *cause* of the old garbling is still unknown and the 40 MHz causal claim
   was retracted, so re-test after rework. **[HW]**
6. **GPIO38 GNSS collision** - verify the GNSS header is unpopulated (`PIN_GPS_RX=38` and
   `PIN_BOARD_SDA=38` claim the same pad; currently held off by `ENV_INCLUDE_GPS=0`).
   Related: `PIN_TOUCH_INT` is undefined here, so the touch driver pull-ups **GPIO39**,
   which is `PIN_GPS_TX`. **[HW]**
7. **TXS0108E contention** - designed around by the split-bus decision; revisit only if
   the split bus proves impractical.
8. **`ARDUINO_USB_MODE=1` is set deliberately and is staying** - this was open while it
   was marked bring-up-only; the user has since decided to keep hardware CDC (`PID_1001`)
   rather than revert to TinyUSB, having hit problems with it before on this host. Serial
   was the only diagnostic channel for this whole port, so losing it to gain config
   parity with the board JSON is not worth it. **No action needed.**
9. **Wi-Fi/BLE PHY double-init** - `addApbChangeCallback(): duplicate func` appears
   whenever Wi-Fi/BLE is enabled. Unresolved; Wi-Fi/BLE is currently disabled. Also a
   hygiene item, **not** a contradiction of the WORKING status: the verified paths are
   display, touch, LoRa RX and CardKB, none of which use the Wi-Fi/BLE PHY.

Closed: the flash budget (measured 64.5% post-touch, ~2.1 MB free - the 64.3% figure was
the pre-touch build and the old 99.7% figure was stale), the boot loop (PSRAM CS on the
panel reset line, root cause #1), touch (commit `08f7fda`), the CardKB bus, and goal 4
(UI + LoRa RX).

## Explicitly unconfirmed

- **Hardware revisions of SKU 29318.** No rev marker published; one schematic
  (2024-11-30) which does contain the TF slot. Treat "no revisions" as absence of
  evidence.
- **The exact role of Q1 around `SD_CS`** in the module schematic. The Q2 half is now
  read directly off the schematic (NMOS, gate on `LCD_CS`, R14 10 kΩ pull-down, and R16
  marked `NC` between Q2's drain and the card-CS node — see the TF socket subsection in
  Phase 3). Because R16 is **not fitted**, **Q2 and R14 are electrically isolated from the
  card's CS and gate nothing**: the card's CS is `SD_CS` through R15 (0 Ω, fitted), with no
  contention and no selection at idle. ⚠️ **An earlier pass in this document treated the
  not-fitted R16 as a fitted link and concluded that Q2 pulls the card's CS low whenever
  `LCD_CS` is high — that claim is RETRACTED**, and so is the contention and
  CS-multiplex reading built on it. The error is recorded because `NC` here means the part is
  *absent*, which is exactly the detail that is easy to miss. What still cannot be
  determined from the schematic is **why the Q1/Q2/R14 network is drawn at all**, and that is
  *not* the current mount failure — see the failure note in Phase 3.
- **Whether INT is read-clear (reg 0x00/0x01) or self-clearing in polling mode.**
  LovyanGFX and the app note conflict. We poll and ignore INT, so it does not block us —
  but a logic analyser would settle it.
- **CardKB supply current** — none published. Budget ≥100 mA on the 5V rail.
- **Whether the Heltec header already populates I2C pull-ups on GPIO15/16.** Measure total
  bus resistance with power off; expect >10 kΩ and add resistors if so.

## Sources

- Panel wiki (pinout, schematic, demo): https://www.waveshare.com/wiki/3.5inch_Capacitive_Touch_LCD
- FT6336U register map (FocalTech app note): https://wiki.lilygo.cc/datasheet/FT6236-FT6336-FT6436L-FT6436_Datasheet.pdf
- Waveshare ESP-IDF component: https://github.com/waveshareteam/ESP32-S3-LCD-Driver-Board
- LovyanGFX `Touch_FT5x06`: https://github.com/lovyan03/LovyanGFX
- M5Stack CardKB v1.1: https://docs.m5stack.com/en/unit/Unit_CardKB_v1.1
- Heltec V4 / V4-R8 datasheets: https://resource.heltec.cn/download/WiFi_LoRa_32_V4/datasheet/WiFi_LoRa_32_V4.3.1_Datasheet.pdf
