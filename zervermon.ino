#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

#define SDA_PIN 5
#define SCL_PIN 6
#define SERIAL_BAUD 115200

#define STARTUP_ANIMATION_MS 10000UL
#define ERROR_AFTER_MS 30000UL
#define PAGE_INTERVAL_MS 8000UL
#define FRAME_INTERVAL_MS 120UL
#define SCREENSAVER_AFTER_MS 60000UL
#define WALKER_INTERVAL_MS 60000UL
#define WALKER_DURATION_MS 7000UL
#define FULL_SWEEP_INTERVAL_MS 180000UL
#define FULL_SWEEP_DURATION_MS 8000UL
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
unsigned long lastFullSweepStartMs = 0;
unsigned long bootStartedMs = 0;

bool displayIsOn = true;
bool walkerActive = false;
bool fullSweepActive = false;
bool startupComplete = false;

int currentPage = 0;
const int BASE_PAGE_COUNT = 4;
const int MAX_POOL_PAGES = 4;

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

bool poolPagePresent(const String& json, int poolIndex) {
  String key = "pool" + String(poolIndex) + "_name";
  String name = getValue(json, key);
  return name.length() > 0 && name != "-";
}

int getDynamicPageCount() {
  int count = BASE_PAGE_COUNT;

  if (lastJson.length() == 0) {
    return BASE_PAGE_COUNT;
  }

  for (int i = 1; i <= MAX_POOL_PAGES; i++) {
    if (poolPagePresent(lastJson, i)) {
      count++;
    }
  }

  return count;
}

int dynamicPageToPoolIndex(int page) {
  int poolPageOffset = page - BASE_PAGE_COUNT;
  if (poolPageOffset < 0) {
    return 0;
  }

  int seen = 0;
  for (int i = 1; i <= MAX_POOL_PAGES; i++) {
    if (poolPagePresent(lastJson, i)) {
      if (seen == poolPageOffset) {
        return i;
      }
      seen++;
    }
  }

  return 0;
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
  String currentTime = valueOrDash(getValue(json, "time"));
  String date = valueOrDash(getValue(json, "date"));
  String uptime = valueOrDash(getValue(json, "uptime"));
  String users = valueOrDash(getValue(json, "users"));
  String load = valueOrDash(getValue(json, "load"));

  drawHeader(hostname, "SYS");

  display.setFont(u8g2_font_6x10_tf);

  display.drawStr(0, 25, "Time");
  display.drawStr(38, 25, currentTime.c_str());

  display.setFont(u8g2_font_5x8_tf);
  display.drawStr(78, 25, date.c_str());

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 38, "Up");
  display.drawStr(38, 38, uptime.c_str());

  display.drawStr(0, 51, "User");
  display.drawStr(38, 51, users.c_str());

  display.setFont(u8g2_font_5x8_tf);
  display.drawStr(62, 51, "Ld");
  display.drawStr(76, 51, load.c_str());
}

void drawNetPage(const String& json) {
  String hostname = getHostname(json);
  String iface = valueOrDash(getValue(json, "net_iface"));
  String ip = valueOrDash(getValue(json, "ip"));
  String speed = valueOrDash(getValue(json, "net_speed"));
  String rx = valueOrDash(getValue(json, "net_rx"));
  String tx = valueOrDash(getValue(json, "net_tx"));

  drawHeader(hostname, "NET");

  display.setFont(u8g2_font_6x10_tf);

  display.drawStr(0, 25, "Iface");
  display.drawStr(44, 25, iface.c_str());

  display.drawStr(0, 38, "IP");
  display.drawStr(44, 38, ip.c_str());

  display.drawStr(0, 51, "R/T");

  display.setFont(u8g2_font_5x8_tf);
  display.drawStr(28, 51, rx.c_str());
  display.drawStr(76, 51, tx.c_str());
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
  String ambient = valueOrDash(getValue(json, "ambient"));
  String disk = valueOrDash(getValue(json, "disk"));
  String fan = valueOrDash(getValue(json, "fan"));

  drawHeader(hostname, "TEMP");

  display.setFont(u8g2_font_6x10_tf);

  display.drawStr(0, 27, "CPU");
  display.drawStr(28, 27, temp.c_str());

  display.drawStr(68, 27, "Amb");
  display.drawStr(96, 27, ambient.c_str());

  display.drawStr(0, 41, "Disk");
  display.drawStr(38, 41, disk.c_str());

  display.drawStr(0, 55, "Fan");
  display.setFont(u8g2_font_5x8_tf);
  display.drawStr(38, 55, fan.c_str());
  display.setFont(u8g2_font_6x10_tf);
}

