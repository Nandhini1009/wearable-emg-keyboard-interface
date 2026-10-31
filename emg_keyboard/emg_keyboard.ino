/*
  Wearable EMG Keyboard Interface (ESP32)

  Hands-free typing:
    - Wrist tilt (MPU6050) moves the highlighted key across an on-screen keyboard.
    - A muscle contraction (EMG sensor) types the highlighted key.
    - An I2C OLED shows the highlighted key and the typed text.

  On start-up the EMG signal is calibrated for 5 seconds. The highest reading seen
  during that time is the user's peak, and the typing threshold is set to 70% of it.

  Libraries (Arduino Library Manager):
    Adafruit MPU6050, Adafruit Unified Sensor, Adafruit SSD1306, Adafruit GFX
*/

#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>

// ===== Pin definitions =====
#define EMG_PIN   34   // EMG sensor analog output
#define MPU_SDA   21   // MPU6050 I2C SDA
#define MPU_SCL   22   // MPU6050 I2C SCL
#define OLED_SDA  25   // OLED I2C SDA (separate I2C bus from the MPU6050)
#define OLED_SCL  26   // OLED I2C SCL

// ===== Display =====
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64

Adafruit_MPU6050 mpu;
TwoWire OLEDWire = TwoWire(1);  // second I2C bus for the OLED
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &OLEDWire, -1);

// ===== EMG calibration settings =====
const unsigned long READY_MS = 2000;         // time to get ready before calibration starts
const unsigned long CALIBRATION_MS = 5000;   // calibration length
const float THRESHOLD_FRACTION = 0.70;       // threshold = 70% of the peak reading
const int FALLBACK_THRESHOLD = 1000;         // fixed threshold used if calibration fails
const int MIN_VALID_PEAK = 300;              // a lower peak is treated as "no muscle signal seen"
int emgThreshold = FALLBACK_THRESHOLD;

// ===== Tilt settings =====
const float TILT_LIMIT = 5.0;       // acceleration (m/s^2) needed to count as a tilt
const float TILT_SMOOTHING = 0.3;   // 0 to 1. Lower = smoother, but slower to react
float smoothX = 0, smoothY = 0;

// ===== Debounce / timing settings =====
const unsigned long MOVE_COOLDOWN_MS = 400;     // minimum time between cursor moves
const unsigned long SELECT_COOLDOWN_MS = 500;   // minimum time between typed characters
const unsigned long LOOP_DELAY_MS = 20;
const unsigned long DEBUG_INTERVAL_MS = 300;
unsigned long lastMoveTime = 0;
unsigned long lastSelectTime = 0;
unsigned long lastDebugTime = 0;

// ===== Keyboard layout =====
const char keyboard[3][10] = {
  {'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'},
  {'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ';'},
  {'Z', 'X', 'C', 'V', 'B', 'N', 'M', ',', '.', '/'}
};
int row = 0, col = 0;
String typedText = "";
const int MAX_TYPED_CHARS = 16;   // only the latest characters fit on the OLED

// Function prototypes
void showMessage(const char *line1, const char *line2 = "", const char *line3 = "");
void calibrateEMG();
void moveCursor(int rowChange, int colChange);
void typeSelectedChar();
void updateOLED();

void setup() {
  Serial.begin(115200);

  pinMode(EMG_PIN, INPUT);

  // MPU6050
  Wire.begin(MPU_SDA, MPU_SCL);
  if (!mpu.begin()) {
    Serial.println("MPU6050 not found!");
    while (1) delay(10);
  }

  // OLED on its own I2C pins
  OLEDWire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found!");
    while (1) delay(10);
  }
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  showMessage("System Ready");
  delay(1000);

  calibrateEMG();

  // Start the tilt filter from the current wrist position
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  smoothX = a.acceleration.x;
  smoothY = a.acceleration.y;

  Serial.println("MPU6050 & EMG Initialized!");
  updateOLED();
}

