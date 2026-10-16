# Heltec V4-R2 + Waveshare 3.5" capacitive-touch LCD — bring-up findings

**Status: WORKING.** Display, radio and companion link verified on real hardware
(2026-10-16). Touch is now enabled and verified; SD remains the only disabled item.
Panel identity confirmed as Waveshare SKU **29318**, `3.5inch Capacitive Touch LCD`
(ST7796S + FT6336U + TF slot). Items still requiring hardware are marked **[HW]**.

**Read the "Hardware bring-up findings" section before touching anything here.** The
first two entries in this document — the "open heap corruption" blocker and the
`WADA_*` bring-up scaffolding around Wi-Fi/BLE — are both **closed and were wrong**.
The boot loop was never heap corruption; it was the panel reset line parked on the
ESP32-S3's PSRAM chip-select. Re-deriving that will cost another day.

**Target env: `heltec_v4_st7796_companion_radio_touch`** (`platformio.ini:253-557`).
The stock `heltec_v4_tft_companion_radio_usb_tcp_touch` env is **out of scope**.

⚠️ **This config is R2-only.** On the V4-R8 (octal PSRAM) GPIOs 33–37 are consumed by
PSRAM, so `LCD_CS=33` dies. Heltec documents this as a hard incompatibility.

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
bring-up down several dead ends (`WADA_NO_WIFI_BRINGUP`, `WADA_NO_BLE_BRINGUP`,
`WADA_RXLOG_NO_PSRAM`, `WADA_DISP_NO_PUSH`, `WADA_REBOOT_DIAG` — all now removed).

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
| 40 MHz | **Garbled** — 4 of the first boots after every flash erase |
| 20 MHz | **Clean 8/8** from a freshly erased flash, animated background enabled |
| 8 MHz | Clean 8/8 |

`SPI_FREQUENCY=20000000` is the current value. The key evidence that this is *timing*
and not memory state: **seven consecutive boots produced byte-identical
`psram_free`, `nvs_free_entries` and LVGL configuration whether they rendered
correctly or garbled.** Identical state, different output ⇒ the discriminator is on
the wire, not in RAM.

⇒ Very likely a **wiring** limit, not a chip limit. Short, hard-soldered display leads
should permit considerably more. **Re-test the clock after rework** — do not treat
20 MHz as a permanent ceiling.

### Other findings

| Area | Finding |
|---|---|
| **Build environment** | This repo cannot build when the Windows username contains a space: PlatformIO expands ~212 include flags under that home dir, SCons passes them unquoted, and every compile dies with `CreateProcess: No such file or directory`. The compiler itself is fine. Fixed with a `C:\pio` junction to `%USERPROFILE%\.platformio` plus `setx PLATFORMIO_CORE_DIR C:\pio`. Affects every PlatformIO project on the machine. |
| **Flash headroom** | Measured **3,960,985 / 6,160,384 = 64.3%** of flash, ~2.1 MB free. The old `99.7%` claim at `platformio.ini:259-262` was **stale** (written before `partitions_st7796.csv` widened the app slots). Corrected. |
| **`TFT_MISO=-1`** | This is a **4-wire** panel — write-only, no MISO line brought out. A register readback returning `COLMOD=0xFF` is the tell: reads fail closed, so any "SPI readback" evidence collected through MISO on this build is meaningless. Leave `TFT_MISO=-1`. |
| **Display geometry** | Declare the panel at its **NATIVE `TFT_WIDTH=320` / `TFT_HEIGHT=480`** and call **`setRotation(1)`**. Declaring it landscape and hand-writing MADCTL is wrong: TFT_eSPI caches its rotation state and desynchronises from the register, so the address window, the driver's assumed dimensions and the hardware all disagree. `setRotation()` keeps the cache and the register in step. |
| **UI rotation pinning** | `s_ui_rotation` must be pinned to **`LV_DISP_ROT_90`** in `UITask.cpp`. It is a **persisted pref** that *overrides* `DEFAULT_UI_ROTATION`, and `device_caps.h` sets `CAP_ROTATABLE 0` for this target, so the UI offers no way to change it back. An unpinned value therefore sticks forever. |
| **Pin map** | Verified by I2C scan: CardKB **ACK 0x5F** on GPIO15/16 and answers `requestFrom`; backlight responds on GPIO6. Panel touch bus is **0x38** on GPIO38/4 but is currently unused — see the voltage issue. |
| **CardKB bus** | Needs its **own I2C peripheral on GPIO15/16**, not a second `TwoWire` alias onto the board bus. Beyond the TXS0108E contention argument already recorded below: **polling the panel's touch line for an absent device wedged SCL and stalled the UI for 525 ms every second.** An isolated bus cannot do that. |
| **GPIO38 collision** | `PIN_GPS_RX=38` and `PIN_BOARD_SDA=38` claim the same pad. `EnvironmentSensorManager::begin()` calls `initBasicGPS()` first and takes GPIO38. **GPS cannot coexist with the panel's touch bus on this board** — mutually exclusive, not a bring-up-only setting. |
| **CardKB ACKing** | The repo comment claiming the CardKB "never ACKs a bare address probe" is **false** — it ACKs. `requestFrom` is still correct and is kept. |

