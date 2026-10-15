# Heltec V4-R2 + Waveshare 3.5" capacitive-touch LCD — bring-up plan

**Status:** pre-work. All four research lanes complete; no hardware connected yet
(`Win32_SerialPort` reports only `COM1`). Panel identity confirmed as Waveshare SKU
**29318**, `3.5inch Capacitive Touch LCD` (ST7796S + FT6336U + TF slot).
Items still requiring hardware are marked **[HW]**.

**Target env: `heltec_v4_st7796_companion_radio_touch`** (`platformio.ini:253-476`).
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

## Hardware bring-up findings (2026-10-14, all MEASURED)

### Closed

| Area | Finding |
|---|---|
| **Build environment** | This repo cannot build when the Windows username contains a space: PlatformIO expands ~212 include flags under that home dir, SCons passes them unquoted, and every compile dies with `CreateProcess: No such file or directory`. The compiler itself is fine. Fixed with a `C:\pio` junction to `%USERPROFILE%\.platformio` plus `setx PLATFORMIO_CORE_DIR C:\pio`. Affects every PlatformIO project on the machine. |
| **Flash headroom** | Measured **4,057,597 / 6,160,384 = 65.9%**, ~2.0 MB free. The `99.7%` claim at `platformio.ini:259-262` is **stale** (written before `partitions_st7796.csv` widened the app slots). Risk voided. |
| **Pin map** | Verified by I2C scan: FT6336U **ACK 0x38** on GPIO38/4; CardKB **ACK 0x5F** on GPIO15/16 and answers `requestFrom`; backlight responds on GPIO6. All three devices wired correctly. |
| **Panel power (SAFETY)** | The panel's I2C pull-ups are referenced to **VCC, not 3V3**. With VCC on 5 V this drove **GPIO38 to 4.23 V**, above the ESP32-S3's **3.6 V absolute max**. Fixed by powering **VCC from the 3.3 V rail**. The panel's `3V3` pin is a *regulated output* -- do **not** tie it to the V4's 3.3 V rail. Backlight brightness was unaffected. |
| **Display geometry** | `setRotation(2)` -- the obviously-correct API -- writes `MADCTL=0xE0` **and truncates the address window to 320 of 480 columns**, so the UI renders over two thirds and the rest shows stale GRAM. A raw `MADCTL=0x20` (MV only) gives full 480x320 with correct orientation. Now in `St7796WaveshareDisplay::setDisplayRotation()` with a comment explaining why the library call is wrong here. |
| **GPIO38 collision** | `PIN_GPS_RX=38` and `PIN_BOARD_SDA=38` claim the same pad. `EnvironmentSensorManager::begin()` calls `initBasicGPS()` first and takes GPIO38. **GPS cannot coexist with the panel's touch bus on this board** -- mutually exclusive, not a bring-up-only setting. |
| **CardKB bus** | The repo comment claiming the CardKB "never ACKs a bare address probe" is **false** -- it ACKs. `requestFrom` is still correct and is kept. |

### Open -- heap corruption (the real blocker)

The firmware does not reach the UI. It panics in the **TLSF heap allocator itself**:

```
MyMesh::begin()  ->  DataStore::loadPrefsInt  ->  DataStore::begin()
  -> DataStore::openRead  ->  File::openNextFile  ->  VFSImpl::open
    -> VFSFileImpl::VFSFileImpl  ->  setvbuf  ->  malloc
      -> multi_heap_realloc  ->  search_suitable_block  ->  block_prev   [FAULT]
```

`block_prev` walking a corrupted free list means **heap metadata is corrupt**, and the faulting allocation is incidental -- it is whatever `malloc` happened to be serving. This matches the pre-existing note in `main.cpp`: *"calloc() inside esp_spiffs_init -> tlsf_block_size_max -> LoadProhibited at vaddr 0x0000000F. A full flash erase did not change it."*

**Ruled out by measurement:** PSRAM (2 MB present, 2095103 free), SPIFFS (mounts cleanly), the display driver, the sensor drivers, Wi-Fi/BLE ordering (the crash *moves* when those are toggled -- the classic heap-corruption signature), and a full flash erase.

