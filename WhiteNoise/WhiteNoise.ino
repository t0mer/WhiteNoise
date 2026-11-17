#include "RF24.h"
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_bt.h"
#include "esp_wifi.h"

// === OLED Display (0.96" SSD1306, I2C) ===
#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_RESET   -1      // no reset pin on most 0.96" I2C modules
#define OLED_ADDR    0x3C    // common default; some modules use 0x3D
#define OLED_SDA     21      // I2C data  (free pin on this wiring)
#define OLED_SCL     22      // I2C clock (free pin on this wiring)

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);

// Scrolling text log mirrored from the serial console.
// 128x64 with the default 6x8 font = 21 columns x 8 rows.
#define LOG_ROWS 8
#define LOG_COLS 21
String logBuffer[LOG_ROWS];
int logCount = 0;
bool displayReady = false;

// === Channel Definitions ===
byte bluetooth_even_channels[] = {
    2,  4,  6,  8,  10, 12, 14, 16, 18, 20,
    22, 24, 26, 28, 30, 32, 34, 36, 38, 40,
    42, 44, 46, 48, 50, 52, 54, 56, 58, 60,
    62, 64, 66, 68, 70, 72, 74, 76, 78, 80
};
byte bluetooth_odd_channels[] = {
    1,  3,  5,  7,  9,  11, 13, 15, 17, 19,
    21, 23, 25, 27, 29, 31, 33, 35, 37, 39,
    41, 43, 45, 47, 49, 51, 53, 55, 57, 59,
    61, 63, 65, 67, 69, 71, 73, 75, 77, 79
};
byte wifi_channels[] = {
    6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18,
    22, 24, 26, 28,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44,
    46, 48, 50, 52,
    55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68
};
byte ble_channels[] = {1, 2, 3, 25, 26, 27, 79, 80, 81};
byte usb_channels[] = {40, 50, 60};
byte video_channels[] = {70, 75, 80};
byte rc_channels[] = {1, 3, 5, 7};
byte full_channels[] = {
    1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
    43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84,
    85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100
};

const int num_bluetooth_even = sizeof(bluetooth_even_channels) / sizeof(bluetooth_even_channels[0]);
const int num_bluetooth_odd = sizeof(bluetooth_odd_channels) / sizeof(bluetooth_odd_channels[0]);
const int num_wifi = sizeof(wifi_channels) / sizeof(wifi_channels[0]);
const int num_ble = sizeof(ble_channels) / sizeof(ble_channels[0]);
const int num_usb = sizeof(usb_channels) / sizeof(usb_channels[0]);
const int num_video = sizeof(video_channels) / sizeof(video_channels[0]);
const int num_rc = sizeof(rc_channels) / sizeof(rc_channels[0]);
const int num_full = sizeof(full_channels) / sizeof(full_channels[0]);

// === Radio Instances ===
// HSPI Bus (Modules 1 & 2)
RF24 radio_hspi1(26, 15); // CE=26, CSN=15 (Module 1)
RF24 radio_hspi2(27, 25); // CE=27, CSN=25 (Module 2)
// VSPI Bus (Modules 3 & 4)
RF24 radio_vspi1(4, 2);   // CE=4,  CSN=2  (Module 3)
RF24 radio_vspi2(16, 17); // CE=16, CSN=17 (Module 4)

// === Start/Stop Button ===
#define BUTTON_PIN 32          // momentary push button to GND (uses internal pull-up)
const unsigned long debounceDelay = 50; // ms
int lastButtonReading = HIGH;  // raw reading from the previous loop
int buttonState = HIGH;        // debounced state (HIGH = released, LOW = pressed)
unsigned long lastDebounceTime = 0;

// === RGB Status LED ===
#define LED_RED_PIN      33    // red leg (via ~220-330 ohm resistor)
#define LED_GREEN_PIN    5     // green leg (via ~220-330 ohm resistor)
#define RGB_COMMON_ANODE false // set true for a common-anode LED (shared leg to 3.3V)
const unsigned long ledBlinkInterval = 300; // ms, red blink rate while active
unsigned long lastLedToggle = 0;
bool ledBlinkOn = false;

// === State Variables ===
byte ch[4] = {45, 45, 45, 45}; // Current channel for each module
char mode = 'r';               // Default mode: random Bluetooth hopping
bool jammersActive = false;    // Flag for running state

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  updateStatusLed();
  initDisplay();
  printWelcomeMessage();
}

// === RGB Status LED ===

void setLed(bool red, bool green) {
  if (RGB_COMMON_ANODE) { red = !red; green = !green; }
  digitalWrite(LED_RED_PIN, red ? HIGH : LOW);
  digitalWrite(LED_GREEN_PIN, green ? HIGH : LOW);
}

