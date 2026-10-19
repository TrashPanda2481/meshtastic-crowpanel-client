/*
 * CrowPanel 5.79" e-paper  --  Meshtastic client  (step 6: UI, deliberate refresh)
 *
 * Views: MESSAGES / NODES, each a scrollable list with a '>' cursor. Buttons (active-low):
 *   MENU (IO2) = switch view      EXIT (IO1) = back / to Messages
 *   dial up (IO6) / down (IO4) = move cursor      press (IO5) = open the selected entry
 *
 * Refresh is deliberate to avoid flashing:
 *   - dial cursor moves         -> fast PARTIAL refresh (no flash)
 *   - page scroll / view switch -> full (clean) refresh
 *   - open/close detail         -> full refresh (static while reading -> no flashing)
 *   - a new message auto-shows ONLY when idle at the top of Messages; it never
 *     stomps your position while navigating or reading.
 *   - every FULL_EVERY partials, one full refresh clears accumulated ghosting.
 *
 * WiFi creds + node IP come from include/secrets.h (gitignored).
 */
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include "EPD.h"
#include "secrets.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "meshtastic/mesh.pb.h"
#include "DHTesp.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------- display ----------
static uint8_t ImageBW[27200];
static const int EPD_PWR_PIN = 7;

// ---------- input (active-low; board has external pull-ups) ----------
static const int PIN_MENU = 2, PIN_EXIT = 1, PIN_UP = 6, PIN_DOWN = 4, PIN_OK = 5;
enum Ev { EV_NONE, EV_MENU, EV_EXIT, EV_UP, EV_DOWN, EV_OK };
static const int inPins[5] = { PIN_MENU, PIN_EXIT, PIN_UP, PIN_DOWN, PIN_OK };
static const Ev  inEv[5]   = { EV_MENU,  EV_EXIT,  EV_UP,  EV_DOWN,  EV_OK  };
static int       inPrev[5];
static uint32_t  inT[5];
static uint32_t  lastInputMs = 0;

// ---------- Meshtastic framing ----------
static const uint8_t  START1 = 0x94, START2 = 0xC3;
static const uint16_t MAX_FRAME = 512;
static WiFiClient client;
static meshtastic_FromRadio g_fr;

// ---------- DHT11 local sensor (GPIO38) ----------
static DHTesp   dht;
static float    curTemp = 0, curHum = 0;
static bool     dhtOk = false, dhtEver = false;
static uint32_t dhtLastMs = 0;

// ---------- outdoor weather (Open-Meteo) ----------
// WX_LAT / WX_LON are #defined in secrets.h so your location stays out of git.
static float    outTemp = 0, outHum = 0;
static int      outCode = -1;
static bool     outOk = false, outEver = false;
static uint32_t outLastMs = 0;

// ---------- data model ----------
struct Node { uint32_t num; char longName[40]; char shortName[12]; };
struct Msg  { uint32_t from; uint8_t ch; char text[160]; };

static const int MAX_NODES = 32;
static Node nodes[MAX_NODES];
static int  nodeCount = 0;

static const int MAX_MSGS = 20;
static Msg  msgs[MAX_MSGS];
static int  msgNext = 0, msgCount = 0;

// local node's channels (from the config dump) + our own node number
struct Chan { uint8_t role; char name[20]; };   // role: 0=disabled, 1=primary, 2=secondary
static Chan     chans[8];
static uint32_t myNodeNum = 0;
static uint8_t  activeChannel = 1;   // channel index that Messages filters to + Send targets (default = Home)
static int      chanSel = 0;         // cursor in the node-detail channel list

static bool tcpOk = false, wifiOk = false;

// ---------- UI state ----------
enum View { VIEW_MESSAGES, VIEW_NODES, VIEW_SEND, VIEW_WEATHER };
enum Mode { MODE_LIST, MODE_DETAIL };
static View currentView = VIEW_MESSAGES;
static Mode mode = MODE_LIST;
struct ViewState { int sel; int top; };
static ViewState vsMsg = {0, 0}, vsNode = {0, 0}, vsSend = {0, 0};

// snapshot of the entry shown in detail (frozen so later traffic can't change it)
static bool detIsMsg = false;
static Msg  detMsg;
static Node detNode;