### STILL OPEN — the panel I2C pull-ups overvoltage

**Unresolved, and it is why touch and SD are disabled.** The panel's I2C pull-ups are
referenced to **VCC, not 3V3**. With VCC on 5 V this drove **GPIO38 to 4.23 V**,
against the ESP32-S3's **3.6 V absolute maximum**. ESP32-S3 GPIOs are not 5V-tolerant.

⇒ Both `CAP_TOUCH=0` and `CAP_SD=0` in `src/ui-touch/device_caps.h` are deliberate
and must stay until the bus is fixed. The touch controller *is* present and *does*
ACK at `0x38` — it is disabled purely on voltage grounds.

**Before re-enabling either:** level-shift the bus, or re-terminate the pull-ups to
3V3. Re-measure GPIO38 at idle *with the bus loaded* before assuming it is fixed.
The panel's `3V3` pin is a *regulated output* — do **not** tie it to the V4's 3.3 V rail.

### Firmware bugs found and fixed on the way

1. `main.cpp:932` — `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` **panics when PSRAM is present**; its own `if (psram_bytes)` guard passes precisely in the failing case.
2. `main.cpp:1713` — the "last resort format" fallback **defeated the SPIFFS-skip diagnostic**, so every boot died in the `esp_spiffs_init` path the diagnostic existed to avoid.
3. `GPIO17` (`PIN_SD_CS`) was **unconfigured and floating** while the display drove SPI3; now parked HIGH before display init.
4. Fourteen `ENV_INCLUDE_*` sensor drivers compiled against a bus holding **exactly one device**.
5. **`loopTask`'s default 8 KB stack was exhausted** (free stack 0 of 8192). Fixed with `ARDUINO_LOOP_STACK_SIZE=16384`. **This is a real bug and worth keeping — but it was NOT the boot loop.** `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` is off on this target, so that stack could never reach the corrupted PSRAM and could not have been the victim. **This is probably not Waveshare-specific** either: other boards run the same heavy `setup()` on 8 KB.
6. The Arduino-core Wi-Fi event-vector race (`WiFi.onEvent()` / `WiFi.mode(WIFI_STA)` push_back vs. `_arduino_event_task` walk) is a genuine core bug with a V4-R8 dump in-repo — but it was **not** what was crashing this build. Both transports run normally now; the `WADA_*` gates that suppressed them are gone.

### Bring-up workflow notes

- **Flashing requires manual download mode** once the board is stable: hold **BOOT** -> tap **RESET** -> release **BOOT**, then `pio run -e <env> -t upload --upload-port <COM>`, then tap **RESET** to run. PlatformIO's "Hard resetting via RTS pin" leaves the ESP32-S3 in ROM download mode (`boot:0x20`) and never starts the app.
- `pio` is **not on PATH**; it lives at `%USERPROFILE%\.platformio\penv\Scripts\pio.exe` (now added to the user PATH).
- `Win32_SerialPort` goes stale and omits the native-USB COM port -- use `GetPnpDevice` instead.
- TinyUSB (`ARDUINO_USB_MODE=0`) enumerated as `PID_0002` and delivered **no CDC bytes** on this host, making the boot log invisible. `ARDUINO_USB_MODE=1` (`PID_1001`) reads reliably. **BRING-UP ONLY -- revert before shipping.**
- The 25-second `[alive ...]` window at `setup()` entry is bring-up scaffolding and adds 25 s to every boot. Remove it.

