#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h> // Keep this include, it contains the SH1107 driver
#include <Preferences.h> 

// --- Display Config ---
// Natively, this is a 64x128 vertical panel. We rotate it in code to be horizontal.
#define SCREEN_WIDTH  64  
#define SCREEN_HEIGHT 128 
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C

// Initialize using the correct SH1107 class
Adafruit_SH1107 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Preferences preferences; 

// --- C3 Super Mini Pin Mapping ---
#define SDA_PIN   8   
#define SCL_PIN   9   
#define CLK_PIN   7   // Caliper Clock wire (white)
#define DATA_PIN  6   // Caliper Data wire (blue)
#define BTN_PIN   4   // Preset/Zero/Wake button

// --- Auto-Sleep Config ---
// Shuts down 30 seconds after the caliper stops sending clock/data pulses
#define CALIPER_OFF_TIMEOUT_MS 30000 

// --- Caliper Connection Monitoring ---
#define CALIPER_TIMEOUT_MS 1000       // 1 second without data means disconnected
unsigned long lastCaliperFrameTime = 0; 
bool caliperConnected = true;         // Start true so it draws immediately if connected

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

void enterDeepSleep() {
  display.clearDisplay();
  display.display(); 
  esp_deep_sleep_enable_gpio_wakeup((1ULL << BTN_PIN), ESP_GPIO_WAKEUP_GPIO_LOW); 
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  lastCaliperFrameTime = millis();

  // Initialize Display for C3 Super Mini (SDA=8, SCL=9)
  Wire.begin(SDA_PIN, SCL_PIN); 
  
  // SH1107 uses standard begin sequence
  if (!display.begin(SCREEN_ADDRESS, true)) {
    Serial.println(F("SH1107 OLED init failed!"));
    for (;;);
  }

  // Rotation 1 or 3 flips the 64x128 vertical panel into a 128x64 landscape screen
  display.setRotation(1); 

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(16, 24); 
  display.println(F("BORE DRO"));
  display.display();

  // Load Saved Settings from Flash
  preferences.begin("bore_dro", false); 
  currentBaseIndex = preferences.getInt("base_idx", 0);      
  activeOffset     = preferences.getFloat("offset", 0.0);   
  preferences.end();

  delay(800);

  // Initialize Pins & Interrupts
  pinMode(CLK_PIN, INPUT_PULLDOWN);
  pinMode(DATA_PIN, INPUT_PULLDOWN);
  pinMode(BTN_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(CLK_PIN), clockISR, FALLING);
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

    updateDisplay(displayedDiameter, baseSizes[currentBaseIndex], true);
  }

  // Handle Caliper Disconnect State smoothly without loops stalling
  if (caliperConnected && (millis() - lastCaliperFrameTime > CALIPER_TIMEOUT_MS)) {
    caliperConnected = false;
    updateDisplay(0.00, baseSizes[currentBaseIndex], false);
  }

  // Go to sleep 30 seconds after the caliper stops sending data
  if (millis() - lastCaliperFrameTime > CALIPER_OFF_TIMEOUT_MS) {
    enterDeepSleep();
  }
}

void handleButton() {
  bool btnStateCurrent = digitalRead(BTN_PIN);
  unsigned long now = millis();

  if (btnStateCurrent == LOW || btnStatePrevious == LOW) {
    lastCaliperFrameTime = now; // Keep awake while button is held
  }

  if (btnStatePrevious == HIGH && btnStateCurrent == LOW) {
    btnPressTime = now;
    saveTriggered = false; 
  }

  if (btnStateCurrent == LOW && !saveTriggered) {
    if (now - btnPressTime >= 1000) {
      saveTriggered = true; 

      activeOffset = baseSizes[currentBaseIndex] - currentRawDistance;

      preferences.begin("bore_dro", false);
      preferences.putFloat("offset", activeOffset);
      preferences.end();

      display.clearDisplay();
      display.setCursor(28, 24);
      display.setTextSize(2);
      display.print("SAVED!");
      display.display();
      
      delay(1000); 
      
      // Return to the correct mode configuration after the save banner drops
      displayedDiameter = currentRawDistance + activeOffset;
      updateDisplay(displayedDiameter, baseSizes[currentBaseIndex], caliperConnected);
    }
  }

  if (btnStatePrevious == LOW && btnStateCurrent == HIGH) {
    unsigned long duration = now - btnPressTime;

    if (duration > 50 && duration < 1000 && !saveTriggered) {
      currentBaseIndex = (currentBaseIndex + 1) % 3; 
      
      preferences.begin("bore_dro", false);
      preferences.putInt("base_idx", currentBaseIndex);
      preferences.end();
      
      // Instantly redraw screen so the base target updates when disconnected
      displayedDiameter = currentRawDistance + activeOffset;
      updateDisplay(displayedDiameter, baseSizes[currentBaseIndex], caliperConnected);
    }
  }

  btnStatePrevious = btnStateCurrent;
}

// Simple flag (showNumbers) to cleanly manage the readout layout change
void updateDisplay(float value, float activeBase, bool showNumbers) {
  display.clearDisplay();

  // Header row remains solid across states
  display.setTextSize(1);
  display.setCursor(4, 6);
  display.print(F("BASE TARGET: "));
  display.print(activeBase, 1);
  display.print(F(" mm"));
  
  // Separation accent line
  display.drawFastHLine(0, 20, 128, SH110X_WHITE);

  if (showNumbers) {
    // Standard running readout numbers
    display.setTextSize(2);
    display.setCursor(4, 34);

    if (value >= 0 && value < 100.0) display.print(" ");

    display.print(value, 2);
    display.setTextSize(1);
    display.print(" mm");
  } else {
    // Reverted plain text when signal lines go quiet
    display.setTextSize(2);
    display.setCursor(16, 34); 
    display.println(F("BORE DRO"));
  }

  display.display();
}