// canned messages for the SEND view (label is shown; text is transmitted; private channel)
struct Canned { const char* label; const char* text; };
static const Canned CANNED[] = {
  { "!!!",            "!!!"           },   // emergency send-help code (shown and sent literally)
  { "Received",       "Received"      },
  { "Affirm",         "Affirm"        },
  { "Standby",        "Standby"       },
  { "Negative",       "Negative"      },
  { "Waiting",        "Waiting"       },
  { "Send Again",     "Send Again"    },
  { "All Secure",     "All Secure"    },
  { "ETA?",           "ETA?"          },
  { "Report Status",  "Report Status" },
};
static const int     CANNED_COUNT   = sizeof(CANNED) / sizeof(CANNED[0]);
// (Send target is the user-selected activeChannel, declared with the channel state above.)
static int           sendConfirmIdx = -1;   // canned index awaiting a 2nd press to confirm
static char          sentMsg[40]    = "";   // last-sent text (footer)
static uint32_t      sentAtMs       = 0;

static const int ROWS       = 8;
static const int ROW_H      = 24;
static const int LIST_Y     = 46;
static const int CLEAN_EVERY = 4;   // every Nth redraw = a flashless two-pass clear (1 = cleanest/slower, higher = snappier / more ghost)

// Re-pull the node list / channels over the LIVE link every few minutes, so nodes
// the node learns after connect (e.g. via MQTT) appear without a power-cycle.
static const uint32_t CONFIG_REFRESH_MS = 180000;  // 3 min
static uint32_t cfgLastMs = 0;
static int      prevNodeCount = -1;                 // -1 = never drawn; gates refresh redraws
static int  partialsSinceClean = 0;

static bool     pendRedraw = false, pendFull = false;
static uint32_t lastChange = 0;
static const uint32_t COALESCE_MS = 200;