## Why this shape

The panel is the *third* display this board has driven, and the repo already contains
most of what is needed:

| Item | Existing state |
|---|---|
| LCD driver | **DONE** — `variants/heltec_v4/St7796WaveshareDisplay.{h,cpp}`, MEASURED |
| Touch driver | **DONE** - `src/helpers/input/RakTapV2Touch.cpp`, FT5x06 at `0x38`. Re-targeted, not written; tap placement verified on hardware |
| SD support | **NEARLY DONE** - `mapSdStorage()` + `fmIsSd()` ship on 8 other boards; needs `CAP_SD=1` and two pins |
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
| 4 | MISO | *not brought out* | **4-wire panel — write-only.** `TFT_MISO=-1` |
| 8 | LCD_CS | **33** | R2 only (see warning above) |
| 9 | LCD_DC | **21** | Heltec `OLED RST` — harmless, no OLED fitted |
| 10 | LCD_RST | **3** | ⚠️ **NOT 26.** 26 is the PSRAM chip-select — see root cause #1 |
| 11 | LCD_BL | **6** | active-high PWM, MEASURED |
| 7 | SD_CS | **17** | new |
| 12 | TP_SDA | **38** | I2C1, touch only — ⚠️ overvoltage, see below |
| 13 | TP_SCL | **4** | I2C1, touch only — ⚠️ overvoltage, see below |
| 14 | TP_INT | *unconnected* | driver polls |
| 15 | TP_RST | *unconnected* | power-on reset |

`17`/`18` confirmed as header pins by the user; they are unpopulated OLED pins per
`variants/heltec_v4/pins_arduino.h:60-61`. ⇒ Safe to reuse, and GPIO21 working as
`LCD_DC` independently proves no OLED is fitted.

**MISO does not exist on this panel.** The original budget assumed pin 4 would let
`SD.begin()` read the CSD. It does not — the module is a 4-wire write-only display.
⇒ `TFT_MISO=-1` is correct and there is no MISO pin to wire. The tell was a register
readback returning `COLMOD=0xFF`: reads fail closed rather than returning garbage, so
an earlier "SPI readback confirms…" conclusion drawn through MISO was unfounded.
This has a knock-on for Phase 3: **SD cannot be brought up without MISO**, which is
one more reason `CAP_SD=0` is not yet a decision anyone should revisit casually.

### 0.2 CardKB — separate bus, and the pull-up decision **[HW]**

| CardKB HY2.0-4P | Colour | → Heltec |
|---|---|---|
| 1 | Black | **GND** |
| 2 | Red | **5V** (or 3V3 — see below) |
| 3 | Yellow | **GPIO15** (new — own bus, via I2C1) |
| 4 | White | **GPIO16** (new — own bus, via I2C1) |

⚠️ **Do NOT repoint `PIN_BOARD_SDA`/`PIN_BOARD_SCL` at 15/16.** Those flags also carry
every environment sensor (`ENV_INCLUDE_AHTX0`, `BME280`, `SHTC3`, `SHT4X`, `INA3221`, … —
`platformio.ini:295-309`), and the sensors are physically wired to 38/4. Moving the board
bus silently disconnects all of them.

⇒ CardKB needs **dedicated** flags (`PIN_CARDKB_SDA`/`PIN_CARDKB_SCL`) and its own
`TwoWire`. `CardKbKeyboard.cpp` currently uses bare `Wire` and inherits the board bus
(`CardKbKeyboard.cpp:41-43`), so this is a code change, not a config flip.

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

