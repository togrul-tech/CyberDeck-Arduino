#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <NimBLEDevice.h>

#define USE_ST7735 1
#if USE_ST7735
#include <Adafruit_ST7735.h>
#else
#include <Adafruit_ILI9341.h>
#endif

#define TFT_CS 5
#define TFT_DC 2
#define TFT_RST 4
#define BLE_PASSKEY 123456
#define TZ_OFFSET_SEC (4 * 3600)

#if USE_ST7735
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
#else
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
#endif

#define C_BLACK 0x0000
#define C_GREEN 0x07E0
#define C_DKGREEN 0x0320
#define C_CYAN 0x07FF
#define C_RED 0xF800
#define C_YELLOW 0xFFE0
#define C_MAGENTA 0xF81F
#define C_WHITE 0xFFFF
#define C_GREY 0x7BEF

#define KEY_ESC 0x1B
#define KEY_BKSP 0x08
#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83

int W, H;

enum Btn { B_NONE, B_ENTER, B_ESC };
enum Mode { M_MENU, M_WIFI, M_CHAN, M_BLE, M_KBD, M_TERM, M_SYS, M_SNAKE, M_HACK, M_MATRIX, M_VIEW };
enum Prompt { P_NONE, P_PASS, P_GUESS };

struct BleDev { char name[20]; char addr[18]; int rssi; };

Mode mode = M_MENU;
bool needRedraw = true;
Preferences prefs;

NimBLEClient *kbClient = nullptr;
char kbName[24] = "";
Mode kbBackMode = M_MENU;

int netCount = 0;

#define BLE_MAX 30
BleDev bleDevs[BLE_MAX];
int bleCount = 0;

#define TERM_MAX 40
#define HIST_MAX 10
String termLines[TERM_MAX];
int termCount = 0;
String termInput;
Prompt termPrompt = P_NONE;
String pendingSsid;
int guessNum = 0, guessTries = 0;
String hist[HIST_MAX];
int histCount = 0, histPos = -1;
String scanSsid[9];
int scanSsidCount = 0;

#define VIEW_MAX_CHARS 6000
#define VIEW_MAX_LINES 700
String viewText, viewTitle;
uint16_t viewLine[VIEW_MAX_LINES];
int viewLines = 0, viewTop = 0;

volatile uint8_t kbBuf[64];
volatile uint8_t kbHead = 0, kbTail = 0;

void kbPush(uint8_t c) {
  uint8_t n = (kbHead + 1) & 63;
  if (n != kbTail) { kbBuf[kbHead] = c; kbHead = n; }
}

int kbRead() {
  if (kbTail == kbHead) return -1;
  uint8_t c = kbBuf[kbTail];
  kbTail = (kbTail + 1) & 63;
  return c;
}

bool kbConnected() { return kbClient && kbClient->isConnected(); }
bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

uint8_t hidToChar(uint8_t k, bool shift) {
  if (k >= 0x04 && k <= 0x1D) return (shift ? 'A' : 'a') + (k - 0x04);
  if (k >= 0x1E && k <= 0x27) return shift ? "!@#$%^&*()"[k - 0x1E] : "1234567890"[k - 0x1E];
  switch (k) {
    case 0x28: return '\n';
    case 0x29: return KEY_ESC;
    case 0x2A: return KEY_BKSP;
    case 0x2B: return ' ';
    case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    case 0x4F: return KEY_RIGHT;
    case 0x50: return KEY_LEFT;
    case 0x51: return KEY_DOWN;
    case 0x52: return KEY_UP;
  }
  return 0;
}

void onKbReport(NimBLERemoteCharacteristic *c, uint8_t *data, size_t len, bool) {
  static uint8_t prev[6] = {0};
  int modAt, keysAt;
  if (len == 8) { modAt = 0; keysAt = 2; }
  else if (len == 7) { modAt = 0; keysAt = 1; }
  else if (len == 9) { modAt = 1; keysAt = 3; }
  else {
    Serial.printf("HID %s [%u] ignored\n", c->getUUID().toString().c_str(), (unsigned)len);
    return;
  }
  bool shift = data[modAt] & 0x22;
  const uint8_t *keys = data + keysAt;
  for (int i = 0; i < 6; i++) {
    uint8_t k = keys[i];
    if (k < 0x04) continue;
    bool held = false;
    for (int j = 0; j < 6; j++) if (prev[j] == k) held = true;
    if (!held) {
      uint8_t ch = hidToChar(k, shift);
      if (ch) kbPush(ch);
    }
  }
  memcpy(prev, keys, 6);
}

int navSel(int n, int key, int cur) {
  if (n <= 0) return 0;
  if (key == KEY_UP && cur > 0) cur--;
  if (key == KEY_DOWN && cur < n - 1) cur++;
  return constrain(cur, 0, n - 1);
}

void drawHeader(const char *title) {
  tft.fillRect(0, 0, W, 12, C_GREEN);
  tft.setTextColor(C_BLACK);
  tft.setTextSize(1);
  tft.setCursor(3, 2);
  tft.print(title);
  int x = W - 9;
  if (kbConnected()) { tft.setCursor(x, 2); tft.print("K"); x -= 8; }
  if (wifiUp()) { tft.setCursor(x, 2); tft.print("W"); }
}

void drawFooter(const char *hint) {
  tft.fillRect(0, H - 10, W, 10, C_DKGREEN);
  tft.setTextColor(C_GREEN);
  tft.setCursor(2, H - 9);
  tft.print(hint);
}

void typeLine(const char *s, uint16_t color, int d = 12) {
  tft.setTextColor(color);
  while (*s) { tft.print(*s++); delay(d); }
  tft.println();
}

void drawRssiBar(int x, int y, int rssi) {
  int bars = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;
    tft.fillRect(x + i * 3, y + 8 - h, 2, h, i < bars ? C_GREEN : C_GREY);
  }
}

void drawGameOver(int score, int best) {
  int bw = min(W - 16, 140), bh = 56;
  int bx = (W - bw) / 2, by = (H - bh) / 2;
  tft.fillRect(bx, by, bw, bh, C_BLACK);
  tft.drawRect(bx, by, bw, bh, C_RED);
  tft.setTextColor(C_RED);
  tft.setTextSize(2);
  tft.setCursor(bx + (bw - 108) / 2, by + 6);
  tft.print("GAME OVER");
  tft.setTextSize(1);
  tft.setTextColor(C_YELLOW);
  tft.setCursor(bx + 8, by + 26);
  tft.printf("Score: %d Best: %d", score, best);
  tft.setTextColor(C_GREEN);
  tft.setCursor(bx + 8, by + 40);
  tft.print("Ent=retry Esc=menu");
}

char translit(uint32_t c) {
  switch (c) {
    case 0x259: return 'e'; case 0x18F: return 'E';
    case 0x15F: return 's'; case 0x15E: return 'S';
    case 0x0E7: return 'c'; case 0x0C7: return 'C';
    case 0x11F: return 'g'; case 0x11E: return 'G';
    case 0x131: return 'i'; case 0x130: return 'I';
    case 0x0F6: return 'o'; case 0x0D6: return 'O';
    case 0x0FC: return 'u'; case 0x0DC: return 'U';
    case 0x0E9: case 0x0E8: case 0x0EA: return 'e';
    case 0x0E1: case 0x0E0: case 0x0E2: case 0x0E4: return 'a';
    case 0x0A0: return ' ';
    case 0x0B0: return 'o';
    case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2018: case 0x2019: return '\'';
    case 0x201C: case 0x201D: case 0x0AB: case 0x0BB: return '"';
    case 0x2022: case 0x0B7: return '*';
    case 0x2026: return '.';
    case 0x200B: case 0x200C: case 0x200D: case 0xFE0F: return 0;
  }
  if (c >= 0x2190 && c <= 0x21FF) return '>';
  if (c >= 0x1F000) return 0;
  return '?';
}