// ---------- data helpers ----------
static Node* findNode(uint32_t num) {
  for (int i = 0; i < nodeCount; i++) if (nodes[i].num == num) return &nodes[i];
  return nullptr;
}
static bool upsertNode(uint32_t num, const char* ln, const char* sn) {
  Node* n = findNode(num);
  bool isNew = false;
  if (!n) { if (nodeCount >= MAX_NODES) return false; n = &nodes[nodeCount++]; n->num = num; isNew = true; }
  strncpy(n->longName,  ln ? ln : "", sizeof(n->longName) - 1);  n->longName[sizeof(n->longName) - 1] = 0;
  strncpy(n->shortName, sn ? sn : "", sizeof(n->shortName) - 1); n->shortName[sizeof(n->shortName) - 1] = 0;
  return isNew;
}
static const char* nodeName(uint32_t num) {
  static char hex[12];
  Node* n = findNode(num);
  if (n && n->shortName[0]) return n->shortName;
  if (n && n->longName[0])  return n->longName;
  snprintf(hex, sizeof(hex), "!%08lx", (unsigned long)num);
  return hex;
}
static void pushMsg(uint32_t from, uint8_t ch, const char* text) {
  Msg* m = &msgs[msgNext];
  m->from = from; m->ch = ch;
  strncpy(m->text, text, sizeof(m->text) - 1); m->text[sizeof(m->text) - 1] = 0;
  for (char* p = m->text; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
  msgNext = (msgNext + 1) % MAX_MSGS;
  if (msgCount < MAX_MSGS) msgCount++;
}
static const Msg* getMsg(int i) {               // i = 0 -> newest
  int idx = msgNext - 1 - i; idx %= MAX_MSGS; if (idx < 0) idx += MAX_MSGS;
  return &msgs[idx];
}
static int getActiveChans(uint8_t* out) {        // indices of non-disabled channels; returns count
  int n = 0;
  for (int i = 0; i < 8; i++) if (chans[i].role != 0) out[n++] = (uint8_t)i;
  return n;
}
static const char* activeChanName() {
  if (activeChannel < 8 && chans[activeChannel].name[0]) return chans[activeChannel].name;
  if (activeChannel < 8 && chans[activeChannel].role == 1) return "LongFast";
  return "?";
}
static int filtMsgCount() {                       // messages on the active channel
  int n = 0;
  for (int i = 0; i < msgCount; i++) if (getMsg(i)->ch == activeChannel) n++;
  return n;
}
static const Msg* getFiltMsg(int fi) {            // fi-th newest message on the active channel
  int seen = 0;
  for (int i = 0; i < msgCount; i++) {
    const Msg* m = getMsg(i);
    if (m->ch == activeChannel) { if (seen == fi) return m; seen++; }
  }
  return nullptr;
}
static ViewState* activeVS() { return currentView == VIEW_NODES ? &vsNode : currentView == VIEW_SEND ? &vsSend : &vsMsg; }
static int        activeN()  { return currentView == VIEW_MESSAGES ? filtMsgCount() : currentView == VIEW_NODES ? nodeCount : currentView == VIEW_SEND ? CANNED_COUNT : 0; }

// ---------- e-paper ----------
static void panelPowerOn() { pinMode(EPD_PWR_PIN, OUTPUT); digitalWrite(EPD_PWR_PIN, HIGH); delay(10); }

static bool panelReady = false;

// Full (clean) refresh -- brief black/white flash, clears ghosting. Leaves the
// panel INITIALIZED (no deep sleep) so later partial refreshes have a valid
// baseline to diff against.
static void epdFullRefresh() {
  panelPowerOn();
  EPD_GPIOInit();
  EPD_FastMode1Init();
  EPD_Display_Clear();
  EPD_Update();
  EPD_GPIOInit();
  EPD_FastMode1Init();
  EPD_Display(ImageBW);
  EPD_FastUpdate();
  panelReady = true;
}

// Single PARTIAL push -- NO flash. ImageBW must already be composed, and a prior
// full refresh (boot) must have left the panel initialized (no HW reset / deep
// sleep in between) so the controller has a valid baseline to diff against.
static void epdPartialPush() {
  EPD_Display(ImageBW);
  EPD_PartUpdate();
}

// greedy word-wrap drawing
static void wrapText(const char* s, int x, int y0, int size, int maxChars) {
  int y = y0;
  char line[96]; int ll = 0;
  const char* p = s;
  while (*p) {
    const char* ws = p;
    while (*p && *p != ' ') p++;
    int wl = (int)(p - ws);
    if (wl > maxChars) wl = maxChars;
    if (ll > 0 && ll + 1 + wl > maxChars) { line[ll] = 0; EPD_ShowString(x, y, line, size, BLACK); y += size + 6; ll = 0; }
    if (ll > 0) line[ll++] = ' ';
    for (int i = 0; i < wl && ll < (int)sizeof(line) - 1; i++) line[ll++] = ws[i];
    while (*p == ' ') p++;
  }
  if (ll > 0) { line[ll] = 0; EPD_ShowString(x, y, line, size, BLACK); }
}

static void drawTabs() {
  const char* names[4] = { "MSGS", "NODES", "SEND", "WX" };
  const View  vtab[4]  = { VIEW_MESSAGES, VIEW_NODES, VIEW_SEND, VIEW_WEATHER };
  const int   xtab[4]  = { 8, 120, 260, 380 };
  for (int i = 0; i < 4; i++) {
    EPD_ShowString(xtab[i], 8, names[i], 24, BLACK);
    if (currentView == vtab[i]) {                       // underline = active tab
      int w = (int)strlen(names[i]) * 14;
      EPD_DrawLine(xtab[i], 34, xtab[i] + w, 34, BLACK);
      EPD_DrawLine(xtab[i], 35, xtab[i] + w, 35, BLACK);
    }
  }
  EPD_ShowString(650, 12, (wifiOk && tcpOk) ? "net:ok" : "net:--", 16, BLACK);
  EPD_DrawLine(8, 40, 783, 40, BLACK);
}

static void composeList() {
  Paint_NewImage(ImageBW, EPD_W, EPD_H, Rotation, WHITE);
  Paint_Clear(WHITE);
  drawTabs();

  int N = activeN();
  ViewState* vs = activeVS();
  if (N == 0) {
    const char* empty = currentView == VIEW_MESSAGES ? "(no messages yet)"
                      : currentView == VIEW_NODES    ? "(no nodes yet)" : "(no presets)";
    EPD_ShowString(8, LIST_Y, empty, 16, BLACK);
  } else {
    int y = LIST_Y;
    for (int r = 0; r < ROWS; r++) {
      int idx = vs->top + r;
      if (idx >= N) break;
      bool sel = (idx == vs->sel);
      char row[100];
      if (currentView == VIEW_MESSAGES) {
        const Msg* m = getFiltMsg(idx);
        if (!m) continue;
        snprintf(row, sizeof(row), "%c %s: %s", sel ? '>' : ' ', nodeName(m->from), m->text);
      } else if (currentView == VIEW_NODES) {
        const Node* nd = &nodes[idx];
        snprintf(row, sizeof(row), "%c %s (%s)  !%08lx", sel ? '>' : ' ',
                 nd->longName[0] ? nd->longName : "?", nd->shortName, (unsigned long)nd->num);
      } else {  // VIEW_SEND
        if (idx == sendConfirmIdx)
          snprintf(row, sizeof(row), "%c %s   <-- press again to SEND", sel ? '>' : ' ', CANNED[idx].label);
        else
          snprintf(row, sizeof(row), "%c %s", sel ? '>' : ' ', CANNED[idx].label);
      }
      EPD_ShowString(8, y, row, 16, BLACK);
      y += ROW_H;
    }
  }

  char foot[80];
  if (currentView == VIEW_SEND) {
    if (sentMsg[0] && millis() - sentAtMs < 8000)
      snprintf(foot, sizeof(foot), "Sent: %s  (ch: %s)", sentMsg, activeChanName());
    else
      snprintf(foot, sizeof(foot), "-> ch: %s   dial: pick   press: send 2x   MENU: switch", activeChanName());
  } else if (currentView == VIEW_MESSAGES) {
    snprintf(foot, sizeof(foot), "ch: %s   MENU: switch   dial: move   press: open", activeChanName());
  } else {
    snprintf(foot, sizeof(foot), "MENU: switch   dial: move   press: open   EXIT: back");
  }
  EPD_ShowString(8, 250, foot, 16, BLACK);
}

static void composeDetail() {
  Paint_NewImage(ImageBW, EPD_W, EPD_H, Rotation, WHITE);
  Paint_Clear(WHITE);
  char line[80];
  if (detIsMsg) {
    EPD_ShowString(8, 6, "MESSAGE", 24, BLACK);
    EPD_DrawLine(8, 38, 783, 38, BLACK);
    snprintf(line, sizeof(line), "From: %s   !%08lx   ch:%d",
             nodeName(detMsg.from), (unsigned long)detMsg.from, detMsg.ch);
    EPD_ShowString(8, 46, line, 16, BLACK);
    EPD_DrawLine(8, 70, 783, 70, BLACK);
    wrapText(detMsg.text, 8, 82, 24, 54);
    EPD_ShowString(8, 250, "press / EXIT: back to list", 16, BLACK);
  } else {
    EPD_ShowString(8, 6, "NODE", 24, BLACK);
    EPD_DrawLine(8, 38, 783, 38, BLACK);
    snprintf(line, sizeof(line), "Long:  %s", detNode.longName[0] ? detNode.longName : "?");
    EPD_ShowString(8, 54, line, 20, BLACK);
    snprintf(line, sizeof(line), "Short: %s", detNode.shortName);
    EPD_ShowString(8, 86, line, 20, BLACK);
    snprintf(line, sizeof(line), "ID:    !%08lx", (unsigned long)detNode.num);
    EPD_ShowString(8, 118, line, 20, BLACK);
    if (detNode.num == myNodeNum) {            // local node -> selectable channel list
      EPD_DrawLine(8, 150, 783, 150, BLACK);
      EPD_ShowString(8, 156, "Channels (dial + press to use):", 16, BLACK);
      uint8_t ci[8]; int cn = getActiveChans(ci);
      int cy = 182;
      for (int k = 0; k < cn; k++) {
        int i = ci[k];
        const char* role = chans[i].role == 1 ? "primary" : "secondary";
        const char* nm = chans[i].name[0] ? chans[i].name : (chans[i].role == 1 ? "LongFast" : "(unnamed)");
        snprintf(line, sizeof(line), "%c %d: %s (%s)", (k == chanSel) ? '>' : ' ', i, nm, role);
        EPD_ShowString(8, cy, line, 16, BLACK);
        cy += 22;
      }
      EPD_ShowString(8, 250, "dial: pick   press: use channel   EXIT: back", 16, BLACK);
    } else {
      EPD_ShowString(8, 250, "press / EXIT: back to list", 16, BLACK);
    }
  }
}

static const char* wxDesc(int c) {
  if (c == 0)                 return "Clear";
  if (c <= 2)                 return "Partly cloudy";
  if (c == 3)                 return "Overcast";
  if (c == 45 || c == 48)     return "Fog";
  if (c >= 51 && c <= 57)     return "Drizzle";
  if (c >= 61 && c <= 67)     return "Rain";
  if (c >= 71 && c <= 77)     return "Snow";
  if (c >= 80 && c <= 82)     return "Showers";
  if (c >= 85 && c <= 86)     return "Snow showers";
  if (c >= 95)                return "Thunderstorm";
  return "";
}

static void composeWeather() {
  Paint_NewImage(ImageBW, EPD_W, EPD_H, Rotation, WHITE);
  Paint_Clear(WHITE);
  drawTabs();
  EPD_DrawLine(396, 44, 396, 238, BLACK);   // split: indoor | outdoor

  EPD_ShowString(16, 50, "INDOOR (DHT11)", 20, BLACK);
  char line[48];
  if (dhtOk) {
    int tF = (int)(curTemp * 9.0f / 5.0f + 32.0f + 0.5f);
    int h  = (int)(curHum + 0.5f);
    snprintf(line, sizeof(line), "%d F",    tF); EPD_ShowString(16, 86,  line, 24, BLACK);
    snprintf(line, sizeof(line), "%d %% RH", h); EPD_ShowString(16, 124, line, 24, BLACK);
  } else {
    EPD_ShowString(16, 86,  "no reading", 20, BLACK);
    EPD_ShowString(16, 116, "check GPIO38 wiring", 16, BLACK);
  }

  EPD_ShowString(412, 50, "OUTDOOR", 20, BLACK);
  if (outOk) {
    int otF = (int)(outTemp + 0.5f);
    int oh  = (int)(outHum + 0.5f);
    snprintf(line, sizeof(line), "%d F",    otF); EPD_ShowString(412, 86,  line, 24, BLACK);
    snprintf(line, sizeof(line), "%d %% RH", oh); EPD_ShowString(412, 124, line, 24, BLACK);
    EPD_ShowString(412, 162, wxDesc(outCode), 16, BLACK);
  } else {
    EPD_ShowString(412, 86, outEver ? "fetch failed" : "fetching...", 16, BLACK);
  }

  EPD_ShowString(8, 250, "MENU: switch   press: refresh   EXIT: messages", 16, BLACK);
}

static void composeCurrent() {
  if (mode == MODE_DETAIL)              composeDetail();
  else if (currentView == VIEW_WEATHER) composeWeather();
  else                                  composeList();
}

static void redraw(bool wantClean) {
  // First draw ever: one true full refresh (the only black/white flash) to set a
  // clean baseline for every later partial update.
  if (!panelReady) {
    composeCurrent();
    epdFullRefresh();
    partialsSinceClean = 0;
    return;
  }
  bool clean = wantClean || partialsSinceClean >= CLEAN_EVERY;
  if (clean) {
    // Two-pass PARTIAL clear (FLASHLESS): pass 1 pushes an all-white frame to
    // drive lingering ghost toward white; pass 2 draws the real content.
    Paint_NewImage(ImageBW, EPD_W, EPD_H, Rotation, WHITE);
    Paint_Clear(WHITE);
    epdPartialPush();                                               // pass 1: white
    composeCurrent();
    epdPartialPush();                                               // pass 2: content
    partialsSinceClean = 0;
  } else {
    composeCurrent();   // fast single partial
    epdPartialPush();
    partialsSinceClean++;
  }
}
static void requestRedraw(bool full) { pendRedraw = true; if (full) pendFull = true; lastChange = millis(); }

static void bootStatus(const char* l1, const char* l2) {
  Paint_NewImage(ImageBW, EPD_W, EPD_H, Rotation, WHITE);
  Paint_Clear(WHITE);
  EPD_ShowString(8, 8, "Meshtastic client", 24, BLACK);
  EPD_DrawLine(8, 40, 783, 40, BLACK);
  if (l1) EPD_ShowString(8, 52, l1, 16, BLACK);
  if (l2) EPD_ShowString(8, 76, l2, 16, BLACK);
  epdFullRefresh();
}

// ---------- protocol ----------
static void sendWantConfig(uint32_t id) {
  uint8_t payload[8]; size_t n = 0;
  payload[n++] = 0x18;
  uint32_t v = id ? id : 1;
  do { uint8_t b = v & 0x7f; v >>= 7; if (v) b |= 0x80; payload[n++] = b; } while (v);
  uint8_t hdr[4] = { START1, START2, (uint8_t)((n >> 8) & 0xff), (uint8_t)(n & 0xff) };
  client.write(hdr, 4);
  client.write(payload, n);
}

// Send a canned text on the private channel. Builds a framed ToRadio; the NODE
// encrypts it with the channel's key, so the CrowPanel needs no crypto itself.
static void sendText(const char* text) {
  meshtastic_ToRadio tr = meshtastic_ToRadio_init_zero;
  tr.which_payload_variant = meshtastic_ToRadio_packet_tag;
  meshtastic_MeshPacket* mp = &tr.packet;
  mp->to = 0xFFFFFFFF;                  // broadcast on the channel
  mp->channel = activeChannel;          // the active (user-selected) channel
  mp->which_payload_variant = meshtastic_MeshPacket_decoded_tag;
  meshtastic_Data* d = &mp->decoded;
  d->portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
  size_t n = strlen(text);
  if (n > sizeof(d->payload.bytes)) n = sizeof(d->payload.bytes);
  memcpy(d->payload.bytes, text, n);
  d->payload.size = n;

  uint8_t buf[256];
  pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
  if (!pb_encode(&os, meshtastic_ToRadio_fields, &tr)) {
    Serial.printf("[send] encode FAIL: %s\n", PB_GET_ERROR(&os));
    return;
  }
  uint8_t fhdr[4] = { START1, START2,
                      (uint8_t)((os.bytes_written >> 8) & 0xff), (uint8_t)(os.bytes_written & 0xff) };
  client.write(fhdr, 4);
  client.write(buf, os.bytes_written);
  Serial.printf("[send] ch%u: \"%s\" (%u bytes)\n", (unsigned)activeChannel, text, (unsigned)os.bytes_written);
}

// Fetch current outdoor conditions from Open-Meteo (HTTPS, no API key).
static void fetchOutdoor() {
  WiFiClientSecure wc;
  wc.setInsecure();                 // skip cert validation (weather data; no secrets)
  HTTPClient https;
  char url[220];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
           "&current=temperature_2m,relative_humidity_2m,weather_code&temperature_unit=fahrenheit",
           WX_LAT, WX_LON);
  if (!https.begin(wc, url)) { outOk = false; Serial.println("[wx] begin failed"); return; }
  int code = https.GET();
  if (code == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, https.getString());
    if (!err) {
      JsonObject cur = doc["current"];
      outTemp = cur["temperature_2m"]       | 0.0f;
      outHum  = cur["relative_humidity_2m"] | 0.0f;
      outCode = cur["weather_code"]         | -1;
      outOk = true;
      Serial.printf("[wx] %dF %d%% code%d\n", (int)outTemp, (int)outHum, outCode);
    } else { outOk = false; Serial.printf("[wx] json err: %s\n", err.c_str()); }
  } else { outOk = false; Serial.printf("[wx] http %d\n", code); }
  https.end();
}