`PIN_GPS_RX=38` (`platformio.ini:348`) and `PIN_BOARD_SDA=38` (`:358`) claim the same pad
while `ENV_INCLUDE_GPS=1` (`:295`). Confirm the GNSS header is unpopulated. Note the
V4's GNSS connector spans GPIO38–42, so it also sits under `PIN_GPS_TX=39`,
`PIN_GPS_RESET=42`, `PIN_GPS_EN=34`.

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
   exclusion list, and drop `TDeckTouch.cpp` from `build_src_filter`
   (`platformio.ini:459-470`) once only the FT5x06 driver compiles — otherwise two files
   define the same symbols.
2. **Re-target geometry.** `RAW_W=240 / RAW_H=320` (`:43-44`) must become the panel's
   native **320×480 portrait**, then map to 480×320 landscape for `DISPLAY_ROTATION=2`.
   The FT6336U reports in native portrait with the **y origin at the FPC end**, and the
   ST7796 has no rotation register — so this is 100% host-side math and is the one
   genuinely novel code in the project. `RAK_TS_FLIP_X`/`RAK_TS_FLIP_Y` do not carry over.
3. **Point at the board bus** — SDA=38 / SCL=4 on **I2C0 (`Wire`)**, alongside the
   environment sensors. ⚠️ **This phase is on HOLD, not done.** The panel's I2C pull-ups
   are referenced to VCC and drive GPIO38 to ~4.23 V against a 3.6 V absolute maximum.
   See "STILL OPEN" above. The driver already uses `Wire`, so wiring it up later is
   small — but do **not** let it re-route `Wire`, or the sensors drop off.
4. **Init timing — the chip NACKs for ~300 ms after reset** (`Trsi`, datasheet Table 3-5).
   The current 3-attempt retry (`RakTapV2Touch.cpp:110-115`) is not enough on a cold
   boot. Use the Waveshare sequence: 200 ms low, then ≥300 ms before first I²C. Do **not**
   copy Waveshare's ESP-IDF component, which uses 10/10 ms — that only works because
   their carrier board is already warm.
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
10. **Flip the capability:** `CAP_TOUCH` 0 -> 1 at `src/ui-touch/device_caps.h:136`, and
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
11. **Flash budget — measured, and a non-issue.** A successful build reports
    **3,960,985 of 6,160,384 bytes = 64.3%**, leaving ~2.1 MB. The app0 slot in
    `partitions_st7796.csv` is `0x010000`–`0x5F0000` = 6,160,384 bytes, matching the
    reported maximum exactly. RAM is 164,692 of 2,097,152 = 7.9%.

    ⚠️ The comment at `platformio.ini:259-262` claiming this env sits at **99.7%** and
    "leaves no room for the FT6336U touch driver" was **stale** — written against the
    smaller app slots before `partitions_st7796.csv` widened them. It has since been
    corrected to the measured 64.3%. Do not trim `deploy/apps` or shrink `spiffs` on
    the strength of either figure.

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

⇒ Use **`SD.h` over the shared SPI3 `SPIClass`**, exactly as Waveshare's demo does
(`SD.begin(SD_CS, fspi)`). Do **not** use `SD_MMC` — that applies to the
*ESP32-S3-Touch-LCD-3.5* board, not this module.

1. Add `PIN_SD_CS=17`. **There is no MISO to enable** - the panel is 4-wire
   write-only, so `TFT_MISO=-1` stays and `SD.begin()` cannot read the CSD over this
   bus. NOTE: the TF slot does have its own MISO (GPIO 18) per the schematic, so the
   card path can read even though the LCD path cannot - but they share the net, which
   is exactly why step 5 matters.
   The GPIO38 overvoltage that blocked `CAP_SD` alongside touch is RESOLVED (CardKB on
   3.3 V), so SD is no longer held for a safety reason.
2. Set `CAP_SD=1` at `src/ui-touch/device_caps.h:139`. **No longer blocked.** The same
   GPIO38 overvoltage that held touch was the only safety reason for `CAP_SD=0`, and it
   is resolved. What remains is ordinary bring-up risk, not an electrical hazard.
3. **Init order is a vendor requirement:** Waveshare's demo states *"SD card
   initialization must be placed after the LCD."* Mount SD only after the panel is up.