**Next step:** find the buffer overflow that corrupts the free list. Audit the paths running between `[BOOT] mesh ok` and the first prefs read -- notably the LVGL/UI allocation paths and `SdNvsPrefs`, a file-backed `Preferences` implementation with its own A/B snapshot buffers. AddressSanitizer, or a heap canary sampled immediately before `loadPrefs`, would localise it far faster than more coredump reading.

### Firmware bugs found and fixed on the way

1. `main.cpp:932` -- `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` **panics when PSRAM is present**; its own `if (psram_bytes)` guard passes precisely in the failing case.
2. `main.cpp:1713` -- the "last resort format" fallback **defeated the SPIFFS-skip diagnostic**, so every boot died in the `esp_spiffs_init` path the diagnostic existed to avoid.
3. `GPIO17` (`PIN_SD_CS`) was **unconfigured and floating** while the display drove SPI3; now parked HIGH before display init.
4. Fourteen `ENV_INCLUDE_*` sensor drivers compiled against a bus holding **exactly one device**.
5. **`loopTask`'s default 8 KB stack was exhausted** (free stack 0 of 8192) -- the true root cause of the original boot loop. `esp_spiffs_init` was overflowing it, which read like a SPIFFS bug and survived a flash erase because it is RAM. Fixed with `ARDUINO_LOOP_STACK_SIZE=16384`; **this is probably not Waveshare-specific**, since other boards run the same heavy `setup()` on 8 KB.
6. `WiFi.onEvent()` / `WiFi.mode(WIFI_STA)` panics this build -- the Arduino core's WiFi event-vector race, already documented in-repo with a V4-R8 dump. Worked around with `WADA_NO_WIFI_BRINGUP=1`.

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
| Touch driver | **NEARLY DONE** — `src/helpers/input/RakTapV2Touch.cpp`, FT5x06 at the same address `0x38`; needs re-targeting, not writing |
| SD support | **NEARLY DONE** — `mapSdStorage()` + `fmIsSd()` ship on 8 other boards; needs `CAP_SD=1` and two pins |
| CardKB | **DONE** — `src/helpers/input/CardKbKeyboard.cpp`, 73 lines, protocol verified |

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
| 6 | SCLK | **47** | SPI3, 40 MHz MEASURED |
| 4 | MISO | **18** | SPI3 MISO — SD read requires it |
| 8 | LCD_CS | **33** | R2 only (see warning above) |
| 9 | LCD_DC | **21** | Heltec `OLED RST` — harmless, no OLED fitted |
| 10 | LCD_RST | **26** | |
| 11 | LCD_BL | **6** | active-high PWM, MEASURED |
| 7 | SD_CS | **17** | new |
| 12 | TP_SDA | **38** | I2C1, touch only |
| 13 | TP_SCL | **4** | I2C1, touch only |
| 14 | TP_INT | *unconnected* | driver polls |
| 15 | TP_RST | *unconnected* | power-on reset |

`17`/`18` confirmed as header pins by the user; they are unpopulated OLED pins per
`variants/heltec_v4/pins_arduino.h:60-61`. ⇒ Safe to reuse, and GPIO21 working as
`LCD_DC` independently proves no OLED is fitted.

**MISO was not in the original budget.** `TFT_MISO=-1` because the LCD is write-only,
but `SD.begin()` reads the CSD. The panel exposes MISO on pin 4 ⇒ SPI3 needs a real
MISO pin.

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
   environment sensors. All of these are 100 kHz devices, so there is no clock compromise,
   and the panel's TXS0108E pull-ups go to 3V3 so there is no voltage fight either. The
   CardKB is deliberately *not* on this bus. The driver already uses `Wire`, so this step is
   smaller than it looks — but do **not** let it re-route `Wire`, or the sensors drop off.
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
10. **Flip the capability:** `CAP_TOUCH` 0→1 at `src/ui-touch/device_caps.h:136`, and
    retire the no-op `variants/heltec_v4/St7796NoTouch.cpp`.