// Set the LED to match the current state: blinking red while active
// (starting in the on phase), solid green while idle.
void updateStatusLed() {
  if (jammersActive) {
    ledBlinkOn = true;
    lastLedToggle = millis();
    setLed(true, false);
  } else {
    setLed(false, true);
  }
}

// Non-blocking blink driver, called every loop. Toggles the red LED while
// active; the idle (solid green) state is set on transition in updateStatusLed.
void handleStatusLed() {
  if (!jammersActive) return;
  unsigned long now = millis();
  if (now - lastLedToggle >= ledBlinkInterval) {
    lastLedToggle = now;
    ledBlinkOn = !ledBlinkOn;
    setLed(ledBlinkOn, false);
  }
}

void loop() {
  handleSerialInput();
  handleButton();
  handleStatusLed();
  if (jammersActive) runCurrentMode();
}

// === Physical Button ===

// Debounced, edge-triggered: a press toggles between start and stop.
void handleButton() {
  int reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
  }
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) { // transitioned to pressed
        toggleJammers();
      }
    }
  }
  lastButtonReading = reading;
}

void toggleJammers() {
  if (jammersActive) stopJammers();
  else startJammers();
}

void startJammers() {
  if (!jammersActive) {
    initializeJammers();
    jammersActive = true;
    updateStatusLed();
    logLine("Jammers activated and running");
  } else {
    logLine("Jammers are already active");
  }
}

void stopJammers() {
  if (jammersActive) {
    deactivateJammers();
    jammersActive = false;
    updateStatusLed();
    logLine("Jammers deactivated");
  } else {
    logLine("Jammers are already inactive");
  }
}

// === Display / Logging ===

void initDisplay() {
  Wire.begin(OLED_SDA, OLED_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    displayReady = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setTextWrap(false);
    display.display();
  } else {
    Serial.println("OLED init failed (check wiring / I2C address)");
  }
}

// Push a single row into the ring buffer, scrolling older rows up.
void pushRow(const String &row) {
  if (logCount < LOG_ROWS) {
    logBuffer[logCount++] = row;
  } else {
    for (int i = 1; i < LOG_ROWS; i++) logBuffer[i - 1] = logBuffer[i];
    logBuffer[LOG_ROWS - 1] = row;
  }
}

// Wrap a logical line to the display width before pushing it.
void appendRow(String row) {
  if (row.length() == 0) { pushRow(""); return; }
  while (row.length() > LOG_COLS) {
    pushRow(row.substring(0, LOG_COLS));
    row = row.substring(LOG_COLS);
  }
  pushRow(row);
}

void renderLog() {
  if (!displayReady) return;
  display.clearDisplay();
  for (int i = 0; i < logCount; i++) {
    display.setCursor(0, i * 8);
    display.print(logBuffer[i]);
  }
  display.display();
}

// Mirror a message to both the serial console and the OLED.
// Handles embedded newlines so multi-line messages scroll correctly.
void logLine(const String &msg) {
  Serial.println(msg);
  if (!displayReady) return;
  int start = 0;
  while (true) {
    int nl = msg.indexOf('\n', start);
    String piece = (nl == -1) ? msg.substring(start) : msg.substring(start, nl);
    appendRow(piece);
    if (nl == -1) break;
    start = nl + 1;
  }
  renderLog();
}

void printWelcomeMessage() {
  logLine("\nRF Jammer Controller Ready for 4 nRF24 Modules");
  logLine("System is in standby mode");
  printModeHelp();
  logLine("Send 'start' to initialize jammers");
  logLine("Send 'stop' to deactivate jammers");
}

void handleSerialInput() {
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.equalsIgnoreCase("start")) {
      startJammers();
    }
    else if (input.equalsIgnoreCase("stop")) {
      stopJammers();
    }
    else if (input.length() == 1 && isValidMode(input.charAt(0))) {
      mode = input.charAt(0);
      logLine("Mode changed to: " + modeName(mode));
      if (jammersActive) {
        logLine("(Change will take effect immediately)");
      }
    }
    else if (input.equalsIgnoreCase("help") || input.equals("?")) {
      printModeHelp();
    }
    else {
      logLine("Unknown command. Send 'help' for options.");
    }
  }
}