4. **Keep the SD clock well below the LCD clock.** The shared bus runs at 40 MHz
   MEASURED for the LCD - already capped by the dupont wiring, see root cause #2 -
   and far too fast for a card. Interleaving high-speed LCD DMA fills with card
   reads is the classic failure here. `include/SdFastClock.h` exists as a precedent
   for *raising* it deliberately, so the inverse is safe.
5. **Keep `LCD_CS` rigorously de-asserted between transactions.** If it idles low, the
   ST7796 drives `MISO` against the card during SD reads.
6. **Fix the one known gap:** `fmIsSd()` (`UITask.cpp:23727`) notes *"Heltec V4 ST7796
   has no Arduino SD global in FM path."*
7. Report free/total space in Settings; one write → read → delete round trip.

**Acceptance [HW]:** card mounts, size reports, round trip succeeds, File Manager shows
the volume, LCD unaffected throughout.

---

## Phase 4 — UI and LoRa verification (item 4)

**Status: DONE (2026-10-16).** Verified on hardware: the full UI renders and is
touch-navigable, and LoRa **RX is confirmed** - messages arrive from a second node and
appear in the chat view. Goal 4 is complete.

**Not yet confirmed:** LoRa **TX** end-to-end, and companion-web reachability
(`MULTI_TRANSPORT_COMPANION=1`). Neither blocks goal 4 as scoped; both are worth a pass
if the companion link is needed. Wi-Fi/BLE is still disabled, and the log still shows
`addApbChangeCallback(): duplicate func` (PHY double-init) whenever it is enabled.

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
1. Wire per 0.2. Add `PIN_CARDKB_SDA=15` / `PIN_CARDKB_SCL=16` and give `CardKbKeyboard.cpp`
   its own `TwoWire` instance — **do not** repoint `PIN_BOARD_SDA`/`PIN_BOARD_SCL`, which
   carry the sensors. Fit the external pull-ups explicitly; the panel's 4.7 kΩ pair is on
   the other side of its translator and is no longer in circuit for this bus.
2. `cardKbPresent()` should flip true. Type into the composer; arrows navigate. Confirm
   the on-screen keyboard stays suppressed via `externalKeyboardTyping()` (`:2877`).

### CardKB cautions

- **Reads are destructive.** `requestEvent()` clears `hadPressed` after one read, and the
  repo has **two poll sites** — `bleKbdUiTick()` (`:50487`) and console mode (`:76038`).
  Confirm they are mutually exclusive; concurrent polling silently drops ~50% of keys.
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

1. **Panel I2C overvoltage on GPIO38 (4.23 V vs 3.6 V abs max)** — blocks *both* touch
   and SD. Needs level shifting or re-termination to 3V3. **This is the top blocker.**
   **[HW]**
2. **CardKB 5V pull-up over-voltage** — pick an option in 0.2 before wiring. **[HW]**
3. **Re-test the SPI clock after wiring rework** — 20 MHz is measured-good on dupont
   jumpers, not a proven ceiling. Short soldered leads should go higher. **[HW]**
4. **SD is blocked on MISO, not just voltage** — the panel is 4-wire write-only, so
   `SD.begin()` cannot read the CSD on SPI3 as wired. Revisit the topology. **[HW]**
5. **GPIO38 GNSS collision** — verify the GNSS header is unpopulated. **[HW]**
6. **TXS0108E contention** — designed around by the split-bus decision; revisit only if
   the split bus proves impractical.

Closed: the flash budget (measured 64.3%, ~2.1 MB free — the old 99.7% figure was
stale and is now corrected in `platformio.ini`), and the boot loop itself (PSRAM CS on
the panel reset line, see root cause #1).

## Explicitly unconfirmed

- **Hardware revisions of SKU 29318.** No rev marker published; one schematic
  (2024-11-30) which does contain the TF slot. Treat "no revisions" as absence of
  evidence.
- **The exact role of Q1/Q2 around `SD_CS`** in the module schematic. Irrelevant at 3.3V
  logic; matters only if VCC/logic is ever driven at 5V.
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