void loop() {
  unsigned long now = millis();
  int emgValue = analogRead(EMG_PIN);

  // Read the wrist tilt and smooth out small tremors
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  smoothX += TILT_SMOOTHING * (a.acceleration.x - smoothX);
  smoothY += TILT_SMOOTHING * (a.acceleration.y - smoothY);

  // Debug output
  if (now - lastDebugTime >= DEBUG_INTERVAL_MS) {
    lastDebugTime = now;
    Serial.print("EMG: ");
    Serial.print(emgValue);
    Serial.print(" | Threshold: ");
    Serial.print(emgThreshold);
    Serial.print(" | Selected: ");
    Serial.println(keyboard[row][col]);
  }

  // Tilt moves the cursor (one move per cooldown period)
  if (now - lastMoveTime >= MOVE_COOLDOWN_MS) {
    if (smoothX > TILT_LIMIT)       moveCursor(0, 1);    // right
    else if (smoothX < -TILT_LIMIT) moveCursor(0, -1);   // left
    else if (smoothY > TILT_LIMIT)  moveCursor(-1, 0);   // up
    else if (smoothY < -TILT_LIMIT) moveCursor(1, 0);    // down
  }

  // A muscle contraction above the threshold types the selected character
  if (emgValue > emgThreshold && now - lastSelectTime >= SELECT_COOLDOWN_MS) {
    typeSelectedChar();
  }

  delay(LOOP_DELAY_MS);
}

// Records the highest EMG reading for 5 seconds and sets the threshold to 70% of it
void calibrateEMG() {
  showMessage("EMG calibration", "Relax your arm...");
  delay(READY_MS);

  int peak = 0;
  int lastSecondsLeft = -1;
  unsigned long start = millis();

  while (millis() - start < CALIBRATION_MS) {
    int value = analogRead(EMG_PIN);
    if (value > peak) peak = value;

    int secondsLeft = (CALIBRATION_MS - (millis() - start) + 999) / 1000;
    if (secondsLeft != lastSecondsLeft) {
      lastSecondsLeft = secondsLeft;
      char timeLine[24];
      snprintf(timeLine, sizeof(timeLine), "Time left: %d s", secondsLeft);
      showMessage("Flex muscle now!", "Repeat hard flexes", timeLine);
    }
    delay(2);
  }

  char line2[24];
  char line3[24];
  if (peak >= MIN_VALID_PEAK) {
    emgThreshold = (int)(peak * THRESHOLD_FRACTION);
    snprintf(line2, sizeof(line2), "Peak: %d", peak);
    snprintf(line3, sizeof(line3), "Threshold: %d", emgThreshold);
    showMessage("Calibration done", line2, line3);
  } else {
    emgThreshold = FALLBACK_THRESHOLD;
    snprintf(line3, sizeof(line3), "Using fixed: %d", emgThreshold);
    showMessage("No EMG signal seen", "Check electrodes", line3);
  }

  Serial.print("EMG peak: ");
  Serial.print(peak);
  Serial.print(" | Threshold: ");
  Serial.println(emgThreshold);
  delay(2000);
}

void moveCursor(int rowChange, int colChange) {
  row += rowChange;
  col += colChange;

  // Wrap around the edges
  if (row < 0) row = 2;
  if (row > 2) row = 0;
  if (col < 0) col = 9;
  if (col > 9) col = 0;

  lastMoveTime = millis();

  Serial.print("Selected: ");
  Serial.println(keyboard[row][col]);

  updateOLED();
}

void typeSelectedChar() {
  char selectedChar = keyboard[row][col];
  Serial.print("Typed: ");
  Serial.println(selectedChar);

  typedText += selectedChar;
  if ((int)typedText.length() > MAX_TYPED_CHARS) {
    typedText.remove(0, 1);
  }

  lastSelectTime = millis();
  updateOLED();
}

void showMessage(const char *line1, const char *line2, const char *line3) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(line1);
  display.setCursor(0, 16);
  display.println(line2);
  display.setCursor(0, 32);
  display.println(line3);
  display.display();
}

void updateOLED() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Selected: ");
  display.println(keyboard[row][col]);

  display.setCursor(0, 20);
  display.print("Typed: ");
  display.println(typedText);

  display.display();
}
