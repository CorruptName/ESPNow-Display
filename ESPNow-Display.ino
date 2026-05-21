#include <WiFi.h>
#include <esp_now.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#ifndef TOUCH_CS_PIN
#define TOUCH_CS_PIN 33
#endif

#ifndef TOUCH_IRQ_PIN
#define TOUCH_IRQ_PIN 36
#endif

#ifndef TFT_ROTATION
#define TFT_ROTATION 1
#endif

#ifndef TOUCH_RAW_X_MIN
#define TOUCH_RAW_X_MIN 200
#endif
#ifndef TOUCH_RAW_X_MAX
#define TOUCH_RAW_X_MAX 3800
#endif
#ifndef TOUCH_RAW_Y_MIN
#define TOUCH_RAW_Y_MIN 200
#endif
#ifndef TOUCH_RAW_Y_MAX
#define TOUCH_RAW_Y_MAX 3800
#endif

constexpr int TOUCH_PRESSURE_THRESHOLD = 200;
constexpr unsigned long TOUCH_DEBOUNCE_MS = 180;
constexpr unsigned long KEY_PRESS_FEEDBACK_MS = 80;
constexpr int MAX_PAYLOAD_DISPLAY_LEN = 100;
constexpr int MAC_STRING_LENGTH = 17;
constexpr int MAC_BUFFER_SIZE = MAC_STRING_LENGTH + 1;

struct Key {
  const char *label;
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

TFT_eSPI tft;
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);
Preferences preferences;

String targetMacInput = "";
bool targetMacValid = false;
uint8_t targetMac[6] = {0};
String statusLine = "Ready";
String lastFrom = "";
String lastPayload = "(none)";
unsigned long lastTouchMs = 0;

Key keys[] = {
    {"A", 8, 120, 54, 40},   {"B", 68, 120, 54, 40},  {"C", 128, 120, 54, 40},
    {"D", 188, 120, 54, 40}, {"E", 248, 120, 54, 40}, {"F", 8, 166, 54, 40},
    {"0", 68, 166, 54, 40},  {"1", 128, 166, 54, 40}, {"2", 188, 166, 54, 40},
    {"3", 248, 166, 54, 40}, {"4", 8, 212, 54, 40},   {"5", 68, 212, 54, 40},
    {"6", 128, 212, 54, 40}, {"7", 188, 212, 54, 40}, {"8", 248, 212, 54, 40},
    {"9", 8, 258, 54, 40},   {":", 68, 258, 54, 40},  {"BK", 128, 258, 54, 40},
    {"CLR", 188, 258, 54, 40}, {"SAVE", 248, 258, 62, 40},
};

String formatMac(const uint8_t *mac) {
  char buffer[MAC_BUFFER_SIZE];
  snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buffer);
}

bool parseMac(const String &text, uint8_t *macOut) {
  if (text.length() != MAC_STRING_LENGTH) {
    return false;
  }

  int values[6];
  if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) != 6) {
    return false;
  }

  for (int i = 0; i < 6; i++) {
    if (values[i] < 0 || values[i] > 255) {
      return false;
    }
    macOut[i] = static_cast<uint8_t>(values[i]);
  }

  return true;
}

void drawKey(const Key &k, uint16_t color, uint16_t textColor) {
  tft.fillRoundRect(k.x, k.y, k.w, k.h, 6, color);
  tft.drawRoundRect(k.x, k.y, k.w, k.h, 6, TFT_DARKGREY);
  tft.setTextColor(textColor, color);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(k.label, k.x + (k.w / 2), k.y + (k.h / 2));
}