static void handleFrame(const uint8_t* buf, size_t len) {
  memset(&g_fr, 0, sizeof(g_fr));
  pb_istream_t st = pb_istream_from_buffer(buf, len);
  if (!pb_decode(&st, meshtastic_FromRadio_fields, &g_fr)) {
    Serial.printf("[decode] FAIL len=%u: %s\n", (unsigned)len, PB_GET_ERROR(&st));
    return;
  }

  switch (g_fr.which_payload_variant) {
    case meshtastic_FromRadio_my_info_tag:
      myNodeNum = g_fr.my_info.my_node_num;
      break;
    case meshtastic_FromRadio_channel_tag: {
      const meshtastic_Channel* ch = &g_fr.channel;
      if (ch->index < 8) {
        chans[ch->index].role = (uint8_t)ch->role;
        const char* nm = ch->has_settings ? ch->settings.name : "";
        strncpy(chans[ch->index].name, nm, sizeof(chans[ch->index].name) - 1);
        chans[ch->index].name[sizeof(chans[ch->index].name) - 1] = 0;
      }
      break;
    }
    case meshtastic_FromRadio_node_info_tag: {
      const meshtastic_NodeInfo* ni = &g_fr.node_info;
      upsertNode(ni->num, ni->has_user ? ni->user.long_name : "", ni->has_user ? ni->user.short_name : "");
      Serial.printf("[node] num=0x%08lx '%s' -> %d nodes\n", (unsigned long)ni->num,
                    ni->has_user ? ni->user.long_name : "?", nodeCount);
      break;
    }
    case meshtastic_FromRadio_packet_tag: {
      const meshtastic_MeshPacket* mp = &g_fr.packet;
      if (mp->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
          mp->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
        char t[160];
        size_t n = mp->decoded.payload.size; if (n > sizeof(t) - 1) n = sizeof(t) - 1;
        memcpy(t, mp->decoded.payload.bytes, n); t[n] = 0;
        pushMsg(mp->from, (uint8_t)mp->channel, t);
        Serial.printf("[TEXT] %s: %s\n", nodeName(mp->from), t);
        // Live update for the shared post: if someone's on the Messages list at the
        // top and not mid-navigation, auto-show the new message (flashless two-pass).
        if (mode == MODE_LIST && currentView == VIEW_MESSAGES &&
            (uint8_t)mp->channel == activeChannel &&
            vsMsg.sel == 0 && vsMsg.top == 0 && millis() - lastInputMs > 2500) {
          requestRedraw(true);
        }
      }
      break;
    }
    case meshtastic_FromRadio_config_complete_id_tag:
      Serial.printf("[config_complete] id=%lu  nodes=%d\n", (unsigned long)g_fr.config_complete_id, nodeCount);
      // Only repaint when the node list actually changed, so the periodic refresh
      // (CONFIG_REFRESH_MS) doesn't flash the panel every cycle when nothing's new.
      if (nodeCount != prevNodeCount) { prevNodeCount = nodeCount; requestRedraw(true); }
      break;
    default: break;
  }
}