struct TextSink {
  String &out;
  size_t maxLen;
  bool html;
  bool inTag = false, tagDone = false, skip = false, inEnt = false, lastSpace = true;
  char tag[12];
  int tagLen = 0;
  char ent[10];
  int entLen = 0;
  uint32_t cp = 0;
  int utfLeft = 0;

  TextSink(String &o, size_t m, bool h) : out(o), maxLen(m), html(h) {}

  void put(char c) {
    if (!c || out.length() >= maxLen) return;
    if (c == ' ') {
      if (lastSpace) return;
      lastSpace = true;
    } else if (c == '\n') {
      if (out.length() && out[out.length() - 1] == ' ') out.remove(out.length() - 1);
      if (out.length() == 0 || out.endsWith("\n\n")) return;
      lastSpace = true;
    } else {
      lastSpace = false;
    }
    out += c;
  }

  void putCp(uint32_t c) {
    if (c == '\r') return;
    if (c == '\t') c = ' ';
    if (c == '\n' && html) c = ' ';
    if (c < 0x80) put((char)c);
    else put(translit(c));
  }

  void feed(uint8_t b) {
    if (utfLeft) {
      if ((b & 0xC0) == 0x80) {
        cp = (cp << 6) | (b & 0x3F);
        if (--utfLeft == 0) handle(cp);
        return;
      }
      utfLeft = 0;
    }
    if (b >= 0xF0) { cp = b & 0x07; utfLeft = 3; return; }
    if (b >= 0xE0) { cp = b & 0x0F; utfLeft = 2; return; }
    if (b >= 0xC0) { cp = b & 0x1F; utfLeft = 1; return; }
    if (b >= 0x80) return;
    handle(b);
  }

  void handle(uint32_t c) {
    if (!html) { putCp(c); return; }
    if (inTag) {
      if (c == '>') { inTag = false; tag[tagLen] = 0; endTag(); return; }
      if (!tagDone) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || (c == '/' && tagLen > 0)) tagDone = true;
        else if (tagLen < 11 && c < 0x80) tag[tagLen++] = tolower((char)c);
      }
      return;
    }
    if (c == '<') { inTag = true; tagDone = false; tagLen = 0; return; }
    if (skip) return;
    if (inEnt) {
      if (c == ';') { inEnt = false; endEnt(); return; }
      if (entLen < 9 && c < 0x80 && (isalnum((int)c) || c == '#')) { ent[entLen++] = (char)c; return; }
      inEnt = false;
      put('&');
      for (int i = 0; i < entLen; i++) put(ent[i]);
    }
    if (c == '&') { inEnt = true; entLen = 0; return; }
    putCp(c);
  }

  void endTag() {
    if (!strcmp(tag, "script") || !strcmp(tag, "style") || !strcmp(tag, "svg")) { skip = true; return; }
    if (!strcmp(tag, "/script") || !strcmp(tag, "/style") || !strcmp(tag, "/svg")) { skip = false; return; }
    if (!strcmp(tag, "li")) { put('\n'); put('*'); put(' '); return; }
    if (!strcmp(tag, "td") || !strcmp(tag, "th")) { put(' '); return; }
    static const char *blocks[] = { "br", "p", "/p", "div", "/div", "tr", "h1", "h2", "h3", "h4",
                                    "/h1", "/h2", "/h3", "/h4", "title", "/title", "table", "/table",
                                    "ul", "/ul", "ol", "/ol", "hr", "/li" };
    for (const char *b : blocks)
      if (!strcmp(tag, b)) { put('\n'); return; }
  }

  void endEnt() {
    ent[entLen] = 0;
    if (ent[0] == '#') {
      uint32_t v = (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent + 2, nullptr, 16) : strtoul(ent + 1, nullptr, 10);
      putCp(v);
      return;
    }
    if (!strcmp(ent, "amp")) put('&');
    else if (!strcmp(ent, "lt")) put('<');
    else if (!strcmp(ent, "gt")) put('>');
    else if (!strcmp(ent, "quot")) put('"');
    else if (!strcmp(ent, "apos")) put('\'');
    else if (!strcmp(ent, "nbsp")) put(' ');
    else if (!strcmp(ent, "mdash") || !strcmp(ent, "ndash")) put('-');
    else if (!strcmp(ent, "laquo") || !strcmp(ent, "raquo")) put('"');
    else put(' ');
  }
};

String urlEncode(const String &s) {
  String r;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
    else if (c == ' ') r += '+';
    else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
  }
  return r;
}

bool netFetch(const String &urlIn, String &out, size_t maxChars,
              const char *accept = "text/html,text/plain;q=0.9,*/*;q=0.5") {
  out = "";
  if (!wifiUp()) { out = "WiFi not connected. Use 'wifi' and 'connect'."; return false; }
  String url = urlIn;
  if (!url.startsWith("http://") && !url.startsWith("https://")) url = String("https://") + url;

  WiFiClientSecure secure;
  WiFiClient plain;
  HTTPClient http;
  bool ok;
  if (url.startsWith("https://")) {
    secure.setInsecure();
    ok = http.begin(secure, url);
  } else {
    ok = http.begin(plain, url);
  }
  if (!ok) { out = "Invalid URL"; return false; }

  http.useHTTP10(true);
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setUserAgent("Lynx/2.9.0 libwww-FM/2.14 SSL-MM/1.4.1");
  http.addHeader("Accept", accept);
  const char *hdrs[] = { "Content-Type" };
  http.collectHeaders(hdrs, 1);

  int code = http.GET();
  if (code <= 0) {
    out = String("Error: ") + http.errorToString(code);
    http.end();
    return false;
  }

  bool html = http.header("Content-Type").indexOf("html") >= 0;
  TextSink sink(out, maxChars, html);
  WiFiClient *s = http.getStreamPtr();
  uint8_t buf[256];
  unsigned long t0 = millis(), lastData = millis();
  while (out.length() < maxChars && millis() - t0 < 20000) {
    int avail = s->available();
    if (avail > 0) {
      int n = s->read(buf, min(avail, (int)sizeof(buf)));
      for (int i = 0; i < n; i++) sink.feed(buf[i]);
      lastData = millis();
    } else if (!s->connected() || millis() - lastData > 5000) {
      break;
    } else {
      delay(2);
    }
  }
  http.end();
  out.trim();
  if (code != 200) out = String("HTTP ") + code + "\n" + out;
  return true;
}

const char *menuItems[] = { "WIFI SCANNER", "CHANNEL MAP", "BLE SCANNER", "KEYBOARD", "TERMINAL",
                            "SYSTEM INFO", "CYBER WORM", "TYPE HACK", "MATRIX" };
const int MENU_N = 9;

void enterMode(Mode m) { mode = m; needRedraw = true; tft.fillScreen(C_BLACK); }

