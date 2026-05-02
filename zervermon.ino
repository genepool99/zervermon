#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

#define SDA_PIN 5
#define SCL_PIN 6
#define SERIAL_BAUD 115200

#define PAGE_INTERVAL_MS 5000UL
#define FRAME_INTERVAL_MS 120UL
#define SCREENSAVER_AFTER_MS 60000UL
#define WALKER_INTERVAL_MS 60000UL
#define WALKER_DURATION_MS 7000UL
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
unsigned long lastWalkerStartMs = 0;

bool displayIsOn = true;
bool walkerActive = false;

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
  display.drawStr(38, 25, ip.c_str());

  display.drawStr(0, 38, "Load");
  display.drawStr(38, 38, load.c_str());

  display.drawStr(0, 51, "CPU/RAM");

  char cpuRam[32];
  snprintf(cpuRam, sizeof(cpuRam), "%s %s", temp.c_str(), ram.c_str());
  display.drawStr(50, 51, cpuRam);
}

void drawZfsPage(const String& json) {
  String hostname = getHostname(json);
  String zfs1 = valueOrDash(getValue(json, "zfs1"));
  String zfs2 = valueOrDash(getValue(json, "zfs2"));
  String zfs3 = valueOrDash(getValue(json, "zfs3"));

  if (zfs1 == "-") {
    String pool = valueOrDash(getValue(json, "pool"));
    String used = valueOrDash(getValue(json, "used"));
    String total = valueOrDash(getValue(json, "total"));

    zfs1 = pool;
    zfs2 = used + "/" + total;
    zfs3 = "-";
  }

  drawHeader(hostname, "ZFS");

  display.setFont(u8g2_font_5x8_tf);

  display.drawStr(0, 26, zfs1.c_str());

  if (zfs2 != "-") {
    display.drawStr(0, 40, zfs2.c_str());
  }

  if (zfs3 != "-") {
    display.drawStr(0, 54, zfs3.c_str());
  }
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

  display.drawStr(0, 54, "Fan");
  display.drawStr(44, 54, fan.c_str());
}

void maybeUpdateWalker(bool hasRecentData) {
  unsigned long now = millis();

  // Decorative footer-only animation; disable it when there is no active status data.
  if (!hasRecentData || lastJson.length() == 0) {
    walkerActive = false;
    return;
  }

  if (walkerActive) {
    if (now - lastWalkerStartMs >= WALKER_DURATION_MS) {
      walkerActive = false;
      lastWalkerStartMs = now;
    }
    return;
  }

  if (now - lastWalkerStartMs >= WALKER_INTERVAL_MS) {
    walkerActive = true;
    lastWalkerStartMs = now;
  }
}

void drawWalker() {
  if (!walkerActive) {
    return;
  }

  unsigned long elapsed = millis() - lastWalkerStartMs;
  if (elapsed >= WALKER_DURATION_MS) {
    return;
  }

  int x = map(elapsed, 0, WALKER_DURATION_MS, -12, 132);
  int y = 56;
  int step = (millis() / 180) % 2;

  // Decorative footer walker constrained to the reserved bottom band.
  display.drawBox(x + 2, y + 2, 7, 4);
  display.drawPixel(x + 1, y + 1);
  display.drawPixel(x + 8, y + 1);
  display.drawPixel(x + 4, y + 3);
  display.drawLine(x + 9, y + 3, x + 11, y + 1);

  if (step == 0) {
    display.drawPixel(x + 3, y + 6);
    display.drawPixel(x + 7, y + 6);
  } else {
    display.drawPixel(x + 2, y + 6);
    display.drawPixel(x + 8, y + 6);
  }
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

  if (walkerActive) {
    drawWalker();
  } else {
    int sweepX = (millis() / 60) % 128;
    display.drawHLine(0, 61, 128);
    display.setDrawColor(0);
    display.drawBox(sweepX, 60, 12, 3);
    display.setDrawColor(1);
  }

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
    maybeUpdateWalker(false);
    drawNoDataPage();
    return;
  }

  if (!hasRecentData) {
    maybeUpdateWalker(false);
    drawScreensaverFrame();
    return;
  }

  display.setContrast(180);

  maybeUpdateWalker(true);

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