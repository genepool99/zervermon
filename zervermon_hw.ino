#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

#define SDA_PIN 5
#define SCL_PIN 6
#define SERIAL_BAUD 115200

#define PAGE_INTERVAL_MS 5000UL
#define FRAME_INTERVAL_MS 120UL
#define SCREENSAVER_AFTER_MS 60000UL
#define DISPLAY_OFF_AFTER_MS 1800000UL  // 30 minutes

U8G2_SH1106_128X64_NONAME_F_HW_I2C display(
  U8G2_R0,
  U8X8_PIN_NONE,
  SCL_PIN,
  SDA_PIN
);

String inputLine = "";
String lastJson = "";

unsigned long lastDataMs = 0;
unsigned long lastFrameMs = 0;
unsigned long lastPageMs = 0;

bool displayIsOn = true;

int currentPage = 0;
const int pageCount = 3;

// Screensaver position
int saverX = 0;
int saverY = 8;
int saverDx = 1;
int saverDy = 1;

String getValue(const String& json, const String& key) {
  String pattern = "\"" + key + "\":\"";
  int start = json.indexOf(pattern);
  if (start < 0) return "";

  start += pattern.length();
  int end = json.indexOf("\"", start);
  if (end < 0) return "";

  return json.substring(start, end);
}

String valueOrDash(const String& value) {
  return value.length() ? value : "-";
}

String getHostname(const String& json) {
  String hostname = getValue(json, "hostname");

  if (hostname.length()) {
    return hostname;
  }

  String title = getValue(json, "title");

  if (title.length()) {
    return title;
  }

  return "Server";
}

String getIpAddress(const String& json) {
  String ip = getValue(json, "ip");

  if (ip.length()) {
    return ip;
  }

  return "-";
}

void wakeDisplay() {
  if (!displayIsOn) {
    display.setPowerSave(0);
    displayIsOn = true;
  }

  display.setContrast(180);
}

void sleepDisplay() {
  if (displayIsOn) {
    display.clearBuffer();
    display.sendBuffer();
    display.setPowerSave(1);
    displayIsOn = false;
  }
}

void drawHeader(const String& hostname, const char* pageLabel) {
  display.setFont(u8g2_font_6x10_tf);

  // Keep the hostname from running into the page label/activity mark.
  String clippedHostname = hostname;
  if (clippedHostname.length() > 14) {
    clippedHostname = clippedHostname.substring(0, 13) + ".";
  }

  display.drawStr(0, 9, clippedHostname.c_str());
  display.drawHLine(0, 12, 128);

  display.setFont(u8g2_font_5x8_tf);
  display.drawStr(96, 9, pageLabel);

  // Animated heartbeat / activity mark
  int phase = (millis() / 250) % 4;
  const char* mark = phase == 0 ? "." : phase == 1 ? "o" : phase == 2 ? "O" : "o";
  display.drawStr(120, 9, mark);
}

void drawSystemPage(const String& json) {
  String hostname = getHostname(json);
  String ip = getIpAddress(json);
  String load = valueOrDash(getValue(json, "load"));
  String temp = valueOrDash(getValue(json, "temp"));
  String ram = valueOrDash(getValue(json, "ram"));

  drawHeader(hostname, "SYS");

  display.setFont(u8g2_font_6x10_tf);

  display.drawStr(0, 25, "IP");
  display.drawStr(44, 25, ip.c_str());

  display.drawStr(0, 38, "Load");
  display.drawStr(44, 38, load.c_str());

  display.drawStr(0, 51, "CPU");
  display.drawStr(44, 51, temp.c_str());

  display.drawStr(0, 64, "RAM");
  display.drawStr(44, 64, ram.c_str());
}

void drawZfsPage(const String& json) {
  String hostname = getHostname(json);
  String pool = valueOrDash(getValue(json, "pool"));
  String used = valueOrDash(getValue(json, "used"));
  String total = valueOrDash(getValue(json, "total"));

  drawHeader(hostname, "ZFS");

  display.setFont(u8g2_font_6x10_tf);

  display.drawStr(0, 28, "Pool");
  display.drawStr(44, 28, pool.c_str());

  display.drawStr(0, 42, "Used");
  display.drawStr(44, 42, used.c_str());

  display.drawStr(0, 56, "Total");
  display.drawStr(44, 56, total.c_str());
}

void drawTempPage(const String& json) {
  String hostname = getHostname(json);
  String temp = valueOrDash(getValue(json, "temp"));
  String disk = valueOrDash(getValue(json, "disk"));
  String fan = valueOrDash(getValue(json, "fan"));

  drawHeader(hostname, "TEMP");

  display.setFont(u8g2_font_6x10_tf);

  display.drawStr(0, 28, "CPU");
  display.drawStr(44, 28, temp.c_str());

  display.drawStr(0, 42, "Disk");
  display.drawStr(44, 42, disk.c_str());

  display.drawStr(0, 56, "Fan");
  display.drawStr(44, 56, fan.c_str());
}

