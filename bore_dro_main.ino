#include <TFT_eSPI.h>       // LilyGO T-Display-S3 uses TFT_eSPI
#include <Preferences.h>
#include <esp_sleep.h>

// --- LILYGO T-Display-S3 Hardware Pins ---
#define PIN_POWER_ON   15   // Powers the LCD & board peripherals (MUST be HIGH)
#define PIN_LCD_BL     38   // Backlight pin
#define PIN_BAT_VOLT    4   // Onboard battery ADC voltage sense pin

// --- Caliper & Button Pin Mapping ---
// Pins 1 and 2 are easily accessible on the T-Display-S3 pin headers
#define CLK_PIN         1   // Caliper Clock wire (white)
#define DATA_PIN        2   // Caliper Data wire (blue)
#define BTN_PIN        14   // Onboard right-side button (or external button to GND)

// --- Auto-Sleep Config ---
// Shuts down 30 seconds after caliper signal stops (only when running on battery)
#define CALIPER_OFF_TIMEOUT_MS 30000 

// --- Caliper Connection Monitoring ---
#define CALIPER_TIMEOUT_MS 1000       // 1 second without data means disconnected
unsigned long lastCaliperFrameTime = 0; 
bool caliperConnected = true;

// --- Preset Base Diameters ---
const float baseSizes[] = {28.00, 54.00, 82.00};
int currentBaseIndex = 0; 

float activeOffset = 0.0;
float currentRawDistance = 0.0;
float displayedDiameter = 0.0;

// --- Signal Variables ---
volatile long rawData = 0;
volatile int bitIndex = 0;
volatile unsigned long lastClockMicros = 0;
volatile bool frameComplete = false;
long capturedData = 0;

// --- Button Handling Variables ---
unsigned long btnPressTime = 0;
bool btnStatePrevious = HIGH;
bool saveTriggered = false; 

// --- Periodic Display Refresh (keeps battery/clock updated) ---
unsigned long lastDisplayRefresh = 0;
unsigned long savedBannerUntil = 0; // For centered on-screen confirmation banner

// Display instance
TFT_eSPI tft = TFT_eSPI();
// Sprite for flicker-free double-buffered rendering (320x170)
TFT_eSprite spr = TFT_eSprite(&tft);
Preferences preferences; 

// --- Clock Interrupt Service Routine ---
void IRAM_ATTR clockISR() {
  unsigned long now = micros();
  
  if (now - lastClockMicros > 2000) {
    bitIndex = 0;
    rawData = 0;
  }
  lastClockMicros = now;

  if (bitIndex < 24) {
    int bitVal = digitalRead(DATA_PIN);
    if (bitVal == HIGH) {
      rawData |= (1L << bitIndex);
    }
    bitIndex++;
    if (bitIndex == 24) {
      capturedData = rawData;
      frameComplete = true;
    }
  }
}

// --- Battery Voltage Measurement ---
float getBatteryVoltage() {
  // Read millivolts using calibrated internal eFuse
  uint32_t mv = analogReadMilliVolts(PIN_BAT_VOLT);
  // Board uses a 1:1 (100k/100k) divider, so multiply by 2
  float voltage = (mv * 2.0f) / 1000.0f;
  return voltage;
}

// --- Deep Sleep Function ---
void enterDeepSleep() {
  // Turn off display visually
  spr.fillSprite(TFT_BLACK);
  spr.pushSprite(0, 0);

  // Power down screen backlight and onboard power rail
  digitalWrite(PIN_LCD_BL, LOW);
  digitalWrite(PIN_POWER_ON, LOW);

  // Configure Button (GPIO 14) to wake the ESP32-S3 from sleep on LOW
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_PIN, 0);

  esp_deep_sleep_start();
}