void bootScreen() {
  tft.fillScreen(C_BLACK);
  tft.setTextSize(1);
  tft.setCursor(0, 4);
  typeLine("> CYBERDECK OS v3.0", C_GREEN);
  typeLine("> booting kernel...", C_GREEN);
  char buf[40];
  snprintf(buf, sizeof(buf), "> CPU %s @%dMHz", ESP.getChipModel(), ESP.getCpuFreqMHz());
  typeLine(buf, C_CYAN);
  snprintf(buf, sizeof(buf), "> RAM free %u B", ESP.getFreeHeap());
  typeLine(buf, C_CYAN);
  typeLine("> WiFi radio .... OK", C_GREEN);
  typeLine("> Bluetooth ..... OK", C_GREEN);
  typeLine("> display ....... OK", C_GREEN);
  typeLine("> games ......... OK", C_GREEN);
  tft.println();
  typeLine("  ACCESS GRANTED", C_MAGENTA, 40);
  delay(800);
}

void runMenu(Btn b, int key) {
  static int sel = 0, lastSel = -1;
  int rowH = min(12, (H - 28) / MENU_N);
  if (needRedraw) {
    drawHeader("CYBERDECK :: MAIN");
    drawFooter("arrows=move Enter=select");
    lastSel = -1;
    needRedraw = false;
  }
  sel = navSel(MENU_N, key, sel);
  if (sel != lastSel) {
    for (int i = 0; i < MENU_N; i++) {
      int y = 15 + i * rowH;
      bool on = (i == sel);
      tft.fillRect(0, y, W, rowH, on ? C_DKGREEN : C_BLACK);
      tft.setTextColor(on ? C_YELLOW : C_GREEN);
      tft.setCursor(6, y + (rowH - 8) / 2 + 1);
      tft.print(on ? "> " : "  ");
      tft.print(menuItems[i]);
    }
    lastSel = sel;
  }
  if (b == B_ENTER) {
    kbBackMode = M_MENU;
    enterMode((Mode)(M_WIFI + sel));
  }
}

void doScan() {
  tft.fillRect(0, 12, W, H - 22, C_BLACK);
  tft.setTextColor(C_YELLOW);
  tft.setCursor(6, 30);
  tft.print("SCANNING...");
  netCount = WiFi.scanNetworks();
  tft.fillRect(0, 12, W, H - 22, C_BLACK);
}

void runWifi(Btn b, int key) {
  static int sel = 0, lastSel = -1, top = 0;
  const int rowH = 10;
  int rows = (H - 34) / rowH;

  if (needRedraw) {
    drawHeader("WIFI SCANNER");
    drawFooter("Ent=join r=scan");
    doScan();
    sel = 0; lastSel = -1; top = 0;
    needRedraw = false;
  }
  if (key == 'r') { doScan(); lastSel = -1; }

  if (netCount <= 0) {
    if (lastSel != -2) {
      tft.setTextColor(C_RED);
      tft.setCursor(6, 30);
      tft.print("No networks found");
      lastSel = -2;
    }
    return;
  }

  if (b == B_ENTER) {
    String ssid = WiFi.SSID(sel);
    bool open = WiFi.encryptionType(sel) == WIFI_AUTH_OPEN;
    termOpen();
    termPrint(String("> connect ") + ssid);
    if (open) {
      wifiConnect(ssid, "");
    } else {
      pendingSsid = ssid;
      termPrompt = P_PASS;
      termPrint(String("Password (") + ssid + "):");
    }
    termRedraw();
    return;
  }

  sel = navSel(netCount, key, sel);
  if (sel == lastSel) return;
  if (sel < top) top = sel;
  if (sel >= top + rows) top = sel - rows + 1;

  tft.fillRect(0, 13, W, rows * rowH + 1, C_BLACK);
  for (int r = 0; r < rows && top + r < netCount; r++) {
    int i = top + r, y = 14 + r * rowH;
    bool on = (i == sel);
    if (on) tft.fillRect(0, y - 1, W, rowH, C_DKGREEN);
    tft.setTextColor(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? C_RED : (on ? C_YELLOW : C_GREEN));
    tft.setCursor(2, y);
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) ssid = "<hidden>";
    int maxChars = (W - 22) / 6;
    if ((int)ssid.length() > maxChars) ssid = ssid.substring(0, maxChars);
    tft.print(ssid);
    drawRssiBar(W - 14, y - 1, WiFi.RSSI(i));
  }
  int dy = H - 21;
  tft.fillRect(0, dy - 1, W, 11, C_BLACK);
  tft.setTextColor(C_CYAN);
  tft.setCursor(2, dy);
  tft.printf("%d/%d CH%d %ddBm %s", sel + 1, netCount, WiFi.channel(sel), WiFi.RSSI(sel),
             WiFi.encryptionType(sel) == WIFI_AUTH_OPEN ? "OPEN" : "LOCK");
  lastSel = sel;
}

void runChannels(Btn b) {
  if (needRedraw || b == B_ENTER) {
    drawHeader("CHANNEL MAP 2.4GHz");
    drawFooter("Enter=scan Esc=back");
    doScan();
    needRedraw = false;

    int cnt[14] = {0}, mx = 1;
    for (int i = 0; i < netCount; i++) {
      int ch = WiFi.channel(i);
      if (ch >= 1 && ch <= 13 && ++cnt[ch] > mx) mx = cnt[ch];
    }
    int base = H - 22, top = 26, bw = (W - 8) / 13;
    for (int ch = 1; ch <= 13; ch++) {
      int x = 4 + (ch - 1) * bw;
      int h = (base - top) * cnt[ch] / mx;
      uint16_t c = cnt[ch] >= mx && cnt[ch] > 0 ? C_RED : C_GREEN;
      tft.fillRect(x + 1, base - h, bw - 2, h, c);
      tft.setTextColor(C_GREY);
      tft.setCursor(x + 1, base + 2);
      tft.print(ch % 10);
      if (cnt[ch]) {
        tft.setTextColor(C_WHITE);
        tft.setCursor(x + 1, base - h - 9);
        tft.print(cnt[ch]);
      }
    }
    tft.setTextColor(C_CYAN);
    tft.setCursor(4, 15);
    tft.printf("Total: %d networks", netCount);
  }
}

void doBleScan() {
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults res = scan->getResults(4000, false);
  bleCount = 0;
  for (int i = 0; i < (int)res.getCount() && bleCount < BLE_MAX; i++) {
    const NimBLEAdvertisedDevice *d = res.getDevice(i);
    std::string nm = d->getName();
    snprintf(bleDevs[bleCount].name, sizeof(bleDevs[0].name), "%s", nm.empty() ? "<unnamed>" : nm.c_str());
    snprintf(bleDevs[bleCount].addr, sizeof(bleDevs[0].addr), "%s", d->getAddress().toString().c_str());
    bleDevs[bleCount].rssi = d->getRSSI();
    bleCount++;
  }
  scan->clearResults();
  for (int i = 0; i < bleCount; i++)
    for (int j = i + 1; j < bleCount; j++)
      if (bleDevs[j].rssi > bleDevs[i].rssi) { BleDev t = bleDevs[i]; bleDevs[i] = bleDevs[j]; bleDevs[j] = t; }
}