void drawNoDataPage() {
  wakeDisplay();

  display.clearBuffer();

  drawHeader("Server Monitor", "USB");

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 30, "Waiting for serial");
  display.drawStr(0, 44, "Send JSON line");

  int x = (millis() / 80) % 128;
  display.drawPixel(x, 62);
  display.drawPixel((x + 8) % 128, 62);
  display.drawPixel((x + 16) % 128, 62);

  display.sendBuffer();
}

void drawStatusFrame() {
  wakeDisplay();

  display.clearBuffer();

  if (lastJson.length() == 0) {
    drawNoDataPage();
    return;
  }

  if (currentPage == 0) {
    drawSystemPage(lastJson);
  } else if (currentPage == 1) {
    drawZfsPage(lastJson);
  } else {
    drawTempPage(lastJson);
  }

  // Small animated footer sweep to keep bottom pixels changing.
  int sweepX = (millis() / 60) % 128;
  display.drawHLine(0, 63, 128);
  display.setDrawColor(0);
  display.drawBox(sweepX, 62, 12, 2);
  display.setDrawColor(1);

  display.sendBuffer();
}

void drawScreensaverFrame() {
  wakeDisplay();
  display.setContrast(80);

  display.clearBuffer();

  const int cardW = 82;
  const int cardH = 28;

  saverX += saverDx;
  saverY += saverDy;

  if (saverX <= 0 || saverX + cardW >= 128) {
    saverDx = -saverDx;
    saverX += saverDx;
  }

  if (saverY <= 0 || saverY + cardH >= 64) {
    saverDy = -saverDy;
    saverY += saverDy;
  }

  display.drawRFrame(saverX, saverY, cardW, cardH, 4);

  String hostname = getHostname(lastJson);
  String ip = getIpAddress(lastJson);

  if (hostname.length() > 12) {
    hostname = hostname.substring(0, 11) + ".";
  }

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(saverX + 7, saverY + 11, hostname.c_str());

  display.setFont(u8g2_font_5x8_tf);

  if (ip != "-") {
    display.drawStr(saverX + 7, saverY + 21, ip.c_str());
  } else {
    String pool = valueOrDash(getValue(lastJson, "pool"));
    String temp = valueOrDash(getValue(lastJson, "temp"));

    char line[32];
    snprintf(line, sizeof(line), "%s %s", pool.c_str(), temp.c_str());
    display.drawStr(saverX + 7, saverY + 21, line);
  }

  // Tiny orbiting dot around the card.
  int phase = (millis() / 100) % 32;
  int dotX = saverX;
  int dotY = saverY;

  if (phase < 8) {
    dotX = saverX + phase * (cardW / 8);
    dotY = saverY - 1;
  } else if (phase < 16) {
    dotX = saverX + cardW;
    dotY = saverY + (phase - 8) * (cardH / 8);
  } else if (phase < 24) {
    dotX = saverX + cardW - (phase - 16) * (cardW / 8);
    dotY = saverY + cardH;
  } else {
    dotX = saverX - 1;
    dotY = saverY + cardH - (phase - 24) * (cardH / 8);
  }

  if (dotX >= 0 && dotX < 128 && dotY >= 0 && dotY < 64) {
    display.drawDisc(dotX, dotY, 1);
  }

  display.sendBuffer();
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n') {
      inputLine.trim();

      if (inputLine.length() > 0) {
        lastJson = inputLine;
        lastDataMs = millis();
        currentPage = 0;
        lastPageMs = millis();

        wakeDisplay();
        drawStatusFrame();
      }

      inputLine = "";
    } else {
      inputLine += c;

      if (inputLine.length() > 300) {
        inputLine = "";
      }
    }
  }
}

void loop() {
  handleSerial();

  unsigned long now = millis();

  if (lastDataMs > 0 && now - lastDataMs >= DISPLAY_OFF_AFTER_MS) {
    sleepDisplay();
    return;
  }

  if (now - lastFrameMs < FRAME_INTERVAL_MS) {
    return;
  }

  lastFrameMs = now;

  bool hasRecentData = lastDataMs > 0 && now - lastDataMs < SCREENSAVER_AFTER_MS;

  if (lastJson.length() == 0) {
    drawNoDataPage();
    return;
  }

  if (!hasRecentData) {
    drawScreensaverFrame();
    return;
  }

  display.setContrast(180);

  if (now - lastPageMs >= PAGE_INTERVAL_MS) {
    lastPageMs = now;
    currentPage = (currentPage + 1) % pageCount;
  }

  drawStatusFrame();
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);

  Wire.begin(SDA_PIN, SCL_PIN);

  display.begin();
  display.setContrast(180);

  drawNoDataPage();
}