void drawUi() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("ESP-NOW Display", 8, 8, 2);

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Listen MAC:", 8, 36, 2);

  tft.fillRoundRect(8, 56, 304, 26, 4, TFT_NAVY);
  tft.drawRoundRect(8, 56, 304, 26, 4, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString(targetMacInput, 12, 62, 2);

  tft.fillRoundRect(8, 86, 304, 26, 4, TFT_DARKGREEN);
  tft.drawRoundRect(8, 86, 304, 26, 4, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString(statusLine, 12, 92, 2);

  for (size_t i = 0; i < (sizeof(keys) / sizeof(keys[0])); i++) {
    drawKey(keys[i], TFT_DARKCYAN, TFT_WHITE);
  }

  tft.fillRoundRect(8, 304, 304, 64, 4, TFT_DARKGREY);
  tft.setTextColor(TFT_YELLOW, TFT_DARKGREY);
  tft.drawString("From: " + lastFrom, 12, 312, 2);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString("Msg: " + lastPayload, 12, 336, 2);
}

void refreshDynamicRows() {
  tft.fillRoundRect(8, 56, 304, 26, 4, TFT_NAVY);
  tft.drawRoundRect(8, 56, 304, 26, 4, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString(targetMacInput, 12, 62, 2);

  tft.fillRoundRect(8, 86, 304, 26, 4, TFT_DARKGREEN);
  tft.drawRoundRect(8, 86, 304, 26, 4, TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString(statusLine, 12, 92, 2);

  tft.fillRoundRect(8, 304, 304, 64, 4, TFT_DARKGREY);
  tft.setTextColor(TFT_YELLOW, TFT_DARKGREY);
  tft.drawString("From: " + lastFrom, 12, 312, 2);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString("Msg: " + lastPayload, 12, 336, 2);
}

void saveTargetMac() {
  uint8_t parsed[6] = {0};
  if (!parseMac(targetMacInput, parsed)) {
    targetMacValid = false;
    statusLine = "Invalid MAC format";
    refreshDynamicRows();
    return;
  }

  memcpy(targetMac, parsed, sizeof(targetMac));
  targetMacValid = true;
  preferences.putString("target_mac", targetMacInput);
  statusLine = "Saved and listening";
  refreshDynamicRows();
}

void appendKey(const char *label) {
  if (strcmp(label, "BK") == 0) {
    if (!targetMacInput.isEmpty()) {
      targetMacInput.remove(targetMacInput.length() - 1);
    }
  } else if (strcmp(label, "CLR") == 0) {
    targetMacInput = "";
  } else if (strcmp(label, "SAVE") == 0) {
    saveTargetMac();
    return;
  } else {
    if (targetMacInput.length() < MAC_STRING_LENGTH) {
      targetMacInput += label;
    }
  }

  statusLine = "Editing";
  refreshDynamicRows();
}

bool touchToScreenPoint(int16_t &sx, int16_t &sy) {
  if (!touch.touched()) {
    return false;
  }

  TS_Point p = touch.getPoint();
  if (p.z < TOUCH_PRESSURE_THRESHOLD) {
    return false;
  }

  int16_t mappedX = map(p.x, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX, 0, tft.width());
  int16_t mappedY = map(p.y, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX, 0, tft.height());

  mappedX = constrain(mappedX, 0, tft.width() - 1);
  mappedY = constrain(mappedY, 0, tft.height() - 1);

  if (TFT_ROTATION == 1 || TFT_ROTATION == 3) {
    sx = mappedY;
    sy = mappedX;
  } else {
    sx = mappedX;
    sy = mappedY;
  }

  return true;
}

void processTouch() {
  if (millis() - lastTouchMs < TOUCH_DEBOUNCE_MS) {
    return;
  }

  int16_t x = 0;
  int16_t y = 0;
  if (!touchToScreenPoint(x, y)) {
    return;
  }

  for (size_t i = 0; i < (sizeof(keys) / sizeof(keys[0])); i++) {
    const Key &k = keys[i];
    if (x >= k.x && x <= (k.x + k.w) && y >= k.y && y <= (k.y + k.h)) {
      drawKey(k, TFT_BLUE, TFT_WHITE);
      appendKey(k.label);
      delay(KEY_PRESS_FEEDBACK_MS);
      drawKey(k, TFT_DARKCYAN, TFT_WHITE);
      lastTouchMs = millis();
      return;
    }
  }
}

void onDataRecv(const esp_now_recv_info *recvInfo, const uint8_t *incomingData, int len) {
  if (recvInfo == nullptr || incomingData == nullptr || len <= 0) {
    return;
  }

  if (targetMacValid && memcmp(recvInfo->src_addr, targetMac, 6) != 0) {
    return;
  }

  lastFrom = formatMac(recvInfo->src_addr);

  String payload;
  for (int i = 0; i < len && i < MAX_PAYLOAD_DISPLAY_LEN; i++) {
    char c = static_cast<char>(incomingData[i]);
    payload += (isPrintable(c) ? c : '.');
  }

  lastPayload = payload;
  statusLine = "Message received";
  refreshDynamicRows();
}

void loadPersistedMac() {
  targetMacInput = preferences.getString("target_mac", "");
  uint8_t parsed[6];
  if (parseMac(targetMacInput, parsed)) {
    memcpy(targetMac, parsed, sizeof(targetMac));
    targetMacValid = true;
    statusLine = "Loaded saved MAC";
  } else {
    targetMacValid = false;
    targetMacInput = "";
    statusLine = "Enter MAC and SAVE";
  }
}

void setupEspNow() {
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    statusLine = "ESP-NOW init failed";
    refreshDynamicRows();
    return;
  }

  esp_now_register_recv_cb(onDataRecv);
  statusLine = targetMacValid ? "Listening for sender" : "No sender filter yet";
}

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.setTextFont(2);

  touchSPI.begin(25, 39, 32, TOUCH_CS_PIN);
  touch.begin(touchSPI);
  touch.setRotation(TFT_ROTATION);

  preferences.begin("espnowdisp", false);
  loadPersistedMac();

  drawUi();
  setupEspNow();
  refreshDynamicRows();
}

void loop() {
  processTouch();
}
