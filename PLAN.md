# Inkplate 6 Plus E-Reader Plan

## Context
Build a functional e-book reader on the Inkplate 6 Plus using PlatformIO + Arduino framework. The device has no touchscreen, so navigation uses 3 physical buttons wired to the onboard MCP23017-1 IO expander (GPB1–GPB3). Books are stored as plain `.txt` files on a microSD card. The goal for v1 is: power on → browse library → read a book → page turn with buttons → resume position on next boot.

---

## Project Structure
```
e-reader/
├── platformio.ini
└── src/
    ├── main.cpp            — state machine + setup/loop
    ├── ButtonHandler.h/cpp — MCP23017 button polling + debounce
    ├── Library.h/cpp       — SD card .txt file listing + selection
    ├── BookReader.h/cpp    — text pagination engine (lazy SD streaming)
    ├── BookmarkManager.h/cpp — save/load page position to SD
    └── UI.h/cpp            — screen rendering for both views
```

---

## platformio.ini
```ini
[env:inkplate6plus]
platform = espressif32
board = esp32dev
framework = arduino
lib_deps =
    https://github.com/SolderedElectronics/Inkplate-Platformio-Library
monitor_speed = 115200
```

The official PlatformIO Inkplate library lives at:
https://github.com/SolderedElectronics/Inkplate-Platformio-Library

---

## Components

### 1. ButtonHandler
- 3 buttons wired to MCP23017-1 PORTB pins: **GPB1** (Prev), **GPB2** (Next), **GPB3** (Menu)
- Wiring: button connects pin to GND; use `INPUT_PULLUP` via `inkplate.pinModeInternal()`
- Read via `inkplate.digitalReadInternal()` — poll in main loop
- Software debounce: ignore re-triggers within 200ms

### 2. Library
- On boot, scan SD root for all `.txt` files
- Store filenames in a `String[]` array (cap at 50 books)
- Render as a scrollable list on screen; highlight selected entry
- Prev/Next scroll the list; Menu opens the selected book

### 3. BookReader
- **Never load the full file into RAM** — stream from SD line by line
- On first open: scan through file recording byte offsets of each page boundary → store as `uint32_t pageOffsets[]`
- Render current page: seek to `pageOffsets[currentPage]`, read lines until page is full
- Word-wrap using Inkplate's GFX `getTextBounds()` to fit display width (1024px)
- Leave margin: 40px left/right, 30px top/bottom
- Font: use Inkplate's built-in `Fonts/FreeMono9pt7b` or similar for readability

### 4. BookmarkManager
- File: `/bookmarks.txt` on SD root, format: `filename.txt=42\n` (one line per book)
- On book open: load saved page number if present
- On page turn and on Menu press: write updated position back to SD

### 5. UI
Two views driven by a state enum in `main.cpp`:
- **`STATE_LIBRARY`**: file list, selected index highlighted, renders on full refresh
- **`STATE_READING`**: current page text, book title + page number in footer
- **`STATE_MENU`** (minimal v1): shown on Menu press during reading — single option "Back to Library"

### 6. Display Strategy
- Use **1-bit mode** (black/white) for fastest page refresh and sharpest text
- Full refresh on view transitions (library ↔ reading)
- Partial refresh (`display.partialUpdate()`) on page turns to reduce flicker
- Display: 1024 × 758 px

---

## Button Wiring Diagram

**Use MCP23017 GPB pins — not direct ESP32 GPIO.** The Inkplate 6 Plus breaks out all 16 pins of a secondary MCP23017 along the board edge specifically for user expansion. Most ESP32 GPIO pins are consumed by the e-paper display driver, making them unavailable.

```
MCP23017-1 (second expander, all pins broken out on board edge)

  GPB1 ──[button]── GND    ← Previous page
  GPB2 ──[button]── GND    ← Next page
  GPB3 ──[button]── GND    ← Menu
  (INPUT_PULLUP — no external resistor needed)
```

Code uses Inkplate library wrappers (not bare Arduino calls):
```cpp
inkplate.pinModeInternal(MCP23017_INT_ADDR, inkplate.ioExpanderForward, 1, INPUT_PULLUP);
int state = inkplate.digitalReadInternal(MCP23017_INT_ADDR, inkplate.ioExpanderForward, 1);
```

For deep sleep wake: connect MCP23017 INT pin to one free ESP32 GPIO and use `esp_sleep_enable_ext0_wakeup()`.

---

## Sleep / Wake (Battery Power)

E-paper retains its image without power — the ESP32 can deep sleep between page turns with no visible change to the display.

**Sleep sequence:**
```cpp
inkplate.einkOff();                              // panel off, image stays
inkplate.setIntPin(BTN_PREV, FALLING);           // any button triggers MCP INT
esp_sleep_enable_ext0_wakeup(MCP_INT_PIN, 0);   // MCP INT → one ESP32 GPIO
esp_deep_sleep_start();                          // ~10µA draw
```

**Wake sequence (runs from top of `setup()` on wake):**
```cpp
// Check esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0
inkplate.begin();
inkplate.einkOn();
// restore state from RTC memory or SD bookmark
```

**Battery monitoring** via `inkplate.readBattery()` (returns voltage as `double`):
- LiPo range: 3.3V (empty) → 4.2V (full)
- Show battery bar in reading footer, updated on each page turn
- Auto-sleep immediately with low-battery message when voltage < 3.5V
- Auto-sleep after **5 minutes** of no button presses

**RTC memory:** Store current book filename + page number in `RTC_DATA_ATTR` variables so they survive deep sleep without an SD read on every wake.

---

## Build Order (incremental, each step is independently testable)

1. **Scaffold** — `platformio.ini` + `main.cpp` that initializes Inkplate and prints "Hello" to display. Confirms toolchain and library work.
2. **Buttons** — Wire 3 buttons, poll in loop, Serial.print on each press. Confirms hardware wiring + MCP23017 reads.
3. **SD + Library view** — Scan for .txt files, render list on screen, navigate with Prev/Next, select with Menu.
4. **BookReader** — Open selected file, build page offsets, render page 1, page-turn with Prev/Next.
5. **Bookmarks** — Save/load position; position restores correctly on reopen.
6. **Sleep/Wake** — Auto-sleep after 5min inactivity; any button wakes and resumes. Battery indicator in footer. Low-battery forced sleep.

---

## Verification Steps (per milestone)

- Milestone 1: Text appears on display, no compile errors
- Milestone 2: Serial monitor shows correct button name on press, no false triggers
- Milestone 3: All .txt filenames render in list; selection highlight moves correctly
- Milestone 4: Full book navigable page by page; last page doesn't crash
- Milestone 5: Close book, reopen → lands on saved page
- Milestone 6: 5min idle → display shows "sleeping…" → button press wakes device

---

## v2 Notes (out of scope for now)
- Native EPUB parsing (ZIP + HTML strip) — likely needs pre-processing script on PC side
- Font size selection via Menu
- WiFi OTA book download