void bleScanScreen() {
  tft.fillRect(0, 12, W, H - 22, C_BLACK);
  tft.setTextColor(C_YELLOW);
  tft.setCursor(6, 30);
  tft.print("SCANNING BLE...");
  doBleScan();
  tft.fillRect(0, 12, W, H - 22, C_BLACK);
}

void runBle(Btn b, int key) {
  static int sel = 0, lastSel = -1, top = 0;
  const int rowH = 10;
  int rows = (H - 34) / rowH;

  if (needRedraw) {
    drawHeader("BLE SCANNER");
    drawFooter("Enter=scan Esc=back");
    bleScanScreen();
    sel = 0; lastSel = -1; top = 0;
    needRedraw = false;
  }
  if (b == B_ENTER) { bleScanScreen(); lastSel = -1; }

  if (bleCount <= 0) {
    if (lastSel != -2) {
      tft.setTextColor(C_RED);
      tft.setCursor(6, 30);
      tft.print("No devices found");
      lastSel = -2;
    }
    return;
  }

  sel = navSel(bleCount, key, sel);
  if (sel == lastSel) return;
  if (sel < top) top = sel;
  if (sel >= top + rows) top = sel - rows + 1;

  tft.fillRect(0, 13, W, rows * rowH + 1, C_BLACK);
  int maxChars = (W - 22) / 6;
  for (int r = 0; r < rows && top + r < bleCount; r++) {
    int i = top + r, y = 14 + r * rowH;
    bool on = (i == sel);
    if (on) tft.fillRect(0, y - 1, W, rowH, C_DKGREEN);
    tft.setTextColor(on ? C_YELLOW : C_CYAN);
    tft.setCursor(2, y);
    char nm[32];
    snprintf(nm, sizeof(nm), "%.*s", maxChars, bleDevs[i].name);
    tft.print(nm);
    drawRssiBar(W - 14, y - 1, bleDevs[i].rssi);
  }
  int dy = H - 21;
  tft.fillRect(0, dy - 1, W, 11, C_BLACK);
  tft.setTextColor(C_GREEN);
  tft.setCursor(2, dy);
  tft.printf("%s %d", bleDevs[sel].addr, bleDevs[sel].rssi);
  lastSel = sel;
}

void kbLine(const char *s, uint16_t c) {
  tft.fillRect(0, H - 23, W, 12, C_BLACK);
  tft.setTextColor(c);
  tft.setCursor(4, H - 21);
  tft.print(s);
}