void drawFanPage(const String& json) {
  String hostname = getHostname(json);
  String fanCpu = valueOrDash(getValue(json, "fan_cpu"));
  String fanRear = valueOrDash(getValue(json, "fan_rear"));
  String fanFront = valueOrDash(getValue(json, "fan_front"));
  String fanMem = valueOrDash(getValue(json, "fan_mem"));

  drawHeader(hostname, "FAN");

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 25, "CPU");
  display.drawStr(42, 25, fanCpu.c_str());

  display.drawStr(0, 38, "Rear");
  display.drawStr(42, 38, fanRear.c_str());

  display.drawStr(0, 51, "Front");
  display.drawStr(42, 51, fanFront.c_str());

  display.setFont(u8g2_font_5x8_tf);
  display.drawStr(82, 51, "Mem");
  display.drawStr(104, 51, fanMem.c_str());
}

void drawPoolPage(const String& json, int poolIndex) {
  if (poolIndex <= 0) {
    drawZfsPage(json);
    return;
  }

  String hostname = getHostname(json);
  String prefix = "pool" + String(poolIndex) + "_";

  String name = valueOrDash(getValue(json, prefix + "name"));
  String health = valueOrDash(getValue(json, prefix + "health"));
  String used = valueOrDash(getValue(json, prefix + "used"));
  String total = valueOrDash(getValue(json, prefix + "total"));
  String capacity = valueOrDash(getValue(json, prefix + "capacity"));

  char label[4];
  snprintf(label, sizeof(label), "P%d", poolIndex);

  drawHeader(hostname, label);

  display.setFont(u8g2_font_6x10_tf);

  if (name.length() > 14) {
    name = name.substring(0, 13) + ".";
  }

  display.drawStr(0, 25, name.c_str());

  display.drawStr(0, 38, health.c_str());
  display.drawStr(70, 38, capacity.c_str());

  display.drawStr(0, 51, "Used");
  display.drawStr(38, 51, used.c_str());

  display.setFont(u8g2_font_5x8_tf);
  display.drawStr(78, 51, "Tot");
  display.drawStr(96, 51, total.c_str());
}