// --- Battery Icon & Status Renderer ---
void drawBattery(int x, int y, float voltage) {
  // USB charging pushes the voltage divider rail >= 4.25V
  bool isCharging = (voltage >= 4.25f);

  // Outer border & positive terminal nub
  spr.drawRoundRect(x, y, 26, 13, 2, TFT_SILVER);
  spr.fillRect(x + 26, y + 3, 2, 7, TFT_SILVER);

  if (isCharging) {
    // When connected to USB power, animate charging bars
    int animStep = (millis() / 350) % 4; // changes every 350ms
    int animWidths[] = {0, 7, 14, 22};
    int fillW = animWidths[animStep];

    // Dim background cavity
    spr.fillRect(x + 2, y + 2, 22, 9, TFT_BLACK);

    // Animated rising green fill
    if (fillW > 0) {
      spr.fillRect(x + 2, y + 2, fillW, 9, TFT_GREEN);
    }

    // Small yellow lightning bolt over the icon
    spr.drawLine(x + 13, y + 1, x + 10, y + 6, TFT_YELLOW);
    spr.drawLine(x + 10, y + 6, x + 15, y + 6, TFT_YELLOW);
    spr.drawLine(x + 15, y + 6, x + 12, y + 11, TFT_YELLOW);

    // Text: Charging in Green
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(TFT_GREEN, TFT_BLACK);
    spr.drawString("Charging", x - 5, y - 1, 2);
  } else {
    // Discharging: Approximate percentage for Li-ion 3.30V (0%) to 4.18V (100%)
    int pct = (int)((voltage - 3.30f) / (4.18f - 3.30f) * 100.0f);
    pct = constrain(pct, 0, 100);

    int fillW = map(pct, 0, 100, 0, 22);
    uint16_t color = TFT_GREEN;
    if (pct < 20) color = TFT_RED;
    else if (pct < 50) color = TFT_YELLOW;

    if (fillW > 0) {
      spr.fillRect(x + 2, y + 2, fillW, 9, color);
    }

    // Text: Voltage reading (e.g. "3.8V")
    spr.setTextDatum(TR_DATUM);
    spr.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    spr.drawString(String(voltage, 1) + "V", x - 5, y - 1, 2);
  }
}

void setup() {
  Serial.begin(115200);
  lastCaliperFrameTime = millis();

  // 1. Enable board power rails and LCD backlight for T-Display-S3
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  delay(100); // Allow power rail and ST7789 driver to stabilize

  // 2. Initialize ST7789 Display via TFT_eSPI
  tft.init();
  tft.setRotation(3); // Landscape mode flipped 180° (320 wide x 170 high)
  tft.fillScreen(TFT_BLACK);

  // 3. Create full-screen 16-bit color sprite for crisp, flicker-free rendering
  spr.createSprite(320, 170);
  spr.setTextDatum(TL_DATUM);

  // Splash Screen: Sleek modern industrial layout with animated progress bar
  unsigned long splashStart = millis();
  const unsigned long splashDuration = 5500; // ~5.5 seconds hold

  while (millis() - splashStart < splashDuration) {
    spr.fillSprite(TFT_BLACK);

    // Subtle outer frame with cyan accent corners
    spr.drawRoundRect(8, 8, 304, 154, 4, TFT_DARKGREY);
    spr.drawFastHLine(8, 8, 30, TFT_CYAN);
    spr.drawFastVLine(8, 8, 20, TFT_CYAN);
    spr.drawFastHLine(282, 8, 30, TFT_CYAN);
    spr.drawFastVLine(311, 8, 20, TFT_CYAN);
    spr.drawFastHLine(8, 161, 30, TFT_CYAN);
    spr.drawFastVLine(8, 142, 20, TFT_CYAN);
    spr.drawFastHLine(282, 161, 30, TFT_CYAN);
    spr.drawFastVLine(311, 142, 20, TFT_CYAN);

    // Title: BORE DRO
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(TFT_CYAN, TFT_BLACK);
    spr.drawString("BORE DRO", 160, 48, 4);

    // Subtitle / Tagline
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.drawString("PRECISION BORING READOUT", 160, 78, 2);

    spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
    spr.drawString("ESP32-S3  |  v2.0", 160, 98, 2);

    // Animated loading progress bar
    float progress = (float)(millis() - splashStart) / (float)splashDuration;
    if (progress > 1.0f) progress = 1.0f;
    int barW = 200;
    int barX = 60;
    int barY = 124;

    spr.drawRoundRect(barX, barY, barW, 8, 3, TFT_DARKGREY);
    int fillPixels = (int)(progress * (barW - 4));
    if (fillPixels > 0) {
      spr.fillRoundRect(barX + 2, barY + 2, fillPixels, 4, 2, TFT_CYAN);
    }

    spr.pushSprite(0, 0);
    delay(20);
  }

  // 4. Load Saved Settings from Flash
  preferences.begin("bore_dro", false); 
  currentBaseIndex = preferences.getInt("base_idx", 0);      
  activeOffset     = preferences.getFloat("offset", 0.0);   
  preferences.end();

  // 5. Initialize Pins & Interrupts
  pinMode(CLK_PIN, INPUT_PULLDOWN);
  pinMode(DATA_PIN, INPUT_PULLDOWN);
  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(PIN_BAT_VOLT, INPUT);

  attachInterrupt(digitalPinToInterrupt(CLK_PIN), clockISR, FALLING);

  // Draw initial UI
  displayedDiameter = currentRawDistance + activeOffset;
  updateDisplay(displayedDiameter, baseSizes[currentBaseIndex], false);
}