// ---------- input ----------
static Ev pollInput() {
  Ev e = EV_NONE;
  uint32_t now = millis();
  for (int i = 0; i < 5; i++) {
    int cur = digitalRead(inPins[i]);
    if (cur != inPrev[i] && now - inT[i] > 25) {
      inT[i] = now;
      if (inPrev[i] == HIGH && cur == LOW) e = inEv[i];
      inPrev[i] = cur;
    }
  }
  return e;
}

static void handleInput(Ev e) {
  lastInputMs = millis();
  if (e == EV_MENU) {
    mode = MODE_LIST;
    sendConfirmIdx = -1;
    currentView = (currentView == VIEW_MESSAGES) ? VIEW_NODES
                : (currentView == VIEW_NODES)    ? VIEW_SEND
                : (currentView == VIEW_SEND)     ? VIEW_WEATHER : VIEW_MESSAGES;
    activeVS()->sel = 0; activeVS()->top = 0;
    requestRedraw(true);
  } else if (e == EV_EXIT) {
    if (mode == MODE_DETAIL) { mode = MODE_LIST; requestRedraw(true); }
    else if (currentView == VIEW_SEND && sendConfirmIdx >= 0) { sendConfirmIdx = -1; requestRedraw(true); }  // cancel pending send
    else { currentView = VIEW_MESSAGES; vsMsg.sel = 0; vsMsg.top = 0; sendConfirmIdx = -1; requestRedraw(true); }
  } else if (e == EV_OK) {
    if (mode == MODE_DETAIL) {
      if (!detIsMsg && detNode.num == myNodeNum) {       // pick a channel -> make it active, jump to Messages
        uint8_t ci[8]; int cn = getActiveChans(ci);
        if (cn > 0) { if (chanSel >= cn) chanSel = cn - 1; activeChannel = ci[chanSel]; }
        mode = MODE_LIST; currentView = VIEW_MESSAGES; vsMsg.sel = 0; vsMsg.top = 0;
      } else {
        mode = MODE_LIST;
      }
      requestRedraw(true);
    }
    else if (currentView == VIEW_SEND) {
      ViewState* vs = activeVS();
      if (CANNED_COUNT > 0) {
        if (sendConfirmIdx == vs->sel) {                 // 2nd press on the same item -> SEND
          sendText(CANNED[vs->sel].text);
          strncpy(sentMsg, CANNED[vs->sel].label, sizeof(sentMsg) - 1); sentMsg[sizeof(sentMsg) - 1] = 0;
          sentAtMs = millis();
          sendConfirmIdx = -1;
        } else {                                         // 1st press -> arm confirm
          sendConfirmIdx = vs->sel;
        }
        requestRedraw(true);
      }
    } else if (currentView == VIEW_WEATHER) {
      dhtEver = false; outEver = false; requestRedraw(true);   // force fresh sensor + weather
    } else {                                             // Messages / Nodes -> open detail
      int N = activeN();
      if (N > 0) {
        ViewState* vs = activeVS();
        if (currentView == VIEW_MESSAGES) {
          const Msg* m = getFiltMsg(vs->sel);
          if (m) { detMsg = *m; detIsMsg = true; mode = MODE_DETAIL; }
        } else {
          detIsMsg = false; detNode = nodes[vs->sel]; chanSel = 0; mode = MODE_DETAIL;
        }
      }
      requestRedraw(true);
    }
  } else if ((e == EV_UP || e == EV_DOWN) && mode == MODE_DETAIL && !detIsMsg && detNode.num == myNodeNum) {
    uint8_t ci[8]; int cn = getActiveChans(ci);
    if (cn > 0) {
      if (e == EV_DOWN) { if (chanSel < cn - 1) chanSel++; }
      else              { if (chanSel > 0)      chanSel--; }
      requestRedraw(false);   // channel cursor move = partial
    }
  } else if ((e == EV_UP || e == EV_DOWN) && mode == MODE_LIST) {
    int N = activeN();
    if (N == 0) return;
    sendConfirmIdx = -1;                                 // moving cancels a pending send-confirm
    ViewState* vs = activeVS();
    bool scrolled = false;
    if (e == EV_DOWN) { if (vs->sel < N - 1) { vs->sel++; if (vs->sel >= vs->top + ROWS) { vs->top++; scrolled = true; } } }
    else              { if (vs->sel > 0)     { vs->sel--; if (vs->sel <  vs->top)        { vs->top--; scrolled = true; } } }
    requestRedraw(scrolled);
  }
}