void drawTinyMascot(int x, int y, int step) {
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

void maybeUpdateFullSweep(bool hasRecentData) {
  unsigned long now = millis();

  if (!hasRecentData || lastJson.length() == 0) {
    fullSweepActive = false;
    return;
  }

  if (fullSweepActive) {
    if (now - lastFullSweepStartMs >= FULL_SWEEP_DURATION_MS) {
      fullSweepActive = false;
      lastFullSweepStartMs = now;
    }
    return;
  }

  if (now - lastFullSweepStartMs >= FULL_SWEEP_INTERVAL_MS) {
    fullSweepActive = true;
    lastFullSweepStartMs = now;
  }
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
  drawTinyMascot(x, y, step);
}

void drawStartupFrame() {
  wakeDisplay();
  display.setContrast(180);
  display.clearBuffer();

  unsigned long elapsed = millis() - bootStartedMs;

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(16, 12, "ZerverMon");
  display.drawStr(10, 28, "Starting display");

  int barW = map(min(elapsed, STARTUP_ANIMATION_MS), 0, STARTUP_ANIMATION_MS, 0, 100);
  display.drawFrame(14, 38, 100, 8);
  display.drawBox(14, 38, barW, 8);

  int x = map(elapsed % STARTUP_ANIMATION_MS, 0, STARTUP_ANIMATION_MS, -12, 132);
  int yValues[] = {4, 16, 28, 40, 52};
  int band = (elapsed / 2000) % 5;
  int y = yValues[band];
  int step = (millis() / 180) % 2;

  drawTinyMascot(x, y, step);

  display.sendBuffer();
}

void drawFullSweepFrame() {
  wakeDisplay();
  display.setContrast(180);
  display.clearBuffer();

  unsigned long elapsed = millis() - lastFullSweepStartMs;
  if (elapsed >= FULL_SWEEP_DURATION_MS) {
    fullSweepActive = false;
    return;
  }

  int x = map(elapsed, 0, FULL_SWEEP_DURATION_MS, -12, 132);
  int band = (elapsed / 1000) % 5;
  int yValues[] = {4, 16, 28, 40, 52};
  int y = yValues[band];
  int step = (millis() / 160) % 2;

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(18, 12, "ZerverMon");
  display.setFont(u8g2_font_5x8_tf);
  display.drawStr(22, 24, "pixel sweep");

  int sparkle = (millis() / 90) % 128;
  display.drawPixel(sparkle, 2);
  display.drawPixel((sparkle + 31) % 128, 18);
  display.drawPixel((sparkle + 67) % 128, 36);
  display.drawPixel((sparkle + 101) % 128, 62);

  drawTinyMascot(x, y, step);

  int wipeX = (millis() / 40) % 128;
  display.drawVLine(wipeX, 0, 64);

  display.sendBuffer();
}

void drawErrorFrame() {
  wakeDisplay();
  display.setContrast(255);
  display.clearBuffer();

  display.setFont(u8g2_font_10x20_tf);
  display.drawStr(8, 24, "ERROR");

  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 42, "No serial data");
  display.drawStr(0, 56, "Check TrueNAS sender");

  if ((millis() / 500) % 2 == 0) {
    display.drawLine(112, 12, 124, 12);
    display.drawLine(112, 12, 118, 2);
    display.drawLine(124, 12, 118, 2);
    display.drawPixel(118, 8);
    display.drawPixel(118, 10);
  }

  display.sendBuffer();
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

  int dynamicCount = getDynamicPageCount();
  if (dynamicCount <= 0) {
    dynamicCount = BASE_PAGE_COUNT;
  }
  if (currentPage >= dynamicCount) {
    currentPage = 0;
  }

  if (currentPage == 0) {
    drawSystemPage(lastJson);
  } else if (currentPage == 1) {
    drawNetPage(lastJson);
  } else if (currentPage == 2) {
    drawTempPage(lastJson);
  } else if (currentPage == 3) {
    drawFanPage(lastJson);
  } else {
    int poolIndex = dynamicPageToPoolIndex(currentPage);
    drawPoolPage(lastJson, poolIndex);
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
        bool hadNoData = lastJson.length() == 0;
        bool wasIdle = lastDataMs > 0 && millis() - lastDataMs >= ERROR_AFTER_MS;

        lastJson = inputLine;
        lastDataMs = millis();

        // Only jump back to the SYS page when data first appears
        // or when recovering from an error/no-data state.
        // Do not reset the page on every normal update.
        if (hadNoData || wasIdle) {
          currentPage = 0;
          lastPageMs = millis();
        }

        wakeDisplay();

        // Do not interrupt the startup animation.
        if (startupComplete) {
          drawStatusFrame();
        }
      }

      inputLine = "";
    } else {
      inputLine += c;

      if (inputLine.length() > 1600) {
        inputLine = "";
      }
    }
  }
}

void loop() {
  handleSerial();

  unsigned long now = millis();

  if (now - lastFrameMs < FRAME_INTERVAL_MS) {
    return;
  }

  lastFrameMs = now;

  if (!startupComplete) {
    if (now - bootStartedMs < STARTUP_ANIMATION_MS) {
      drawStartupFrame();
      return;
    }

    startupComplete = true;
    lastPageMs = now;
  }

  if (lastJson.length() == 0) {
    maybeUpdateWalker(false);
    drawErrorFrame();
    return;
  }

  if (lastDataMs > 0 && now - lastDataMs >= DISPLAY_OFF_AFTER_MS) {
    maybeUpdateWalker(false);
    sleepDisplay();
    return;
  }

  if (lastDataMs > 0 && now - lastDataMs >= ERROR_AFTER_MS) {
    maybeUpdateFullSweep(false);
    maybeUpdateWalker(false);
    drawErrorFrame();
    return;
  }

  bool hasRecentData = lastDataMs > 0 && now - lastDataMs < SCREENSAVER_AFTER_MS;

  display.setContrast(180);

  maybeUpdateFullSweep(hasRecentData);
  if (fullSweepActive) {
    maybeUpdateWalker(false);
    drawFullSweepFrame();
    return;
  }

  maybeUpdateWalker(hasRecentData);

  if (now - lastPageMs >= PAGE_INTERVAL_MS) {
    lastPageMs = now;
    int dynamicCount = getDynamicPageCount();
    if (dynamicCount <= 0) {
      dynamicCount = BASE_PAGE_COUNT;
    }
    currentPage = (currentPage + 1) % dynamicCount;
  }

  drawStatusFrame();
}

void setup() {
  Serial.setRxBufferSize(2048);
  Serial.begin(SERIAL_BAUD);
  delay(500);

  Wire.begin(SDA_PIN, SCL_PIN);

  display.begin();
  display.setContrast(180);

  bootStartedMs = millis();
  startupComplete = false;

  drawStartupFrame();
}