void loop() {
  handleButton();

  if (frameComplete) {
    frameComplete = false;

    // Reset fallback timers because valid data arrived
    lastCaliperFrameTime = millis();
    caliperConnected = true;

    bool isNegative = (capturedData & (1L << 20));
    long rawValue = capturedData & 0xFFFFF; 

    if (isNegative) {
      rawValue = -rawValue;
    }

    currentRawDistance = (rawValue / 100.00); 
    displayedDiameter = currentRawDistance + activeOffset;

    lastDisplayRefresh = millis();
    updateDisplay(displayedDiameter, baseSizes[currentBaseIndex], true);
  }

  // Handle Caliper Disconnect State smoothly
  if (caliperConnected && (millis() - lastCaliperFrameTime > CALIPER_TIMEOUT_MS)) {
    caliperConnected = false;
    updateDisplay(0.00, baseSizes[currentBaseIndex], false);
  }

  // Periodic refresh (every 200ms) to keep battery status, timer, and banners updated
  if (millis() - lastDisplayRefresh > 200) {
    lastDisplayRefresh = millis();
    updateDisplay(caliperConnected ? displayedDiameter : 0.00, baseSizes[currentBaseIndex], caliperConnected);
  }

  // --- Auto-Sleep Logic ---
  float currentVolt = getBatteryVoltage();
  bool isCharging = (currentVolt >= 4.25f);

  if (!isCharging) {
    // On battery: Go to sleep 30 seconds after caliper signal stops
    if (millis() - lastCaliperFrameTime > CALIPER_OFF_TIMEOUT_MS) {
      enterDeepSleep();
    }
  }
}

void handleButton() {
  bool btnStateCurrent = digitalRead(BTN_PIN);
  unsigned long now = millis();

  if (btnStateCurrent == LOW || btnStatePrevious == LOW) {
    lastCaliperFrameTime = now; // Keep awake while button is actively touched
  }

  if (btnStatePrevious == HIGH && btnStateCurrent == LOW) {
    btnPressTime = now;
    saveTriggered = false; 
  }

  // Long Press (>= 1000ms): Zero / Calibrate offset
  if (btnStateCurrent == LOW && !saveTriggered) {
    if (now - btnPressTime >= 1000) {
      saveTriggered = true; 

      activeOffset = baseSizes[currentBaseIndex] - currentRawDistance;

      preferences.begin("bore_dro", false);
      preferences.putFloat("offset", activeOffset);
      preferences.end();

      savedBannerUntil = millis() + 1500; // Display confirmation banner for 1.5 seconds

      displayedDiameter = currentRawDistance + activeOffset;
      updateDisplay(displayedDiameter, baseSizes[currentBaseIndex], caliperConnected);
    }
  }

  // Short Press: Cycle Base Target
  if (btnStatePrevious == LOW && btnStateCurrent == HIGH) {
    unsigned long duration = now - btnPressTime;

    if (duration > 50 && duration < 1000 && !saveTriggered) {
      currentBaseIndex = (currentBaseIndex + 1) % 3; 
      
      preferences.begin("bore_dro", false);
      preferences.putInt("base_idx", currentBaseIndex);
      preferences.end();
      
      displayedDiameter = currentRawDistance + activeOffset;
      updateDisplay(displayedDiameter, baseSizes[currentBaseIndex], caliperConnected);
    }
  }

  btnStatePrevious = btnStateCurrent;
}