void initializeJammers() {
  // Disable ESP32 Bluetooth and Wi-Fi
  esp_bt_controller_deinit();
  esp_wifi_stop();
  esp_wifi_deinit();
  esp_wifi_disconnect();

  // Initialize HSPI (Modules 1 & 2)
  SPIClass *hspi = new SPIClass(HSPI);
  hspi->begin(14, 12, 13, -1); // SCK=14, MISO=12, MOSI=13
  if (radio_hspi1.begin(hspi)) {
    logLine("HSPI Module 1 Jammer Initialized!");
    configureRadio(radio_hspi1, ch[0]);
  } else {
    logLine("HSPI Module 1 initialization failed!");
  }
  if (radio_hspi2.begin(hspi)) {
    logLine("HSPI Module 2 Jammer Initialized!");
    configureRadio(radio_hspi2, ch[1]);
  } else {
    logLine("HSPI Module 2 initialization failed!");
  }

  // Initialize VSPI (Modules 3 & 4)
  SPIClass *vspi = new SPIClass(VSPI);
  vspi->begin(18, 19, 23, -1); // SCK=18, MISO=19, MOSI=23
  if (radio_vspi1.begin(vspi)) {
    logLine("VSPI Module 3 Jammer Initialized!");
    configureRadio(radio_vspi1, ch[2]);
  } else {
    logLine("VSPI Module 3 initialization failed!");
  }
  if (radio_vspi2.begin(vspi)) {
    logLine("VSPI Module 4 Jammer Initialized!");
    configureRadio(radio_vspi2, ch[3]);
  } else {
    logLine("VSPI Module 4 initialization failed!");
  }
}

void deactivateJammers() {
  radio_hspi1.stopConstCarrier();
  radio_hspi2.stopConstCarrier();
  radio_vspi1.stopConstCarrier();
  radio_vspi2.stopConstCarrier();
  logLine("All jammers stopped transmission");
}

void runCurrentMode() {
  byte newCh[4];
  switch (mode) {
    case 'r': // Random Bluetooth hopping (odd/even)
      newCh[0] = bluetooth_odd_channels[random(num_bluetooth_odd)];
      newCh[1] = bluetooth_even_channels[random(num_bluetooth_even)];
      newCh[2] = bluetooth_odd_channels[random(num_bluetooth_odd)];
      newCh[3] = bluetooth_even_channels[random(num_bluetooth_even)];
      break;
    case 'w': // WiFi channels
      for (int i = 0; i < 4; i++) newCh[i] = wifi_channels[random(num_wifi)];
      break;
    case 'b': // BLE advertising channels
      for (int i = 0; i < 4; i++) newCh[i] = ble_channels[random(num_ble)];
      break;
    case 'u': // USB wireless channels
      for (int i = 0; i < 4; i++) newCh[i] = usb_channels[random(num_usb)];
      break;
    case 'v': // Video streaming channels
      for (int i = 0; i < 4; i++) newCh[i] = video_channels[random(num_video)];
      break;
    case 'c': // RC toys/drones channels
      for (int i = 0; i < 4; i++) newCh[i] = rc_channels[random(num_rc)];
      break;
    case 'f': // Full spectrum
      for (int i = 0; i < 4; i++) newCh[i] = full_channels[random(num_full)];
      break;
    default:
      for (int i = 0; i < 4; i++) newCh[i] = ch[i]; // Maintain current channels
      break;
  }
  updateChannels(newCh);
  delayMicroseconds(random(60));
}

void updateChannels(byte newCh[4]) {
  if (newCh[0] != ch[0]) { radio_hspi1.setChannel(newCh[0]); ch[0] = newCh[0]; }
  if (newCh[1] != ch[1]) { radio_hspi2.setChannel(newCh[1]); ch[1] = newCh[1]; }
  if (newCh[2] != ch[2]) { radio_vspi1.setChannel(newCh[2]); ch[2] = newCh[2]; }
  if (newCh[3] != ch[3]) { radio_vspi2.setChannel(newCh[3]); ch[3] = newCh[3]; }
}

bool isValidMode(char m) {
  return (m == 'r' || m == 'w' || m == 'b' || m == 'u' || m == 'v' || m == 'c' || m == 'f');
}

void printModeHelp() {
  logLine("\nAvailable commands:");
  logLine("start  - Initialize and activate jammers");
  logLine("stop   - Deactivate jammers");
  logLine("help/? - Show this help");
  logLine("\nAvailable modes (send single character):");
  logLine("r - Random Bluetooth hopping (odd/even)");
  logLine("w - WiFi channels");
  logLine("b - BLE advertising channels");
  logLine("u - USB wireless channels");
  logLine("v - Video streaming channels");
  logLine("c - RC toys/drones channels");
  logLine("f - Full spectrum (all channels)");
}

String modeName(char m) {
  switch (m) {
    case 'r': return "Random Bluetooth hopping (odd/even)";
    case 'w': return "WiFi channels";
    case 'b': return "BLE advertising channels";
    case 'u': return "USB wireless channels";
    case 'v': return "Video streaming channels";
    case 'c': return "RC toys/drones channels";
    case 'f': return "Full spectrum (all channels)";
    default:  return "Unknown mode";
  }
}

void configureRadio(RF24& radio, byte channel) {
  radio.setAutoAck(false);
  radio.stopListening();
  radio.setRetries(0, 0);
  radio.setPALevel(RF24_PA_MAX, true);
  radio.setDataRate(RF24_2MBPS);
  radio.setCRCLength(RF24_CRC_DISABLED);
  radio.startConstCarrier(RF24_PA_MAX, channel);
}