void kbDrawInfo() {
  tft.fillRect(0, 13, W, H - 24, C_BLACK);
  if (kbConnected()) {
    tft.setTextColor(C_GREEN);
    tft.setCursor(4, 18); tft.print("CONNECTED:");
    tft.setCursor(4, 30); tft.print(kbName);
    tft.setTextColor(C_CYAN);
    tft.setCursor(4, 48); tft.print("Go to TERMINAL and type!");
    tft.setCursor(4, 60); tft.print("Esc = menu");
    return;
  }
  tft.setTextColor(C_YELLOW);
  tft.setCursor(4, 16); tft.print("Waiting for keyboard");
  tft.setTextColor(C_GREEN);
  tft.setCursor(4, 30); tft.print("1) Hold F1/F2/F3 for");
  tft.setCursor(4, 40); tft.print("   3 sec");
  tft.setCursor(4, 50); tft.print("   (light blinks fast)");
  tft.setCursor(4, 62); tft.print("2) If asked for code:");
  tft.setTextColor(C_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(16, 74); tft.print(BLE_PASSKEY);
  tft.setTextSize(1);
  tft.setCursor(100, 80); tft.print("+ Enter");
}

bool isKeyboard(const NimBLEAdvertisedDevice *d) {
  if (NimBLEDevice::isBonded(d->getAddress())) return true;
  if (d->isAdvertisingService(NimBLEUUID((uint16_t)0x1812))) return true;
  return d->haveAppearance() && d->getAppearance() == 0x03C1;
}

void kbConnect() {
  kbLine("Searching...", C_YELLOW);
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults res = scan->getResults(5000, false);

  bool found = false;
  NimBLEAddress addr;
  for (int i = 0; i < (int)res.getCount() && !found; i++) {
    const NimBLEAdvertisedDevice *d = res.getDevice(i);
    if (isKeyboard(d)) {
      found = true;
      addr = d->getAddress();
      std::string nm = d->getName();
      snprintf(kbName, sizeof(kbName), "%s", nm.empty() ? addr.toString().c_str() : nm.c_str());
    }
  }
  scan->clearResults();
  if (!found) {
    kbLine("Not found, retrying...", C_GREY);
    return;
  }

  char buf[40];
  snprintf(buf, sizeof(buf), "Connecting: %s", kbName);
  kbLine(buf, C_CYAN);
  Serial.printf("[KB] found: %s (%s)\n", kbName, addr.toString().c_str());

  if (!kbClient) kbClient = NimBLEDevice::createClient();
  if (!kbClient->connect(addr)) {
    Serial.println("[KB] 1) connect FAILED");
    kbLine("Connect failed, retry", C_RED);
    return;
  }
  Serial.println("[KB] 1) connect OK");
  kbLine("1/3 connected, pairing...", C_CYAN);

  if (!kbClient->secureConnection()) {
    Serial.println("[KB] 2) pairing FAILED");
    kbClient->disconnect();
    kbLine("Pairing failed", C_RED);
    return;
  }
  Serial.println("[KB] 2) pairing OK");
  kbLine("2/3 pairing OK", C_CYAN);

  NimBLERemoteService *hid = kbClient->getService(NimBLEUUID((uint16_t)0x1812));
  if (!hid) {
    Serial.println("[KB] 3) NO HID service");
    kbClient->disconnect();
    kbLine("No HID service", C_RED);
    return;
  }
  int subs = 0;
  const std::vector<NimBLERemoteCharacteristic *> &chars = hid->getCharacteristics(true);
  for (NimBLERemoteCharacteristic *c : chars) {
    bool report = c->getUUID() == NimBLEUUID((uint16_t)0x2A4D) ||
                  c->getUUID() == NimBLEUUID((uint16_t)0x2A22);
    if (report && c->canNotify() && c->subscribe(true, onKbReport)) subs++;
  }
  Serial.printf("[KB] 3) subscribed to %d reports\n", subs);
  if (subs == 0) {
    kbClient->disconnect();
    kbLine("3/3 subscribe FAILED", C_RED);
    return;
  }

  drawHeader("KEYBOARD (BLE)");
  kbDrawInfo();
  delay(1000);
  enterMode(kbBackMode);
}

void runKbd() {
  static unsigned long lastTry = 0;
  if (needRedraw) {
    drawHeader("KEYBOARD (BLE)");
    drawFooter(kbConnected() ? "Esc=menu" : "waiting for keyboard");
    kbDrawInfo();
    lastTry = 0;
    needRedraw = false;
  }
  if (!kbConnected() && millis() - lastTry > 1500) {
    kbConnect();
    lastTry = millis();
  }
}

void termPrint(const String &s) {
  int cols = W / 6;
  int start = 0;
  while (true) {
    int nl = s.indexOf('\n', start);
    String part = nl < 0 ? s.substring(start) : s.substring(start, nl);
    do {
      if (termCount == TERM_MAX) {
        for (int i = 1; i < TERM_MAX; i++) termLines[i - 1] = termLines[i];
        termCount--;
      }
      termLines[termCount++] = part.substring(0, cols);
      part = part.length() > (unsigned)cols ? part.substring(cols) : String();
    } while (part.length());
    if (nl < 0) break;
    start = nl + 1;
  }
}

void termRedraw() {
  int rows = (H - 26) / 8;
  tft.fillRect(0, 13, W, H - 13, C_BLACK);
  tft.setTextColor(C_GREEN);
  int start = max(0, termCount - rows);
  for (int i = start; i < termCount; i++) {
    tft.setCursor(0, 14 + (i - start) * 8);
    tft.print(termLines[i]);
  }
  const char *pfx = termPrompt == P_PASS ? "pw> " : termPrompt == P_GUESS ? "?> " : "> ";
  String shown;
  if (termPrompt == P_PASS) for (size_t i = 0; i < termInput.length(); i++) shown += '*';
  else shown = termInput;
  int cols = W / 6 - strlen(pfx) - 1;
  if ((int)shown.length() > cols) shown = shown.substring(shown.length() - cols);
  tft.drawFastHLine(0, H - 11, W, C_DKGREEN);
  tft.setTextColor(C_YELLOW);
  tft.setCursor(0, H - 9);
  tft.print(pfx);
  tft.print(shown);
  tft.print("_");
}

void termOpen() {
  enterMode(M_TERM);
  drawHeader("TERMINAL");
  if (termCount == 0) {
    termPrint("CyberDeck OS v3.0");
    termPrint("type 'help' and press Enter");
  }
  termRedraw();
  needRedraw = false;
}

void histAdd(const String &s) {
  if (!s.length() || (histCount && hist[histCount - 1] == s)) return;
  if (histCount == HIST_MAX) {
    for (int i = 1; i < HIST_MAX; i++) hist[i - 1] = hist[i];
    histCount--;
  }
  hist[histCount++] = s;
}

void wifiConnect(const String &ssid, const String &pass) {
  termPrint(String("Connecting: ") + ssid);
  termRedraw();
  WiFi.disconnect();
  delay(100);
  if (pass.length()) WiFi.begin(ssid.c_str(), pass.c_str());
  else WiFi.begin(ssid.c_str());

  unsigned long t0 = millis();
  while (!wifiUp() && millis() - t0 < 15000) {
    delay(500);
    termLines[termCount - 1] += ".";
    termRedraw();
  }
  if (wifiUp()) {
    termPrint("CONNECTED!");
    termPrint(String("IP: ") + WiFi.localIP().toString());
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    termPrint("Saved.");
    drawHeader("TERMINAL");
  } else {
    termPrint(String("Failed (code ") + (int)WiFi.status() + ")");
    termPrint("Check password, retry.");
    WiFi.disconnect();
  }
}

struct Calc {
  const char *p;
  bool err = false;

  void ws() { while (*p == ' ') p++; }

  double expr() {
    double v = term();
    while (true) {
      ws();
      if (*p == '+') { p++; v += term(); }
      else if (*p == '-') { p++; v -= term(); }
      else return v;
    }
  }

  double term() {
    double v = power();
    while (true) {
      ws();
      if (*p == '*') { p++; v *= power(); }
      else if (*p == '/') { p++; v /= power(); }
      else if (*p == '%') { p++; v = fmod(v, power()); }
      else return v;
    }
  }

  double power() {
    double v = factor();
    ws();
    if (*p == '^') { p++; return pow(v, power()); }
    return v;
  }

  double factor() {
    ws();
    if (*p == '-') { p++; return -factor(); }
    if (*p == '+') { p++; return factor(); }
    if (*p == '(') {
      p++;
      double v = expr();
      ws();
      if (*p == ')') p++; else err = true;
      return v;
    }
    char *end;
    double v = strtod(p, &end);
    if (end == p) { err = true; return 0; }
    p = end;
    return v;
  }
};

void guessStep(const String &in) {
  int v = in.toInt();
  if (v < 1 || v > 100) { termPrint("Enter a number 1-100"); return; }
  guessTries++;
  if (v < guessNum) termPrint("Go HIGHER ^");
  else if (v > guessNum) termPrint("Go LOWER v");
  else {
    termPrint(String("YOU GOT IT! ") + guessTries + " tries");
    termPrompt = P_NONE;
  }
}

void showNet(const String &title, const String &text) {
  if (text.length() == 0) { termPrint("(empty response)"); return; }
  if (text.length() < 200 && text.indexOf('\n') < 0) { termPrint(text); return; }
  viewOpen(title, text);
}

void termExec(String cmd) {
  cmd.trim();
  if (!cmd.length()) return;
  int sp = cmd.indexOf(' ');
  String name = sp < 0 ? cmd : cmd.substring(0, sp);
  name.toLowerCase();
  String arg = sp < 0 ? String() : cmd.substring(sp + 1);
  arg.trim();

  if (name == "help") {
    termPrint("-- WiFi / internet --");
    termPrint("wifi      connect <n>  status");
    termPrint("disconnect forget ip myip");
    termPrint("google <word> ddg <word>");
    termPrint("get <site> weather <city>");
    termPrint("joke time dns <site>");
    termPrint("-- system --");
    termPrint("info uptime ble sys reboot");
    termPrint("clear history echo menu");
    termPrint("-- fun --");
    termPrint("calc <expr> roll coin");
    termPrint("guess snake hack matrix");
  } else if (name == "clear" || name == "cls") {
    termCount = 0;
  } else if (name == "info") {
    termPrint(String("Chip: ") + ESP.getChipModel() + " " + ESP.getCpuFreqMHz() + "MHz");
    termPrint(String("Heap: ") + ESP.getFreeHeap() + " B");
    termPrint(String("MAC : ") + WiFi.macAddress());
    termPrint(String("Kbd : ") + (kbConnected() ? kbName : "none"));
  } else if (name == "uptime") {
    unsigned long s = millis() / 1000;
    char buf[24];
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
    termPrint(buf);
  } else if (name == "time" || name == "date") {
    struct tm t;
    if (getLocalTime(&t, 2000)) {
      char buf[32];
      strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M:%S", &t);
      termPrint(buf);
    } else {
      termPrint("No clock (WiFi required)");
    }
  } else if (name == "wifi") {
    termPrint("Scanning...");
    termRedraw();
    netCount = WiFi.scanNetworks();
    scanSsidCount = min(netCount, 9);
    termPrint(String(netCount) + " networks found:");
    for (int i = 0; i < scanSsidCount; i++) {
      scanSsid[i] = WiFi.SSID(i);
      bool open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
      termPrint(String(i + 1) + ") " + scanSsid[i].substring(0, 15) + " " + WiFi.RSSI(i) + (open ? "" : " *"));
    }
    termPrint("type 'connect <number>'");
  } else if (name == "connect") {
    if (!arg.length()) { termPrint("Usage: connect <number|name>"); return; }
    String ssid = arg;
    int n = arg.toInt();
    if (n >= 1 && n <= scanSsidCount && String(n) == arg) ssid = scanSsid[n - 1];
    pendingSsid = ssid;
    termPrompt = P_PASS;
    termPrint(String("Password (") + ssid + "):");
    termPrint("(empty Enter if open)");
  } else if (name == "disconnect") {
    WiFi.disconnect();
    termPrint("WiFi disconnected");
  } else if (name == "forget") {
    prefs.remove("ssid");
    prefs.remove("pass");
    WiFi.disconnect();
    termPrint("Saved WiFi removed");
  } else if (name == "status") {
    if (wifiUp()) {
      termPrint(String("WiFi: ") + WiFi.SSID() + " " + WiFi.RSSI() + "dBm");
      termPrint(String("IP  : ") + WiFi.localIP().toString());
    } else {
      termPrint("WiFi: not connected");
    }
    termPrint(String("Kbd : ") + (kbConnected() ? kbName : "none"));
  } else if (name == "ip") {
    termPrint(String("IP : ") + WiFi.localIP().toString());
    termPrint(String("GW : ") + WiFi.gatewayIP().toString());
    termPrint(String("DNS: ") + WiFi.dnsIP().toString());
    termPrint(String("MAC: ") + WiFi.macAddress());
  } else if (name == "myip") {
    termPrint("Loading...");
    termRedraw();
    String out;
    netFetch("https://api.ipify.org", out, 100, "text/plain");
    termPrint(String("Public IP: ") + out);
  } else if (name == "dns") {
    if (!arg.length()) { termPrint("Usage: dns <site>"); return; }
    IPAddress ip;
    if (WiFi.hostByName(arg.c_str(), ip)) termPrint(arg + " = " + ip.toString());
    else termPrint("Not found");
  } else if (name == "get" || name == "open") {
    if (!arg.length()) { termPrint("Usage: get <site>"); return; }
    termPrint("Loading...");
    termRedraw();
    String out;
    netFetch(arg, out, VIEW_MAX_CHARS);
    showNet(arg, out);
  } else if (name == "google" || name == "ddg") {
    if (!arg.length()) { termPrint(String("Usage: ") + name + " <word>"); return; }
    termPrint("Searching...");
    termRedraw();
    String url = name == "google"
                   ? String("https://www.google.com/search?hl=en&gbv=1&q=") + urlEncode(arg)
                   : String("https://lite.duckduckgo.com/lite/?q=") + urlEncode(arg);
    String out;
    netFetch(url, out, VIEW_MAX_CHARS);
    showNet(name + ": " + arg, out);
  } else if (name == "weather") {
    String city = arg.length() ? arg : String("Baku");
    termPrint("Loading...");
    termRedraw();
    String out;
    netFetch(String("https://wttr.in/") + urlEncode(city) + "?format=%25l:+%25t+%25h+%25w&m", out, 200, "text/plain");
    termPrint(out);
  } else if (name == "joke") {
    termPrint("Loading...");
    termRedraw();
    String out;
    netFetch("https://icanhazdadjoke.com/", out, 400, "text/plain");
    termPrint(out);
  } else if (name == "ble") {
    termPrint("BLE scan (4 sec)...");
    termRedraw();
    doBleScan();
    termPrint(String(bleCount) + " devices:");
    for (int i = 0; i < bleCount && i < 6; i++) termPrint(String(" ") + bleDevs[i].name + " " + bleDevs[i].rssi);
  } else if (name == "calc") {
    if (!arg.length()) { termPrint("Ex: calc (2+3)*4^2"); return; }
    Calc c;
    c.p = arg.c_str();
    double v = c.expr();
    c.ws();
    if (c.err || *c.p) termPrint("Invalid expression");
    else { char buf[32]; snprintf(buf, sizeof(buf), "= %.10g", v); termPrint(buf); }
  } else if (name == "roll") {
    termPrint(String("Dice: ") + (int)random(1, 7));
  } else if (name == "coin") {
    termPrint(random(2) ? "HEADS" : "TAILS");
  } else if (name == "guess") {
    guessNum = random(1, 101);
    guessTries = 0;
    termPrompt = P_GUESS;
    termPrint("I picked a number 1-100. Guess!");
    termPrint("(Esc to quit)");
  } else if (name == "echo") {
    termPrint(arg);
  } else if (name == "history") {
    for (int i = 0; i < histCount; i++) termPrint(String(i + 1) + " " + hist[i]);
  } else if (name == "snake" || name == "worm") {
    enterMode(M_SNAKE);
  } else if (name == "hack") {
    enterMode(M_HACK);
  } else if (name == "matrix") {
    enterMode(M_MATRIX);
  } else if (name == "sys") {
    enterMode(M_SYS);
  } else if (name == "menu" || name == "exit") {
    enterMode(M_MENU);
  } else if (name == "reboot") {
    termPrint("Rebooting...");
    termRedraw();
    delay(500);
    ESP.restart();
  } else {
    termPrint(String("?? ") + name + " (type help)");
  }
}

void runTerm(int key) {
  if (needRedraw) { termOpen(); return; }
  if (key < 0) return;

  if (key == KEY_ESC) {
    if (termPrompt != P_NONE) {
      termPrompt = P_NONE;
      termInput = "";
      termPrint("(cancelled)");
      termRedraw();
      return;
    }
    enterMode(M_MENU);
    return;
  }

  if (key == '\n') {
    String in = termInput;
    termInput = "";
    histPos = -1;
    if (termPrompt == P_PASS) {
      termPrompt = P_NONE;
      wifiConnect(pendingSsid, in);
    } else if (termPrompt == P_GUESS) {
      termPrint(String("?> ") + in);
      guessStep(in);
    } else {
      termPrint(String("> ") + in);
      histAdd(in);
      termExec(in);
    }
    if (mode != M_TERM) return;
  } else if (key == KEY_UP && termPrompt == P_NONE) {
    if (!histCount) return;
    histPos = histPos < 0 ? histCount - 1 : max(0, histPos - 1);
    termInput = hist[histPos];
  } else if (key == KEY_DOWN && termPrompt == P_NONE) {
    if (histPos < 0) return;
    if (++histPos >= histCount) { histPos = -1; termInput = ""; }
    else termInput = hist[histPos];
  } else if (key == KEY_BKSP) {
    if (termInput.length()) termInput.remove(termInput.length() - 1);
  } else if (key >= 32 && key < 127) {
    if (termInput.length() < 120) termInput += (char)key;
  } else {
    return;
  }
  termRedraw();
}

void viewOpen(const String &title, const String &text) {
  viewTitle = title;
  viewText = text;

  int cols = W / 6;
  int n = viewText.length(), i = 0;
  viewLines = 0;
  while (i < n && viewLines < VIEW_MAX_LINES) {
    viewLine[viewLines++] = i;
    int j = i, lastSp = -1;
    while (j < n && j - i < cols && viewText[j] != '\n') {
      if (viewText[j] == ' ') lastSp = j;
      j++;
    }
    if (j < n && viewText[j] == '\n') i = j + 1;
    else if (j < n && lastSp > i) i = lastSp + 1;
    else i = j;
  }
  if (viewLines == 0) viewLine[viewLines++] = 0;
  viewTop = 0;
  enterMode(M_VIEW);
}

void runView(int key) {
  int rows = (H - 24) / 8, cols = W / 6;
  bool dirty = needRedraw;
  if (needRedraw) { drawFooter("arrows=scroll Esc=back"); needRedraw = false; }

  int maxTop = max(0, viewLines - rows);
  if (key == KEY_DOWN && viewTop < maxTop) { viewTop++; dirty = true; }
  if (key == KEY_UP && viewTop > 0) { viewTop--; dirty = true; }
  if ((key == KEY_RIGHT || key == ' ') && viewTop < maxTop) { viewTop = min(viewTop + rows, maxTop); dirty = true; }
  if (key == KEY_LEFT && viewTop > 0) { viewTop = max(0, viewTop - rows); dirty = true; }
  if (!dirty) return;

  char hdr[40];
  snprintf(hdr, sizeof(hdr), "%.13s %d/%d", viewTitle.c_str(), min(viewTop + rows, viewLines), viewLines);
  drawHeader(hdr);
  tft.fillRect(0, 13, W, H - 24, C_BLACK);
  tft.setTextColor(C_GREEN);
  for (int r = 0; r < rows && viewTop + r < viewLines; r++) {
    int k = viewTop + r;
    int s = viewLine[k];
    int e = k + 1 < viewLines ? viewLine[k + 1] : viewText.length();
    String line = viewText.substring(s, min(e, s + cols));
    line.replace("\n", "");
    tft.setCursor(0, 14 + r * 8);
    tft.print(line);
  }
}

void runSys(Btn) {
  static unsigned long last = 0;
  if (needRedraw) {
    drawHeader("SYSTEM INFO");
    drawFooter("Esc=back");
    needRedraw = false;
    last = 0;
  }
  if (millis() - last < 1000) return;
  last = millis();

  tft.fillRect(0, 14, W, H - 26, C_BLACK);
  tft.setCursor(0, 16);
  tft.setTextColor(C_GREEN);
  unsigned long s = millis() / 1000;
  tft.printf(" Chip : %s\n", ESP.getChipModel());
  tft.printf(" Cores: %d Rev %d\n", ESP.getChipCores(), ESP.getChipRevision());
  tft.printf(" CPU  : %d MHz\n", ESP.getCpuFreqMHz());
  tft.printf(" Flash: %u KB\n", ESP.getFlashChipSize() / 1024);
  tft.printf(" Heap : %u B\n", ESP.getFreeHeap());
  tft.setTextColor(C_CYAN);
  tft.printf(" MAC  :\n %s\n", WiFi.macAddress().c_str());
  tft.setTextColor(C_YELLOW);
  tft.printf(" Uptime %02lu:%02lu:%02lu\n", s / 3600, (s / 60) % 60, s % 60);
  tft.printf(" Kbd  : %s\n", kbConnected() ? "connected" : "none");
  tft.printf(" WiFi : %s\n", wifiUp() ? WiFi.SSID().c_str() : "none");
}

#define SN_MAX 600
#define SN_CELL 6
uint8_t snX[SN_MAX], snY[SN_MAX];
int snLen, snDx, snDy, snNdx, snNdy, snFx, snFy, snScore, snHi, snGW, snGH, snStep;
bool snAlive;
unsigned long snLast;

void snCell(int x, int y, uint16_t c) { tft.fillRect(x * SN_CELL, 13 + y * SN_CELL, SN_CELL - 1, SN_CELL - 1, c); }

bool snOn(int x, int y) {
  for (int i = 0; i < snLen; i++) if (snX[i] == x && snY[i] == y) return true;
  return false;
}

void snFood() {
  do { snFx = random(snGW); snFy = random(snGH); } while (snOn(snFx, snFy));
  snCell(snFx, snFy, C_MAGENTA);
}

void snHeader() {
  char b[32];
  snprintf(b, sizeof(b), "CYBER WORM %d hi:%d", snScore, snHi);
  drawHeader(b);
}

void snReset() {
  snGW = W / SN_CELL;
  snGH = (H - 13) / SN_CELL;
  tft.fillRect(0, 12, W, H - 12, C_BLACK);
  snLen = 3;
  for (int i = 0; i < snLen; i++) { snX[i] = snGW / 2 - i; snY[i] = snGH / 2; }
  snDx = snNdx = 1; snDy = snNdy = 0;
  for (int i = 0; i < snLen; i++) snCell(snX[i], snY[i], i ? C_GREEN : C_CYAN);
  snScore = 0;
  snStep = 150;
  snAlive = true;
  snLast = millis();
  snFood();
  snHeader();
}

void runSnake(Btn b, int key) {
  if (needRedraw) {
    snHi = prefs.getInt("worm_hi", 0);
    snReset();
    needRedraw = false;
  }
  if (!snAlive) { if (b == B_ENTER) snReset(); return; }

  if (key == KEY_UP && snDy == 0) { snNdx = 0; snNdy = -1; }
  if (key == KEY_DOWN && snDy == 0) { snNdx = 0; snNdy = 1; }
  if (key == KEY_LEFT && snDx == 0) { snNdx = -1; snNdy = 0; }
  if (key == KEY_RIGHT && snDx == 0) { snNdx = 1; snNdy = 0; }

  if (millis() - snLast < (unsigned long)snStep) return;
  snLast = millis();
  snDx = snNdx; snDy = snNdy;

  int nx = snX[0] + snDx, ny = snY[0] + snDy;
  bool hit = nx < 0 || ny < 0 || nx >= snGW || ny >= snGH;
  for (int i = 0; !hit && i < snLen - 1; i++) if (snX[i] == nx && snY[i] == ny) hit = true;
  if (hit) {
    snAlive = false;
    if (snScore > snHi) { snHi = snScore; prefs.putInt("worm_hi", snHi); }
    drawGameOver(snScore, snHi);
    return;
  }

  bool grow = (nx == snFx && ny == snFy);
  if (grow && snLen < SN_MAX) snLen++;
  else snCell(snX[snLen - 1], snY[snLen - 1], C_BLACK);
  for (int i = snLen - 1; i > 0; i--) { snX[i] = snX[i - 1]; snY[i] = snY[i - 1]; }
  snX[0] = nx; snY[0] = ny;
  snCell(snX[1], snY[1], C_GREEN);
  snCell(nx, ny, C_CYAN);

  if (grow) {
    snScore++;
    snStep = max(60, snStep - 5);
    snFood();
    snHeader();
  }
}

const char *hackWords[] = {
  "root", "sudo", "ping", "hash", "node", "byte", "grep", "kernel", "proxy", "shell",
  "admin", "login", "cache", "stack", "virus", "patch", "debug", "linux", "crypt", "token",
  "cyber", "matrix", "socket", "packet", "binary", "script", "server", "router", "access",
  "denied", "hacker", "malware", "exploit", "firewall", "decrypt", "backdoor", "payload",
  "trojan", "botnet", "cipher", "daemon", "buffer", "pixel", "glitch", "neon", "signal" };
const int HW_N = sizeof(hackWords) / sizeof(hackWords[0]);

struct FallWord { const char *w; int16_t x; float y; bool on; };
#define HK_MAX 4
FallWord hk[HK_MAX];
String hkIn;
int hkScore, hkLives, hkHi, hkSpawnMs;
float hkSpeed;
bool hkAlive;
unsigned long hkLast, hkSpawn;

int hkFloor() { return H - 22; }

void hkHeader() {
  char b[32];
  snprintf(b, sizeof(b), "TYPE HACK S:%d L:%d", hkScore, hkLives);
  drawHeader(b);
}

void hkDrawInput() {
  tft.fillRect(0, H - 11, W, 11, C_BLACK);
  tft.drawFastHLine(0, H - 12, W, C_DKGREEN);
  tft.setTextColor(C_YELLOW);
  tft.setCursor(2, H - 9);
  tft.print("> ");
  tft.print(hkIn);
  tft.print("_");
}

void hkDrawWord(int i, bool erase) {
  int len = strlen(hk[i].w);
  int y = (int)hk[i].y;
  if (erase) { tft.fillRect(hk[i].x, y, len * 6, 8, C_BLACK); return; }
  int typed = (hkIn.length() && strncmp(hk[i].w, hkIn.c_str(), hkIn.length()) == 0) ? hkIn.length() : 0;
  tft.setCursor(hk[i].x, y);
  for (int c = 0; c < len; c++) {
    tft.setTextColor(c < typed ? C_YELLOW : C_CYAN, C_BLACK);
    tft.print(hk[i].w[c]);
  }
}

void hkReset() {
  tft.fillRect(0, 12, W, H - 12, C_BLACK);
  for (int i = 0; i < HK_MAX; i++) hk[i].on = false;
  hkIn = "";
  hkScore = 0;
  hkLives = 3;
  hkSpeed = 0.35f;
  hkSpawnMs = 2200;
  hkAlive = true;
  hkLast = hkSpawn = millis();
  hkHeader();
  hkDrawInput();
}

void hkSpawnWord() {
  for (int i = 0; i < HK_MAX; i++) {
    if (hk[i].on) continue;
    hk[i].w = hackWords[random(HW_N)];
    int len = strlen(hk[i].w);
    hk[i].x = random(0, max(1, W - len * 6));
    hk[i].y = 14;
    hk[i].on = true;
    hkDrawWord(i, false);
    return;
  }
}

void runHack(Btn b, int key) {
  if (needRedraw) {
    hkHi = prefs.getInt("hack_hi", 0);
    hkReset();
    needRedraw = false;
  }
  if (!hkAlive) { if (b == B_ENTER) hkReset(); return; }

  bool typedChange = false;
  if (key >= 'A' && key <= 'Z') key += 32;
  if (key >= 'a' && key <= 'z' && hkIn.length() < 12) { hkIn += (char)key; typedChange = true; }
  else if (key == KEY_BKSP && hkIn.length()) { hkIn.remove(hkIn.length() - 1); typedChange = true; }
  else if (b == B_ENTER) { hkIn = ""; typedChange = true; }

  if (typedChange) {
    for (int i = 0; i < HK_MAX; i++) {
      if (hk[i].on && hkIn == hk[i].w) {
        hkDrawWord(i, true);
        hk[i].on = false;
        hkScore += strlen(hk[i].w);
        hkSpeed += 0.03f;
        hkSpawnMs = max(700, hkSpawnMs - 70);
        hkIn = "";
        hkHeader();
        break;
      }
    }
    hkDrawInput();
    for (int i = 0; i < HK_MAX; i++) if (hk[i].on) hkDrawWord(i, false);
  }

  bool any = false;
  for (int i = 0; i < HK_MAX; i++) any |= hk[i].on;
  if (!any || millis() - hkSpawn >= (unsigned long)hkSpawnMs) { hkSpawnWord(); hkSpawn = millis(); }

  if (millis() - hkLast < 40) return;
  hkLast = millis();
  for (int i = 0; i < HK_MAX; i++) {
    if (!hk[i].on) continue;
    int oldY = (int)hk[i].y;
    float ny = hk[i].y + hkSpeed;
    if ((int)ny == oldY) { hk[i].y = ny; continue; }
    hkDrawWord(i, true);
    hk[i].y = ny;
    if (ny >= hkFloor()) {
      hk[i].on = false;
      hkLives--;
      hkHeader();
      if (hkLives <= 0) {
        hkAlive = false;
        if (hkScore > hkHi) { hkHi = hkScore; prefs.putInt("hack_hi", hkHi); }
        drawGameOver(hkScore, hkHi);
        return;
      }
      continue;
    }
    hkDrawWord(i, false);
  }
}

#define MAX_COLS 64
int16_t dropY[MAX_COLS];

void runMatrix(int key) {
  static unsigned long last = 0;
  static int speed = 60;
  int cols = min(W / 6, MAX_COLS), rows = H / 8;

  if (needRedraw) {
    tft.fillScreen(C_BLACK);
    for (int c = 0; c < cols; c++) dropY[c] = -random(rows);
    needRedraw = false;
  }

  if (key == KEY_UP) speed = max(15, speed - 15);
  if (key == KEY_DOWN) speed = min(150, speed + 15);

  if (millis() - last < (unsigned long)speed) return;
  last = millis();

  for (int c = 0; c < cols; c++) {
    int y = dropY[c];
    int x = c * 6;
    if (y >= 0 && y < rows) {
      tft.fillRect(x, y * 8, 6, 8, C_BLACK);
      tft.setTextColor(C_WHITE);
      tft.setCursor(x, y * 8);
      tft.print((char)random(33, 126));
    }
    if (y - 1 >= 0 && y - 1 < rows) {
      tft.fillRect(x, (y - 1) * 8, 6, 8, C_BLACK);
      tft.setTextColor(C_GREEN);
      tft.setCursor(x, (y - 1) * 8);
      tft.print((char)random(33, 126));
    }
    int tail = y - 8;
    if (tail >= 0 && tail < rows) tft.fillRect(x, tail * 8, 6, 8, C_BLACK);
    dropY[c]++;
    if (tail >= rows) dropY[c] = -random(rows);
  }
}

void setup() {
  Serial.begin(115200);
#if USE_ST7735
  tft.initR(INITR_BLACKTAB);
#else
  tft.begin();
#endif
  tft.setRotation(1);
  W = tft.width();
  H = tft.height();
  randomSeed(esp_random());

  bootScreen();

  prefs.begin("cdeck", false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");

  String ssid = prefs.getString("ssid", "");
  if (ssid.length()) {
    String pass = prefs.getString("pass", "");
    if (pass.length()) WiFi.begin(ssid.c_str(), pass.c_str());
    else WiFi.begin(ssid.c_str());
  }

  NimBLEDevice::init("CyberDeck");
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(BLE_PASSKEY);

  enterMode(M_KBD);
}

void loop() {
  int key = kbRead();
  Btn b = B_NONE;

  if (mode != M_TERM) {
    if (key == '\n') b = B_ENTER;
    else if (key == KEY_ESC) b = B_ESC;
  }

  static bool wasConn = false;
  bool conn = kbConnected();
  if (wasConn && !conn && mode != M_KBD) {
    wasConn = false;
    kbBackMode = mode == M_VIEW ? M_TERM : mode;
    enterMode(M_KBD);
    return;
  }
  wasConn = conn;

  if (b == B_ESC) {
    if (mode == M_VIEW) { termOpen(); return; }
    if (mode != M_MENU) { enterMode(M_MENU); return; }
  }

  switch (mode) {
    case M_MENU:   runMenu(b, key); break;
    case M_WIFI:   runWifi(b, key); break;
    case M_CHAN:   runChannels(b); break;
    case M_BLE:    runBle(b, key); break;
    case M_KBD:    runKbd(); break;
    case M_TERM:   runTerm(key); break;
    case M_SYS:    runSys(b); break;
    case M_SNAKE:  runSnake(b, key); break;
    case M_HACK:   runHack(b, key); break;
    case M_MATRIX: runMatrix(key); break;
    case M_VIEW:   runView(key); break;
  }
}