void updateDisplay(float value, float activeBase, bool showNumbers) {
  spr.fillSprite(TFT_BLACK);

  // --- Top Status Bar ---
  // Target Base Size
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
  spr.drawString("TARGET:", 10, 10, 2);

  spr.setTextColor(TFT_YELLOW, TFT_BLACK);
  spr.drawFloat(activeBase, 1, 72, 10, 2);
  spr.drawString("mm", 112, 10, 2);

  // Caliper Link status indicator (dot + label)
  spr.fillCircle(160, 17, 4, showNumbers ? TFT_GREEN : TFT_RED);
  spr.setTextColor(showNumbers ? TFT_GREEN : TFT_RED, TFT_BLACK);
  spr.drawString(showNumbers ? "LINK" : "WAIT", 168, 10, 2);

  // Battery & Charging icon on top right
  float battV = getBatteryVoltage();
  drawBattery(284, 11, battV);

  // Accent divider line
  spr.drawFastHLine(10, 34, 300, TFT_NAVY);

  // --- Middle Area: Centered Banner, Numbers, or Disconnect Prompt ---
  if (millis() < savedBannerUntil) {
    // Perfectly centered confirmation banner without wiping out top or bottom bars
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(TFT_GREEN, TFT_BLACK);
    spr.drawString("CALIBRATED!", 160, 72, 4);

    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.drawString("Offset Saved to Memory", 160, 102, 2);
  } else if (showNumbers) {
    // Large 7-segment readout
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setTextDatum(TR_DATUM);
    spr.drawFloat(value, 2, 230, 52, 7);

    // Units label
    spr.setTextDatum(TL_DATUM);
    spr.setTextColor(TFT_CYAN, TFT_BLACK);
    spr.drawString("mm", 240, 82, 4);
  } else {
    // Standby / Disconnected view
    spr.setTextDatum(MC_DATUM);
    spr.setTextColor(TFT_ORANGE, TFT_BLACK);
    spr.drawString("WAITING FOR SIGNAL...", 160, 85, 4);
  }

  // --- Bottom Left: Raw reading or helper text ---
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
  if (showNumbers) {
    spr.drawString("Raw: " + String(currentRawDistance, 2) + " mm", 15, 142, 2);
  } else {
    spr.drawString("Check Caliper Link", 15, 142, 2);
  }

  // --- Bottom Right: Sleep Countdown / Status Timer ---
  spr.setTextDatum(TR_DATUM);
  bool isCharging = (battV >= 4.25f);

  if (isCharging) {
    // While charging, sleep is disabled
    spr.setTextColor(TFT_DARKGREEN, TFT_BLACK);
    spr.drawString("PWR ON", 305, 142, 2);
  } else if (!showNumbers) {
    // Only show sleep countdown when caliper is disconnected / waiting for signal
    unsigned long elapsed = millis() - lastCaliperFrameTime;
    int secondsLeft = (CALIPER_OFF_TIMEOUT_MS > elapsed) ? (int)((CALIPER_OFF_TIMEOUT_MS - elapsed) / 1000) : 0;

    if (secondsLeft <= 5) {
      spr.setTextColor(TFT_RED, TFT_BLACK);       // Warning: about to sleep
    } else if (secondsLeft <= 10) {
      spr.setTextColor(TFT_ORANGE, TFT_BLACK);    // Low time remaining
    } else {
      spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
    }
    spr.drawString("SLEEP: " + String(secondsLeft) + "s", 305, 142, 2);
  }
  // During active measurement: bottom right is left completely clean and empty

  // Push the completed frame to the LCD
  spr.pushSprite(0, 0);
}