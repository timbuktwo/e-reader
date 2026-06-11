#include <Inkplate.h>
#include <SdFat.h>
#include "Fonts/FreeSerif12pt7b.h"

// --- Layout ---
#define SCREEN_W        758
#define SCREEN_H        1024
#define MARGIN_X        44
#define MARGIN_Y        40
#define FOOTER_H        36
#define TEXT_W          (SCREEN_W - MARGIN_X * 2)
#define TEXT_H          (SCREEN_H - MARGIN_Y - FOOTER_H - 10)
#define LINE_SPACING    34   // px between baselines
#define PARA_SPACING    14   // extra px after a blank line

// --- Library ---
#define LIB_LINE_H      36
#define MAX_BOOKS       50

// --- Touch ---
#define LEFT_ZONE       303
#define RIGHT_ZONE      454
#define POST_ACTION_MS  150

// --- Reader ---
#define MAX_PAGES       2000
#define FULL_REFRESH_N  10   // force full refresh every N page turns

Inkplate inkplate(INKPLATE_1BIT);

// ── State ─────────────────────────────────────────────────────────────────────
enum State { STATE_LIBRARY, STATE_READING };
State state = STATE_LIBRARY;

// ── Library ───────────────────────────────────────────────────────────────────
char bookNames[MAX_BOOKS][64];
int  bookCount    = 0;
int  selectedIdx  = 0;
int  scrollOffset = 0;

// ── Reader ────────────────────────────────────────────────────────────────────
uint32_t pageOffsets[MAX_PAGES];
int      pageCount     = 0;
int      currentPage   = 0;
int      partialCount    = 0;  // partial refreshes since last full (reading)
int      libPartialCount = 0;  // partial refreshes since last full (library)
char     openBookName[64];
SdFile   bookFile;

unsigned long lastActionMs = 0;

// ══════════════════════════════════════════════════════════════════════════════
// Helpers
// ══════════════════════════════════════════════════════════════════════════════

// Always call this before any text measurement or rendering in reading mode.
// setTextSize MUST be 1 — it acts as a multiplier with GFX fonts.
void setReadingFont() {
    inkplate.setFont(&FreeSerif12pt7b);
    inkplate.setTextSize(1);
    inkplate.setTextColor(BLACK);
}

void titleFromFilename(const char* fname, char* out, int maxLen) {
    strncpy(out, fname, maxLen - 1);
    out[maxLen - 1] = '\0';
    int len = strlen(out);
    if (len > 4 && strcasecmp(out + len - 4, ".txt") == 0)
        out[len - 4] = '\0';
    for (char* p = out; *p; p++)
        if (*p == '_') *p = ' ';
}

// ══════════════════════════════════════════════════════════════════════════════
// SD / Library
// ══════════════════════════════════════════════════════════════════════════════

bool scanBooks() {
    bookCount = 0;
    SdFat sd = inkplate.getSdFat();
    SdFile root, entry;
    if (!root.open("/")) return false;
    while (entry.openNext(&root, O_RDONLY) && bookCount < MAX_BOOKS) {
        if (!entry.isDir()) {
            char fname[64];
            entry.getName(fname, sizeof(fname));
            int len = strlen(fname);
            if (fname[0] == '.') { entry.close(); continue; }
            if (len > 4 && strcasecmp(fname + len - 4, ".txt") == 0) {
                strncpy(bookNames[bookCount], fname, sizeof(bookNames[0]) - 1);
                bookNames[bookCount][sizeof(bookNames[0]) - 1] = '\0';
                bookCount++;
            }
        }
        entry.close();
    }
    root.close();
    return bookCount > 0;
}

// ══════════════════════════════════════════════════════════════════════════════
// Text / pagination engine
// ══════════════════════════════════════════════════════════════════════════════

// Measure the pixel width of a string.
// getTextBounds returns 0 for whitespace-only strings with GFX fonts,
// so we measure a reference word with/without a leading space to get spaceW.
int spaceWidth() {
    int16_t x1, y1;
    uint16_t w1, w2, h;
    inkplate.getTextBounds("X",  0, 0, &x1, &y1, &w1, &h);
    inkplate.getTextBounds(" X", 0, 0, &x1, &y1, &w2, &h);
    return (int)(w2 - w1);
}

