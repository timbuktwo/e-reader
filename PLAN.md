# Inkplate 6 Plus E-Reader Plan

## Context
Build a functional e-book reader on the Inkplate 6 Plus using PlatformIO + Arduino framework. Navigation uses the built-in capacitive touchscreen (tap zones) plus the onboard Wake button (GPIO36) — no external hardware needed. Books are stored as plain `.txt` files on a microSD card. The goal for v1 is: power on → browse library → read a book → page turn with taps → resume position on next boot.

---

## Project Structure
```
e-reader/
├── platformio.ini
└── src/
    ├── main.cpp              — state machine + setup/loop
    ├── InputHandler.h/cpp    — touchscreen tap zones + Wake button (GPIO36), debounce, input lockout
    ├── Library.h/cpp         — SD card .txt file listing + selection
    ├── BookReader.h/cpp      — text pagination engine (lazy SD streaming)
    ├── BookmarkManager.h/cpp — save/load page position to SD + RTC reconciliation
    └── UI.h/cpp              — screen rendering for all views + sleep screen
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

---

## Components

### 1. InputHandler
Navigation uses the built-in touchscreen + the Wake button (GPIO36). No external wiring needed.

**Touchscreen tap zones (STATE_READING):**
- Tap left 40% of screen → Previous page
- Tap right 40% of screen → Next page
- Tap center 20% → ignored (accidental touch buffer)

**Touchscreen tap zones (STATE_LIBRARY):**
- Tap upper half of list item → scroll up
- Tap lower half of list item → scroll down
- Tap on a title → open book

**Wake button (GPIO36):**
- Short press (< 500ms) → Menu / confirm
- Long press (≥ 500ms) → Back to library from anywhere
- Wakes device from deep sleep (via `esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0)`)

**Implementation:**
- Poll `inkplate.touchInArea()` or raw `inkplate.tsGetData()` in main loop
- Debounce: ignore re-triggers within 200ms of last touch
- **Input lockout:** ignore all input for 500ms after wake or display init
- Wake button read via `digitalRead(36)` with a timestamp to distinguish short/long press

### 2. Library
- On boot, scan SD root for all `.txt` files; store filenames in `String[]` (cap 50)
- Render as scrollable list; highlight selected entry
- Prev/Next scroll the list; Menu opens selected book
- **Error state:** if SD card is missing or unreadable, display "No SD card found. Please insert a card with .txt books." and halt
- **Empty state:** if SD has no `.txt` files, display "No books found. Copy .txt files to your SD card."
- **Filename display:** strip `.txt` extension, replace `_` and `-` with spaces, truncate to 40 chars

### 3. BookReader
- **Never load the full file into RAM** — stream from SD line by line
- On first open: scan file recording byte offsets of page boundaries → `uint32_t pageOffsets[]` (max 1000 pages = 4KB RAM)
- Render current page: seek to `pageOffsets[currentPage]`, read lines until page full
- Word-wrap using GFX `getTextBounds()` to fit 1024px width with 40px left/right margins, 30px top/bottom
- Font: `Fonts/FreeSerif9pt7b` (proportional, more book-like than monospace)
- **Page boundary behavior:**
  - Next on last page → display "End of book" message for 2s → return to STATE_LIBRARY
  - Prev on first page → no-op (do nothing)
- **Ghost mitigation:** force a full refresh every 10 page turns; counter resets on full refresh

### 4. BookmarkManager
- File: `/bookmarks.txt` on SD, one line per book: `filename.txt=42`
- On book open: prefer RTC value if it matches current book; fall back to SD bookmark
- **Save triggers:** every page turn, every state transition out of STATE_READING, and explicitly before any sleep entry (including low-battery sleep)
- **RTC vs SD reconciliation on cold boot** (battery died, RTC wiped): load from SD bookmark; if no SD entry exists, start from page 0

### 5. UI
Three views driven by a state enum:

**`STATE_LIBRARY`**
- Full refresh on entry
- Scrollable filename list, selected entry highlighted
- Footer: "X books" count

**`STATE_READING`**
- Full refresh on book open; partial refresh on page turns
- Footer: `[book title]  page X / Y  [battery bar]`
- Battery bar: 4-segment icon updated on each page turn

**`STATE_MENU`**
- Opened with Menu button during STATE_READING; **partial refresh** (not full)
- Options (navigated with Prev/Next, selected with Menu):
  1. Resume reading ← default selection, also what Menu re-press does
  2. Back to library
- Prev/Next move highlight up/down through options
- Menu confirms selection
- Re-pressing Menu with "Resume reading" highlighted (or pressing nothing for 30s) closes menu and returns to reading with no action

**`STATE_SLEEP_WARNING`** *(new)*
- Triggered when battery < 3.6V or idle timer reaches 4min 30s (30s warning before 5min sleep)
- Partial refresh overlay: "Battery low — sleeping soon" or "Sleeping in 30s…"
- Any touch or Wake button press during this 30s window resets the idle timer and returns to reading
- If no press: flush bookmark to SD, render sleep screen, enter deep sleep

**Sleep screen** *(rendered before deep sleep)*
- Clears to a simple centered message: book title + "Press Wake to resume"
- This replaces the reading page so the user can visually distinguish a sleeping device

### 6. Display Strategy
- 1-bit mode (black/white) for sharpest text and fastest refresh
- Full refresh: state transitions (library ↔ reading), first page of book, every 10th page turn
- Partial refresh: page turns within a book, menu open/close
- `partialUpdateCounter` tracked in `main.cpp`; resets on any full refresh

---

## Input Layout

No external wiring required — all inputs are built into the board.

```
┌─────────────────────────────────────┐
│                                     │
│   TAP LEFT        TAP RIGHT         │
│   (Prev page)     (Next page)       │
│                                     │
│         [center buffer zone]        │
│                                     │
└─────────────────────────────────────┘
         [WAKE BUTTON - GPIO36]
         short press = Menu / confirm
         long press  = Back to Library
         any press   = wake from sleep
