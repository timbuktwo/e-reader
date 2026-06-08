#include <Inkplate.h>
#include <SdFat.h>

// --- Display constants (portrait: 758w x 1024h) ---
#define SCREEN_W        758
#define SCREEN_H        1024
#define MARGIN_X        40
#define MARGIN_Y        30
#define FOOTER_H        40
#define LINE_H          36   // px per library list row
#define MAX_BOOKS       50
#define LEFT_ZONE       303  // 0-302 = left 40% of 758px
#define RIGHT_ZONE      454  // 455-757 = right 40%
#define POST_ACTION_MS  150

Inkplate inkplate(INKPLATE_1BIT);

// --- Book list ---
char bookNames[MAX_BOOKS][64];
int  bookCount = 0;
int  selectedIdx = 0;
int  scrollOffset = 0;  // first visible row index

unsigned long lastActionMs = 0;

// --- Helpers ---

// Derive a display title from a filename: strip .txt, replace _ and - with spaces
void titleFromFilename(const char* fname, char* out, int maxLen) {
    strncpy(out, fname, maxLen - 1);
    out[maxLen - 1] = '\0';
    // Strip .txt extension (case-insensitive)
    int len = strlen(out);
    if (len > 4 && strcasecmp(out + len - 4, ".txt") == 0)
        out[len - 4] = '\0';
    // Replace underscores and dashes with spaces
    for (char* p = out; *p; p++)
        if (*p == '_') *p = ' ';
}

// --- SD scan ---
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
            // Skip macOS metadata files (._filename) and hidden files
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

// --- Library UI ---
void drawLibrary() {
    inkplate.clearDisplay();
    inkplate.setTextSize(2);

    if (bookCount == 0) {
        inkplate.setCursor(MARGIN_X, SCREEN_H / 2 - 20);
        inkplate.print("No .txt books found on SD card.");
        inkplate.display();
        return;
    }

    int visibleRows = (SCREEN_H - MARGIN_Y - FOOTER_H) / LINE_H;
    int y = MARGIN_Y;

    for (int i = scrollOffset; i < bookCount && i < scrollOffset + visibleRows; i++) {
        char title[64];
        titleFromFilename(bookNames[i], title, sizeof(title));

        if (i == selectedIdx) {
            // Highlight selected row
            inkplate.fillRect(MARGIN_X - 4, y - 2, SCREEN_W - MARGIN_X * 2 + 8, LINE_H, BLACK);
            inkplate.setTextColor(WHITE);
        } else {
            inkplate.setTextColor(BLACK);
        }

        inkplate.setCursor(MARGIN_X, y + 4);
        inkplate.print(title);
        y += LINE_H;
    }

    // Footer
    inkplate.setTextColor(BLACK);
    inkplate.setTextSize(1);
    inkplate.setCursor(MARGIN_X, SCREEN_H - FOOTER_H + 10);
    char footer[48];
    snprintf(footer, sizeof(footer), "%d book%s  |  tap left/right to scroll  |  tap center to open",
             bookCount, bookCount == 1 ? "" : "s");
    inkplate.print(footer);

    inkplate.display();
    lastActionMs = millis();
}

void setup() {
    Serial.begin(115200);
    inkplate.begin();
    inkplate.setRotation(1);  // portrait mode (758 x 1024)
    inkplate.tsInit(1);

    // Show loading message
    inkplate.clearDisplay();
    inkplate.setTextSize(2);
    inkplate.setCursor(MARGIN_X, SCREEN_H / 2 - 20);
    inkplate.print("Reading SD card...");
    inkplate.display();

    if (!inkplate.sdCardInit()) {
        inkplate.clearDisplay();
        inkplate.setCursor(MARGIN_X, SCREEN_H / 2 - 20);
        inkplate.print("SD card error. Please insert a card with .txt books.");
        inkplate.display();
        Serial.println("SD init failed.");
        return;
    }

    scanBooks();
    Serial.printf("Found %d books.\n", bookCount);
    for (int i = 0; i < bookCount; i++)
        Serial.println(bookNames[i]);

    drawLibrary();
}

void loop() {
    unsigned long now = millis();
    if (now - lastActionMs < POST_ACTION_MS) {
        delay(10);
        return;
    }

    if (!inkplate.tsAvailable()) {
        delay(10);
        return;
    }

    uint16_t x[2], y[2];
    uint8_t n = inkplate.tsGetData(x, y);
    if (n == 0) return;

    int visibleRows = (SCREEN_H - MARGIN_Y - FOOTER_H) / LINE_H;

    if (x[0] < LEFT_ZONE) {
        // Scroll / select up
        if (selectedIdx > 0) {
            selectedIdx--;
            if (selectedIdx < scrollOffset) scrollOffset = selectedIdx;
            drawLibrary();
        }
    } else if (x[0] > RIGHT_ZONE) {
        // Scroll / select down
        if (selectedIdx < bookCount - 1) {
            selectedIdx++;
            if (selectedIdx >= scrollOffset + visibleRows)
                scrollOffset = selectedIdx - visibleRows + 1;
            drawLibrary();
        }
    } else {
        // Centre tap = open selected book (stub for now)
        inkplate.clearDisplay();
        inkplate.setTextSize(2);
        inkplate.setCursor(MARGIN_X, SCREEN_H / 2 - 20);
        inkplate.print("Opening: ");
        char title[64];
        titleFromFilename(bookNames[selectedIdx], title, sizeof(title));
        inkplate.print(title);
        inkplate.display();
        lastActionMs = millis();
        Serial.printf("Open: %s\n", bookNames[selectedIdx]);
    }
}