11. **Flash budget — measured, and a non-issue.** A successful build reports
    **4,057,597 of 6,160,384 bytes = 65.9%**, leaving ~2.0 MB. The app0 slot in
    `partitions_st7796.csv` is `0x010000–0x5F0000` = 6,160,384 bytes, matching the
    reported maximum exactly. RAM is 170,464 of 2,097,152 = 8.1%.

    ⚠️ The comment at `platformio.ini:259-262` claiming this env sits at **99.7%** and
    "leaves no room for the FT6336U touch driver" is **stale** — almost certainly written
    against the smaller app slots before `partitions_st7796.csv` widened them. Do not
    pre-emptively trim `deploy/apps` or shrink `spiffs` on the strength of that comment.
    Re-measure after this phase instead.

**Colour order note:** Waveshare's two official demos **disagree** — the ESP-IDF BSP
forces BGR, the Arduino demo passes RGB. The repo's `TFT_RGB_ORDER=TFT_RGB` +
`TFT_INVERSION_ON=1` is MEASURED working and supersedes both. Don't change it on the
strength of the datasheets.

**Acceptance [HW]:** Settings → Diagnostics shows a real `heltecV4CapTouchDebug()` string
(not `"touch: none"`); raw coords via `heltecV4CapTouchGetRaw()` are sane in all four
corners; taps and swipes land correctly at `DISPLAY_ROTATION=2`.

---

## Phase 3 — SD mount and prove (item 3)

Scope is mount-and-prove only.

**Bus topology is confirmed:** the schematic shows the TF slot wired straight to the
same `MOSI`/`SCLK`/`MISO` nets as the ST7796, differing only by CS. It is *not* a second
SPI peripheral. DAT1/DAT2 are unconnected, so **4-bit SDIO is physically impossible**.

⇒ Use **`SD.h` over the shared SPI3 `SPIClass`**, exactly as Waveshare's demo does
(`SD.begin(SD_CS, fspi)`). Do **not** use `SD_MMC` — that applies to the
*ESP32-S3-Touch-LCD-3.5* board, not this module.

1. Add `PIN_SD_CS=17` and enable SPI3 MISO=18 (currently `TFT_MISO=-1`).
2. Set `CAP_SD=1` at `src/ui-touch/device_caps.h:139`.
3. **Init order is a vendor requirement:** Waveshare's demo states *"SD card
   initialization must be placed after the LCD."* Mount SD only after the panel is up.
4. **Drop the SD clock to ≤20 MHz.** The bus is 40 MHz MEASURED for the LCD, far too fast
   for a card. Interleaving high-speed LCD DMA fills with card reads is the classic
   failure here. `include/SdFastClock.h` exists as a precedent for *raising* it
   deliberately, so the inverse is safe.
5. **Keep `LCD_CS` rigorously de-asserted between transactions.** If it idles low, the
   ST7796 drives `MISO` against the card during SD reads.
6. **Fix the one known gap:** `fmIsSd()` (`UITask.cpp:23727`) notes *"Heltec V4 ST7796
   has no Arduino SD global in FM path."*
7. Report free/total space in Settings; one write → read → delete round trip.

**Acceptance [HW]:** card mounts, size reports, round trip succeeds, File Manager shows
the volume, LCD unaffected throughout.

---

## Phase 4 — UI and LoRa verification (item 4)

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

1. **CardKB 5V pull-up over-voltage** — pick an option in 0.2 before wiring. **[HW]**
2. **Vext not asserted / switched 3V3 rail** — brown-out risk in Phase 1. **[HW]**
3. **Touch coordinate transform** — the only genuinely novel code; rotation 2 mapping
   unproven **[HW]**.
4. **SD/LCD shared SPI3 bus** — drop the SD clock and respect init order.
5. **GPIO38 GNSS collision** — verify unpopulated **[HW]**.
6. **TXS0108E contention** — designed around by the split-bus decision; revisit only if
   the split bus proves impractical.

Closed: the flash budget. The 99.7% figure in `platformio.ini` is stale; measured is
65.9% with ~2.0 MB free, so Phase 2 has ample room.

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