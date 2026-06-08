#include <Inkplate.h>

Inkplate inkplate(INKPLATE_1BIT);

void setup() {
    Serial.begin(115200);
    inkplate.begin();
    inkplate.clearDisplay();
    inkplate.setCursor(100, 350);
    inkplate.setTextSize(3);
    inkplate.print("Hello, e-reader!");
    inkplate.display();
    Serial.println("Display initialized.");
}

void loop() {
}