```

Deep sleep wake via `esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0)` — Wake button pulls GPIO36 low.

Touchscreen read via `inkplate.tsGetData()` — returns touch coordinates; map X position to left/right zones.

Also update STATE_MENU navigation: tap top half of option = up, tap bottom half = down, Wake button = confirm.

---

## Sleep / Wake (Battery Power)

### Boot path detection
On every `setup()` entry, check wake cause first:
```cpp
esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    // woke from deep sleep — restore from RTC_DATA_ATTR
} else {
    // fresh power-on — go to STATE_LIBRARY
}
```

### Three distinct boot paths
| Situation | RTC state | Action |
|---|---|---|
| Fresh power-on | irrelevant | → STATE_LIBRARY |
| Wake from deep sleep | valid | → restore book + page from RTC |
| Wake after battery died | wiped | → load from SD bookmark, → STATE_LIBRARY |

### Sleep sequence
```cpp
bookmarkManager.saveToDisk();        // always flush to SD before sleep
UI.renderSleepScreen();              // "tap any button to wake" + book title
inkplate.einkOff();                  // panel off, image retained
inkplate.setIntPin(GPB1, FALLING);   // INTB fires on any button press
inkplate.setIntPin(GPB2, FALLING);
inkplate.setIntPin(GPB3, FALLING);
esp_sleep_enable_ext0_wakeup(INTB_PIN, 0);
esp_deep_sleep_start();              // ~10µA
```

### Wake sequence
```cpp
// In setup(), after detecting ESP_SLEEP_WAKEUP_EXT0:
inkplate.begin();
inkplate.einkOn();
buttonHandler.lockInput(500);        // discard first 500ms of input
// restore state — no button action registered for the wake press
```

**Wake convention:** the button press that wakes the device is consumed silently. The device restores the last page and waits. The user's next deliberate press is the first action. This is consistent with how Kindle and Kobo behave.

### Battery monitoring
- `readBattery()` returns voltage as `double` (LiPo: 3.3V empty → 4.2V full)
- Checked on every page turn
- < 3.6V → enter STATE_SLEEP_WARNING ("Battery low — sleeping soon")
- < 3.3V → force sleep immediately with no warning (too low to display safely)
- Battery bar in reading footer: 4 segments, updated each page turn

### Idle timer
- Resets on every button press
- At 4min 30s: enter STATE_SLEEP_WARNING ("Sleeping in 30s…")
- Any press during warning: reset timer, return to STATE_READING
- At 5min: sleep

---

## SD Card Handling

- Mount SD on boot; if mount fails → STATE_LIBRARY with error message, no further SD ops
- Unmount SD cleanly before entering deep sleep to prevent corruption
- Re-mount on wake before any SD read

---

## Build Order (incremental, each step is independently testable)

1. **Scaffold** — `platformio.ini` + `main.cpp` initializes Inkplate, displays "Hello" on screen. Confirms toolchain.
2. **Input** — Poll touchscreen tap zones + Wake button (GPIO36). Serial.print on each detected input. Confirms touch coordinates map correctly to left/right/wake actions.
3. **SD + Library view** — Scan for `.txt` files, render list, navigate with Prev/Next, select with Menu. Test error + empty states.
4. **BookReader** — Open file, paginate, render page 1, page-turn. Test last page → library transition. Test ghost refresh counter.
5. **Bookmarks** — Save/load per book. Test RTC vs SD reconciliation.
6. **Sleep/Wake** — Auto-sleep + sleep screen. Wake restores correctly. Test double-press is not felt by user. Test low-battery path.
7. **Battery UI** — Footer battery bar. Sleep warning state. Low-battery forced sleep.

---

## Verification Steps (per milestone)

- **M1:** Text on display, no compile errors
- **M2:** Serial shows "PREV", "NEXT", or "MENU" on correct tap zones and Wake button; no false triggers within 200ms debounce window
- **M3:** File list renders; selection moves; empty/no-SD error states display correctly
- **M4:** Full book navigable; last page returns to library; page 10+ shows no ghosting (full refresh fired)
- **M5:** Close book, reopen → same page; kill power mid-chapter, reopen → SD bookmark page loads
- **M6:** 5min idle → sleep screen renders → button wakes → reading resumes; first post-wake press is the action, not the wake
- **M7:** Battery bar updates each page turn; warning shows at 4:30; low-voltage forces sleep with bookmark saved

---

## v2 Notes (out of scope for v1)
- Native EPUB parsing (ZIP + HTML strip via PC-side Calibre conversion for now)
- Font size selection via Menu
- WiFi OTA book download
- Button hold-repeat for fast library scrolling
- Charging detection / "Charging" display state