int wordWidth(const char* word) {
    int16_t x1, y1;
    uint16_t w, h;
    inkplate.getTextBounds(word, 0, 0, &x1, &y1, &w, &h);
    return (int)w;
}

// Read one word from file into buf (max bufLen-1 chars).
// Also sets *isNewline=true if the word was preceded by a blank line.
// Returns false at EOF.
bool readWord(SdFile& f, char* buf, int bufLen, bool* isNewline) {
    *isNewline = false;
    int c;
    int blankLines = 0;

    // Skip leading whitespace, count blank lines
    while (true) {
        c = f.read();
        if (c == -1) return false;
        if (c == '\n') blankLines++;
        else if (c != ' ' && c != '\r' && c != '\t') break;
    }
    if (blankLines >= 1) *isNewline = true;

    // Read the word
    int i = 0;
    while (c != -1 && c != ' ' && c != '\n' && c != '\r' && c != '\t') {
        if (i < bufLen - 1) buf[i++] = (char)c;
        c = f.read();
    }
    buf[i] = '\0';
    return i > 0;
}

// Core layout engine — used for both pagination and rendering.
// mode=0: paginate (record pageOffsets, count pages)
// mode=1: render page `targetPage`
void layoutFile(int mode, int targetPage) {
    bookFile.seekSet(0);

    int   page   = 0;
    int   curX   = MARGIN_X;
    int   curY   = MARGIN_Y + LINE_SPACING;  // baseline of first line
    bool  firstWord = true;

    if (mode == 0) {
        pageCount = 0;
        pageOffsets[0] = 0;
        pageCount = 1;
    }

    if (mode == 1 && targetPage > 0)
        bookFile.seekSet(pageOffsets[targetPage]);

    if (mode == 1) {
        inkplate.clearDisplay();
        setReadingFont();
    }

    // In render mode we only process one page
    if (mode == 1) {
        // Render words until page is full
        char word[128];
        bool newPara;
        int  lineX = MARGIN_X;
        int  lineY = MARGIN_Y + LINE_SPACING;

        while (true) {
            uint32_t posBefore = bookFile.curPosition();
            bool got = readWord(bookFile, word, sizeof(word), &newPara);
            if (!got) break;

            if (newPara) {
                lineX = MARGIN_X;
                lineY += LINE_SPACING + PARA_SPACING;
                if (lineY > MARGIN_Y + TEXT_H) break;
            }

            int ww = wordWidth(word);
            int spaceW = (lineX == MARGIN_X) ? 0 : spaceWidth();

            if (lineX + spaceW + ww > MARGIN_X + TEXT_W) {
                // wrap
                lineX  = MARGIN_X;
                lineY += LINE_SPACING;
                if (lineY > MARGIN_Y + TEXT_H) break;
                spaceW = 0;
            }

            inkplate.setCursor(lineX + spaceW, lineY);
            inkplate.print(word);
            lineX += spaceW + ww;
        }
        return;
    }

    // Pagination mode — scan the whole file
    char word[128];
    bool newPara;
    int  lineX = MARGIN_X;
    int  lineY = MARGIN_Y + LINE_SPACING;

    while (true) {
        uint32_t posBefore = bookFile.curPosition();
        bool got = readWord(bookFile, word, sizeof(word), &newPara);
        if (!got) break;

        if (newPara) {
            lineX = MARGIN_X;
            lineY += LINE_SPACING + PARA_SPACING;
        }

        int ww     = wordWidth(word);
        int spaceW = (lineX == MARGIN_X) ? 0 : spaceWidth();

        if (lineX + spaceW + ww > MARGIN_X + TEXT_W) {
            lineX   = MARGIN_X;
            lineY  += LINE_SPACING;
            spaceW  = 0;
        }

        if (lineY > MARGIN_Y + TEXT_H) {
            // New page starts at this word
            if (pageCount < MAX_PAGES) {
                pageOffsets[pageCount] = posBefore;
                pageCount++;
            }
            lineX = MARGIN_X;
            lineY = MARGIN_Y + LINE_SPACING;
            // Re-place this word on the new page
            spaceW = 0;
        }

        lineX += spaceW + ww;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Drawing
// ══════════════════════════════════════════════════════════════════════════════

void drawFooter(const char* title, int page, int total) {
    inkplate.setFont(NULL);
    inkplate.setTextSize(1);
    inkplate.setTextColor(BLACK);

    // Divider line
    inkplate.drawFastHLine(MARGIN_X, SCREEN_H - FOOTER_H, SCREEN_W - MARGIN_X * 2, BLACK);

    char buf[80];
    snprintf(buf, sizeof(buf), "%d / %d", page + 1, total);
    inkplate.setCursor(MARGIN_X, SCREEN_H - FOOTER_H + 14);
    inkplate.print(title);
    inkplate.setCursor(SCREEN_W - MARGIN_X - strlen(buf) * 6, SCREEN_H - FOOTER_H + 14);
    inkplate.print(buf);
}

void drawLibrary(bool forceFullRefresh = true) {
    inkplate.clearDisplay();
    inkplate.setFont(NULL);
    inkplate.setTextSize(2);
    inkplate.setTextColor(BLACK);

    if (bookCount == 0) {
        inkplate.setCursor(MARGIN_X, SCREEN_H / 2);
        inkplate.print("No .txt books found on SD card.");
        inkplate.display();
        return;
    }

    int visibleRows = (SCREEN_H - MARGIN_Y - FOOTER_H) / LIB_LINE_H;
    int y = MARGIN_Y;

    for (int i = scrollOffset; i < bookCount && i < scrollOffset + visibleRows; i++) {
        char title[64];
        titleFromFilename(bookNames[i], title, sizeof(title));
        if (i == selectedIdx) {
            inkplate.fillRect(MARGIN_X - 4, y - 2, SCREEN_W - MARGIN_X * 2 + 8, LIB_LINE_H, BLACK);
            inkplate.setTextColor(WHITE);
        } else {
            inkplate.setTextColor(BLACK);
        }
        inkplate.setCursor(MARGIN_X, y + 4);
        inkplate.print(title);
        y += LIB_LINE_H;
    }

    inkplate.setTextColor(BLACK);
    inkplate.setTextSize(1);
    inkplate.setCursor(MARGIN_X, SCREEN_H - FOOTER_H + 10);
    char footer[64];
    snprintf(footer, sizeof(footer), "%d book%s  |  left/right to scroll  |  centre to open",
             bookCount, bookCount == 1 ? "" : "s");
    inkplate.print(footer);

    if (forceFullRefresh) {
        inkplate.display();
        libPartialCount = 0;
    } else {
        libPartialCount++;
        if (libPartialCount >= FULL_REFRESH_N) {
            inkplate.display();
            libPartialCount = 0;
        } else {
            inkplate.partialUpdate();
        }
    }
    lastActionMs = millis();
}

void drawPage(bool forceFullRefresh) {
    setReadingFont();
    layoutFile(1, currentPage);  // renders text into framebuffer

    char title[32];
    titleFromFilename(openBookName, title, sizeof(title));
    // Truncate title for footer
    if (strlen(title) > 20) { title[20] = '\0'; }
    drawFooter(title, currentPage, pageCount);

    if (forceFullRefresh || partialCount == 0) {
        inkplate.display();
        partialCount = 0;
    } else {
        inkplate.partialUpdate();
    }
    partialCount++;
    if (partialCount >= FULL_REFRESH_N) {
        partialCount = 0;
    }

    lastActionMs = millis();
}

// ══════════════════════════════════════════════════════════════════════════════
// Book open
// ══════════════════════════════════════════════════════════════════════════════

void openBook(const char* filename) {
    // Show loading screen
    inkplate.clearDisplay();
    inkplate.setFont(NULL);
    inkplate.setTextSize(2);
    inkplate.setTextColor(BLACK);
    inkplate.setCursor(MARGIN_X, SCREEN_H / 2 - 20);
    inkplate.print("Loading");
    char title[64];
    titleFromFilename(filename, title, sizeof(title));
    inkplate.setCursor(MARGIN_X, SCREEN_H / 2 + 10);
    inkplate.print(title);
    inkplate.display();

    strncpy(openBookName, filename, sizeof(openBookName) - 1);

    if (bookFile.isOpen()) bookFile.close();
    if (!bookFile.open(filename, O_RDONLY)) {
        Serial.printf("Failed to open %s\n", filename);
        return;
    }

    Serial.printf("Paginating %s...\n", filename);
    setReadingFont();
    layoutFile(0, 0);  // build page offset table
    Serial.printf("Done: %d pages\n", pageCount);

    currentPage  = 0;
    partialCount = 0;
    state = STATE_READING;
    drawPage(true);
}

// ══════════════════════════════════════════════════════════════════════════════
// Setup / Loop
// ══════════════════════════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    inkplate.begin();
    inkplate.setRotation(1);  // portrait, power button at top
    inkplate.tsInit(1);

    inkplate.clearDisplay();
    inkplate.setTextSize(2);
    inkplate.setCursor(MARGIN_X, SCREEN_H / 2);
    inkplate.print("Reading SD card...");
    inkplate.display();

    if (!inkplate.sdCardInit()) {
        inkplate.clearDisplay();
        inkplate.setCursor(MARGIN_X, SCREEN_H / 2);
        inkplate.print("SD card error.");
        inkplate.display();
        return;
    }

    scanBooks();
    drawLibrary();
}

// ── Gesture tracking ──────────────────────────────────────────────────────────
#define SWIPE_THRESHOLD   100  // min px of vertical travel to count as a swipe
#define TAP_MAX_DRIFT      60  // max px of total travel to count as a tap

static bool     touchActive = false;
static uint16_t touchStartX = 0, touchStartY = 0;
static uint16_t touchLastX  = 0, touchLastY  = 0;

void handleGesture(int dx, int dy, uint16_t sx) {
    int adx = abs(dx), ady = abs(dy);

    if (ady > SWIPE_THRESHOLD && ady > adx) {
        // ── Vertical swipe ─────────────────────────────────────────────────
        if (dy < 0) {
            // Swipe UP → back to library from anywhere
            if (state == STATE_READING) {
                if (bookFile.isOpen()) bookFile.close();
                state = STATE_LIBRARY;
                drawLibrary();
            }
        }
        // Swipe DOWN reserved for future menu
    } else if (adx < TAP_MAX_DRIFT && ady < TAP_MAX_DRIFT) {
        // ── Tap ────────────────────────────────────────────────────────────
        if (state == STATE_LIBRARY) {
            int visibleRows = (SCREEN_H - MARGIN_Y - FOOTER_H) / LIB_LINE_H;
            if (sx < LEFT_ZONE) {
                if (selectedIdx > 0) {
                    selectedIdx--;
                    if (selectedIdx < scrollOffset) scrollOffset = selectedIdx;
                    drawLibrary(false);
                }
            } else if (sx > RIGHT_ZONE) {
                if (selectedIdx < bookCount - 1) {
                    selectedIdx++;
                    if (selectedIdx >= scrollOffset + visibleRows)
                        scrollOffset = selectedIdx - visibleRows + 1;
                    drawLibrary(false);
                }
            } else {
                if (bookCount > 0) openBook(bookNames[selectedIdx]);
            }

        } else if (state == STATE_READING) {
            if (sx < LEFT_ZONE) {
                if (currentPage > 0) { currentPage--; drawPage(false); }
            } else if (sx > RIGHT_ZONE) {
                if (currentPage < pageCount - 1) {
                    currentPage++;
                    drawPage(false);
                } else {
                    if (bookFile.isOpen()) bookFile.close();
                    state = STATE_LIBRARY;
                    drawLibrary();
                }
            }
        }
    }
}

void loop() {
    unsigned long now = millis();

    if (!inkplate.tsAvailable()) { delay(10); return; }

    uint16_t x[2], y[2];
    uint8_t n = inkplate.tsGetData(x, y);

    if (n > 0) {
        if (!touchActive) {
            touchActive = true;
            touchStartX = x[0]; touchStartY = y[0];
        }
        touchLastX = x[0]; touchLastY = y[0];
    } else if (touchActive) {
        touchActive = false;
        // Gesture complete — only act if cooldown has passed
        if (now - lastActionMs >= POST_ACTION_MS) {
            int dx = (int)touchLastX - (int)touchStartX;
            int dy = (int)touchLastY - (int)touchStartY;
            handleGesture(dx, dy, touchStartX);
        }
    }

    delay(10);
}