// ---------- framing ----------
enum FrameState { WAIT_S1, WAIT_S2, LEN_HI, LEN_LO, PAYLOAD, SKIP };
static FrameState fstate = WAIT_S1;
static uint16_t   flen = 0, fgot = 0;
static uint8_t    fbuf[MAX_FRAME];

static void pumpTcp() {
  while (client.available()) {
    uint8_t b = client.read();
    switch (fstate) {
      case WAIT_S1: if (b == START1) fstate = WAIT_S2; break;
      case WAIT_S2: fstate = (b == START2) ? LEN_HI : WAIT_S1; break;
      case LEN_HI:  flen = (uint16_t)b << 8; fstate = LEN_LO; break;
      case LEN_LO:
        flen |= b; fgot = 0;
        if (flen == 0)             fstate = WAIT_S1;
        else if (flen > MAX_FRAME) fstate = SKIP;     // too big for fbuf -> skip its bytes to stay in sync
        else                       fstate = PAYLOAD;
        break;
      case PAYLOAD:
        fbuf[fgot++] = b;
        if (fgot >= flen) { handleFrame(fbuf, flen); fstate = WAIT_S1; }
        break;
      case SKIP:
        if (++fgot >= flen) fstate = WAIT_S1;         // discard oversized frame payload
        break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[client] boot (step 6: UI, deliberate refresh)");

  for (int i = 0; i < 5; i++) { pinMode(inPins[i], INPUT); inPrev[i] = HIGH; inT[i] = 0; }
  dht.setup(38, DHTesp::DHT11);   // local temp/humidity sensor on GPIO38

  bootStatus("WiFi: connecting...", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(250); Serial.print("."); }
  Serial.println();
  wifiOk = (WiFi.status() == WL_CONNECTED);
  if (!wifiOk) { bootStatus("WiFi: FAILED", "check secrets.h (2.4 GHz?)"); return; }
  Serial.printf("[client] WiFi OK ip=%s\n", WiFi.localIP().toString().c_str());

  bootStatus("connecting to node...", MESH_NODE_IP);
  if (client.connect(MESH_NODE_IP, MESH_NODE_PORT)) {
    client.setNoDelay(true); tcpOk = true; sendWantConfig(1);
    Serial.println("[client] TCP connected; requested config");
  } else {
    Serial.println("[client] TCP connect FAILED");
  }
  requestRedraw(true);
}

void loop() {
  pumpTcp();

  Ev e = pollInput();
  if (e != EV_NONE) handleInput(e);

  uint32_t now = millis();

  // periodic local DHT11 read (blocks ~250ms; infrequent so it's fine)
  if (!dhtEver || now - dhtLastMs > 30000) {
    dhtLastMs = now; dhtEver = true;
    TempAndHumidity r = dht.getTempAndHumidity();
    if (dht.getStatus() == DHTesp::ERROR_NONE) { curTemp = r.temperature; curHum = r.humidity; dhtOk = true; }
    else dhtOk = false;
    if (currentView == VIEW_WEATHER && mode == MODE_LIST) requestRedraw(true);
  }

  // periodic outdoor weather fetch (HTTPS, ~1-3s; every 15 min, + on demand)
  if (wifiOk && (!outEver || now - outLastMs > 900000)) {
    outLastMs = now; outEver = true;
    fetchOutdoor();
    if (currentView == VIEW_WEATHER && mode == MODE_LIST) requestRedraw(true);
  }

  // periodic node-list / channel refresh on the live link (re-request config);
  // config_complete only repaints if the node count changed, so this is quiet.
  if (client.connected() && now - cfgLastMs > CONFIG_REFRESH_MS) {
    cfgLastMs = now;
    sendWantConfig(1);
  }

  if (pendRedraw && now - lastChange > COALESCE_MS) {
    pendRedraw = false;
    bool full = pendFull; pendFull = false;
    wifiOk = (WiFi.status() == WL_CONNECTED);
    tcpOk  = client.connected();
    redraw(full);
  }

  if (wifiOk && !client.connected()) {
    tcpOk = false;
    static uint32_t lastTry = 0;
    if (now - lastTry > 3000) {
      lastTry = now;
      if (client.connect(MESH_NODE_IP, MESH_NODE_PORT)) {
        client.setNoDelay(true); tcpOk = true; sendWantConfig(1);
        Serial.println("[client] reconnected");
        requestRedraw(true);
      }
    }
  }
}
