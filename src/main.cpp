#include <Inkplate.h>

#define LONG_PRESS_MS  500
#define POST_ACTION_MS 150   // drain touch queue after refresh completes
#define LEFT_ZONE      410   // 0-409 = left 40%
#define RIGHT_ZONE     614   // 615-1023 = right 40%

Inkplate inkplate(INKPLATE_1BIT);

unsigned long lastActionMs = 0;

void printAction(const char* action) {
    Serial.println(action);
    inkplate.clearDisplay();
    inkplate.setCursor(50, 350);
    inkplate.setTextSize(3);
    inkplate.print(action);
    inkplate.display();
    lastActionMs = millis();
}

void setup() {
    Serial.begin(115200);
    inkplate.begin();

    // Initialize touchscreen properly — required before any touch reads
    inkplate.tsInit(1);

    inkplate.clearDisplay();
    inkplate.setCursor(30, 300);
    inkplate.setTextSize(2);
    inkplate.print("Tap left=PREV  right=NEXT");
    inkplate.display();

    Serial.println("Input test ready.");
}

void loop() {
    unsigned long now = millis();

    // Gate: don't read touch while display is still refreshing / cooling down
    if (now - lastActionMs < POST_ACTION_MS) {
        delay(10);
        return;
    }

    // Only read touch data when the touchscreen INT fires (interrupt-driven)
    if (inkplate.tsAvailable()) {
        uint16_t x[2], y[2];
        uint8_t n = inkplate.tsGetData(x, y);
        if (n > 0) {
            if (x[0] < LEFT_ZONE) {
                printAction("PREV");
            } else if (x[0] > RIGHT_ZONE) {
                printAction("NEXT");
            }
            // centre zone: ignored
        }
    }

    delay(10);
}
