// summon.ino — M5StickS3 paging device v3
// BtnA        = summon / dismiss inbound alarm
// BtnB single = quiet countdown
// BtnB double = cycle volume (MUTE→LOW→MED→HIGH)
// BtnB hold at boot = factory-reset pairing

#include <M5Unified.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Preferences.h>

// ─── Tunables ───────────────────────────────────────────────────────────────
#define ESPNOW_CHANNEL      6
#define PEER_TIMEOUT_MS     3000
#define HEARTBEAT_MS        750
#define SUMMON_DURATION_MS  30000
#define ACK_COUNTDOWN_MS    20000
#define AWAIT_ACK_MS        30000
#define BEEP_INTERVAL_MS    650
#define COUNTDOWN_TICK_MS   1000
#define UNREACH_BEEP_MS     900
#define PAIR_BEACON_MS      400
#define PAIR_OK_SHOW_MS     2500
#define SLEEP_TIMEOUT_MS    5000
#define BAT_POLL_MS         5000
#define LOW_BAT_PCT         15
#define LOW_BAT_CHIRP_MS    60000
#define BRIGHT              100
#define REJECT_BEEP_HZ      400
#define DTAP_WINDOW_MS      400
#define VOL_TOAST_MS        2000

// ─── Colors (blue-on-black theme) ───────────────────────────────────────────
#define COL_BG       BLACK    // normal background
#define COL_PRI      0x5D9B   // sky blue — primary text
#define COL_SEC      0x2C77   // dimmer steel blue — status/hints
#define COL_WARN     0xFB00   // amber — warnings / no-signal
#define COL_OK       0x07E0   // green — confirmed/countdown number
#define COL_ALARM_BG RED
#define COL_ALARM_FG 0xFFE0   // yellow

enum Kind : uint8_t { K_HB=0, K_SUMMON=1, K_ACK=2, K_PAIR_HELLO=10 };

struct __attribute__((packed)) Msg { uint8_t kind; uint32_t seq; };

// ─── State ──────────────────────────────────────────────────────────────────
static uint8_t    bcast[6]    = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static uint8_t    peer_mac[6] = {0};
static bool       paired      = false;
static Preferences prefs;

static volatile uint32_t last_peer_ms       = 0;
static volatile uint32_t summon_until       = 0;
static volatile uint32_t ack_until          = 0;
static volatile uint32_t awaiting_ack_until = 0;
static volatile uint32_t pair_done_until    = 0;
static volatile int8_t   last_rssi          = -127;
static volatile bool     pair_event         = false;

static uint32_t my_seq            = 0;
static uint32_t last_activity_ms  = 0;
static uint32_t last_low_chirp_ms = 0;
static int      last_bat_pct      = -1;
static bool     last_charging     = false;
static bool     display_on        = true;

// volume
static const uint8_t VOL_LEVELS[] = {0, 64, 100, 200};
static const char*   VOL_NAMES[]  = {"MUTE", "LOW ", "MED ", "HIGH"};
static uint8_t       vol_idx      = 2;
static uint32_t      vol_toast_until = 0;

// BtnB double-tap
static uint32_t btnB_last_ms = 0;
static bool     btnB_pending = false;

// ─── Helpers ────────────────────────────────────────────────────────────────
static bool mac_eq(const uint8_t* a, const uint8_t* b) { return memcmp(a, b, 6) == 0; }
static bool is_zero(const uint8_t* m) {
  for (int i = 0; i < 6; ++i) { if (m[i]) return false; }
  return true;
}

static void load_pair() {
  prefs.begin("summon", true);
  if (prefs.getBytesLength("peer") == 6) { prefs.getBytes("peer", peer_mac, 6); }
  prefs.end();
  paired = !is_zero(peer_mac);
}
static void save_pair() {
  prefs.begin("summon", false); prefs.putBytes("peer", peer_mac, 6); prefs.end();
}
static void clear_pair() {
  prefs.begin("summon", false); prefs.remove("peer"); prefs.end();
  memset(peer_mac, 0, 6); paired = false;
}

static void add_peer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) { return; }
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = ESPNOW_CHANNEL;
  (void) esp_now_add_peer(&p);
}

static void send_to(const uint8_t* mac, uint8_t kind) {
  Msg m = { kind, ++my_seq };
  (void) esp_now_send(mac, (const uint8_t*)&m, sizeof(m));
}

static void wake() { if (!display_on) { M5.Display.setBrightness(BRIGHT); display_on = true; } }
static void doze() { if ( display_on) { M5.Display.setBrightness(0);      display_on = false; } }

// ─── ESP-NOW receive ────────────────────────────────────────────────────────
static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (info == nullptr || data == nullptr) { return; }
  if (len != (int)sizeof(Msg))            { return; }
  Msg m;
  memcpy(&m, data, sizeof(m));
  const uint8_t* src = info->src_addr;
  if (info->rx_ctrl) { last_rssi = info->rx_ctrl->rssi; }
  uint32_t now = millis();

  if (m.kind == K_PAIR_HELLO) {
    if (!paired) {
      memcpy(peer_mac, src, 6);
      paired = true;
      save_pair();
      add_peer(peer_mac);
      pair_event      = true;
      pair_done_until = now + PAIR_OK_SHOW_MS;
      last_activity_ms = now;
    }
    return;
  }

  if (!paired || !mac_eq(src, peer_mac)) { return; }
  last_peer_ms = now;

  if (m.kind == K_SUMMON) {
    summon_until     = now + SUMMON_DURATION_MS;
    last_activity_ms = now;
  } else if (m.kind == K_ACK) {
    if (now < awaiting_ack_until) {
      ack_until          = now + ACK_COUNTDOWN_MS;
      awaiting_ack_until = 0;
      last_activity_ms   = now;
    }
  }
}

// ─── Rendering (vertical 135×240) ───────────────────────────────────────────
static void draw_status_bar(uint32_t now) {
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(COL_SEC, COL_BG);
  M5.Display.setCursor(4, 4);
  if (last_bat_pct >= 0) { M5.Display.printf("%3d%%%c", last_bat_pct, last_charging ? '+' : ' '); }
  else                   { M5.Display.print("---  "); }
  M5.Display.setCursor(70, 4);
  bool peer_ok = (now - last_peer_ms) < PEER_TIMEOUT_MS;
  if (peer_ok) { M5.Display.printf("%4ddBm", last_rssi); }
  else         { M5.Display.setTextColor(COL_WARN, COL_BG); M5.Display.print("NOLINK"); }
  M5.Display.setTextColor(COL_SEC, COL_BG);
  M5.Display.setCursor(4, 16);
  M5.Display.printf("peer %02X:%02X", peer_mac[4], peer_mac[5]);
  M5.Display.setCursor(82, 16);
  uint32_t age = (now - last_peer_ms) / 1000;
  if (peer_ok && age < 99) { M5.Display.printf("%lus", age); }
  else                     { M5.Display.print("...."); }
}

static void draw_vol_toast() {
  M5.Display.fillRect(4, 200, 127, 34, 0x1082);
  M5.Display.drawRect(4, 200, 127, 34, COL_SEC);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COL_PRI, 0x1082);
  M5.Display.setCursor(12, 208);
  M5.Display.printf("VOL: %s", VOL_NAMES[vol_idx]);
}

static void draw_pairing_screen() {
  M5.Display.fillScreen(COL_BG);
  M5.Display.setTextColor(COL_PRI, COL_BG);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(8, 20);  M5.Display.print("PAIR");
  M5.Display.setCursor(8, 50);  M5.Display.print("ING..");
  M5.Display.setTextColor(COL_SEC, COL_BG);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(4, 110); M5.Display.print("Flash & power");
  M5.Display.setCursor(4, 122); M5.Display.print("the other unit");
  M5.Display.setCursor(4, 150); M5.Display.print("hold BtnB at");
  M5.Display.setCursor(4, 162); M5.Display.print("boot to re-pair");
}

static void draw_paired_screen() {
  M5.Display.fillScreen(COL_BG);
  M5.Display.setTextColor(COL_OK, COL_BG);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(8, 70);  M5.Display.print("PAIR");
  M5.Display.setCursor(8, 100); M5.Display.print("ED!");
  M5.Display.setTextColor(COL_SEC, COL_BG);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(4, 160);
  M5.Display.printf("peer %02X:%02X:%02X",
                    peer_mac[3], peer_mac[4], peer_mac[5]);
}

static void draw_charging_screen() {
  M5.Display.fillScreen(COL_BG);
  M5.Display.setTextColor(COL_SEC, COL_BG);
  const int x = 30, y = 60, w = 75, h = 110;
  M5.Display.drawRect(x, y, w, h, COL_SEC);
  M5.Display.fillRect(x + w/4, y - 6, w/2, 6, COL_SEC);
  int pct = last_bat_pct < 0 ? 0 : (last_bat_pct > 100 ? 100 : last_bat_pct);
  int fillH = (h - 6) * pct / 100;
  uint16_t col = (pct < 20) ? RED : (pct < 50) ? COL_WARN : COL_OK;
  M5.Display.fillRect(x + 3, y + h - 3 - fillH, w - 6, fillH, col);
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(COL_PRI, COL_BG);
  M5.Display.setCursor(20, 185);
  M5.Display.printf("%3d%%", pct);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(COL_SEC, COL_BG);
  M5.Display.setCursor(16, 220);
  M5.Display.print("CHARGING");
}

static void draw_main(bool summoning, bool counting, bool waiting,
                      bool peer_ok, uint32_t now) {
  M5.Display.fillScreen(summoning ? COL_ALARM_BG : COL_BG);
  if (!summoning) { draw_status_bar(now); }

  if (summoning) {
    M5.Display.setTextSize(4);
    M5.Display.setTextColor(COL_ALARM_FG, COL_ALARM_BG);
    M5.Display.setCursor(6, 70);   M5.Display.print("SUM-");
    M5.Display.setCursor(6, 115);  M5.Display.print("MON!");
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COL_ALARM_FG, COL_ALARM_BG);
    M5.Display.setCursor(4, 210);  M5.Display.print("A = dismiss");
  } else if (counting) {
    uint32_t remain = (ack_until - now + 999) / 1000;
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COL_PRI, COL_BG);
    M5.Display.setCursor(4, 60);   M5.Display.print("ON THE WAY");
    M5.Display.setTextSize(6);
    M5.Display.setTextColor(COL_OK, COL_BG);
    M5.Display.setCursor(remain < 10 ? 48 : 28, 110);
    M5.Display.printf("%lu", remain);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_SEC, COL_BG);
    M5.Display.setCursor(4, 218);  M5.Display.print("B = quiet");
  } else if (waiting) {
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(COL_PRI, COL_BG);
    M5.Display.setCursor(4, 90);   M5.Display.print("SUMMON-");
    M5.Display.setCursor(4, 112);  M5.Display.print("ING...");
    if (!peer_ok) {
      M5.Display.setTextSize(2);
      M5.Display.setTextColor(COL_WARN, COL_BG);
      M5.Display.setCursor(4, 155); M5.Display.print("NO SIG-");
      M5.Display.setCursor(4, 177); M5.Display.print("NAL!");
    }
  } else {
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(peer_ok ? COL_PRI : COL_WARN, COL_BG);
    M5.Display.setCursor(4, 105);
    M5.Display.print(peer_ok ? "A = summon" : "NO LINK");
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(COL_SEC, COL_BG);
    M5.Display.setCursor(4, 218);  M5.Display.print("dbl-B: volume");
  }
}

// ─── Setup ──────────────────────────────────────────────────────────────────
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(0);
  M5.Display.setBrightness(BRIGHT);
  M5.Speaker.begin();
  M5.Speaker.setVolume(VOL_LEVELS[vol_idx]);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    M5.Display.fillScreen(COL_BG);
    M5.Display.setCursor(8, 100);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(RED);
    M5.Display.print("ENOW FAIL");
    while (1) { delay(1000); }
  }
  esp_now_register_recv_cb(on_recv);
  add_peer(bcast);

  // hold BtnB during first 300ms after boot → factory-reset pairing
  uint32_t end = millis() + 300;
  bool held = false;
  while (millis() < end) {
    M5.update();
    if (M5.BtnB.isPressed()) { held = true; }
    delay(20);
  }
  if (held) {
    clear_pair();
    M5.Display.fillScreen(COL_BG);
    M5.Display.setTextColor(COL_WARN, COL_BG);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(8, 100); M5.Display.print("PAIRING");
    M5.Display.setCursor(16, 124); M5.Display.print("RESET");
    delay(1200);
  } else {
    load_pair();
    if (paired) { add_peer(peer_mac); }
  }
  last_activity_ms = millis();
}

// ─── Loop ───────────────────────────────────────────────────────────────────
void loop() {
  M5.update();
  uint32_t now = millis();

  // PAIRING
  if (!paired) {
    wake();
    static uint32_t last_beacon = 0, last_draw = 0;
    if (now - last_beacon >= PAIR_BEACON_MS) { send_to(bcast, K_PAIR_HELLO); last_beacon = now; }
    if (pair_event || now - last_draw >= 300) { last_draw = now; pair_event = false; draw_pairing_screen(); }
    return;
  }

  // PAIR-OK CONFIRMATION
  if (now < pair_done_until) {
    wake();
    static uint32_t last_echo = 0, last_draw = 0;
    if (now - last_echo >= 150) { send_to(peer_mac, K_PAIR_HELLO); last_echo = now; }
    if (now - last_draw >= 200) { last_draw = now; draw_paired_screen(); }
    return;
  }

  // heartbeat
  static uint32_t last_beat = 0;
  if (now - last_beat >= HEARTBEAT_MS) { send_to(peer_mac, K_HB); last_beat = now; }

  // battery poll
  static uint32_t last_bat_poll = 0;
  if (now - last_bat_poll >= BAT_POLL_MS) {
    last_bat_poll = now;
    last_bat_pct  = M5.Power.getBatteryLevel();
    last_charging = (M5.Power.isCharging() > 0);
  }

  // BtnA
  if (M5.BtnA.wasPressed()) {
    wake(); last_activity_ms = now;
    if (now < summon_until) {
      summon_until = 0;
      send_to(peer_mac, K_ACK);
      M5.Speaker.tone(1500, 80);
    } else if (now < awaiting_ack_until || now < ack_until) {
      M5.Speaker.tone(REJECT_BEEP_HZ, 50);   // anti-stack
    } else {
      send_to(peer_mac, K_SUMMON);
      awaiting_ack_until = now + AWAIT_ACK_MS;
      M5.Speaker.tone(1000, 80);
    }
  }

  // BtnB — double-tap detection
  if (M5.BtnB.wasPressed()) {
    if (btnB_pending && (now - btnB_last_ms < DTAP_WINDOW_MS)) {
      btnB_pending = false;
      vol_idx = (vol_idx + 1) % 4;
      M5.Speaker.setVolume(VOL_LEVELS[vol_idx]);
      if (VOL_LEVELS[vol_idx] > 0) { M5.Speaker.tone(1200, 60); }
      vol_toast_until = now + VOL_TOAST_MS;
      wake(); last_activity_ms = now;
    } else {
      btnB_pending  = true;
      btnB_last_ms  = now;
    }
  }
  // BtnB single-tap fires after window
  if (btnB_pending && (now - btnB_last_ms >= DTAP_WINDOW_MS)) {
    btnB_pending = false;
    wake(); last_activity_ms = now;
    if (now < ack_until) { ack_until = 0; M5.Speaker.tone(800, 60); }
  }

  // alarm beep while summoned
  static uint32_t last_beep = 0;
  if (now < summon_until && now - last_beep >= BEEP_INTERVAL_MS) {
    M5.Speaker.tone(2400, 220);
    last_beep = now; wake(); last_activity_ms = now;
  }

  // countdown tick
  static uint32_t last_tick = 0;
  if (now < ack_until && now - last_tick >= COUNTDOWN_TICK_MS) {
    M5.Speaker.tone(1800, 40); last_tick = now;
  }

  // unreachable self-beep
  bool peer_ok = (now - last_peer_ms) < PEER_TIMEOUT_MS;
  static uint32_t last_unreach = 0;
  if ((now < awaiting_ack_until) && !peer_ok && (now - last_unreach >= UNREACH_BEEP_MS)) {
    M5.Speaker.tone(2000, 90);
    last_unreach = now; wake(); last_activity_ms = now;
  }

  // low battery chirp
  if (!last_charging && last_bat_pct > 0 && last_bat_pct < LOW_BAT_PCT &&
      (now - last_low_chirp_ms >= LOW_BAT_CHIRP_MS)) {
    M5.Speaker.tone(500, 150);
    last_low_chirp_ms = now;
  }

  // sleep policy
  bool summoning = now < summon_until;
  bool counting  = now < ack_until;
  bool waiting   = now < awaiting_ack_until;
  bool keep_on   = summoning || counting || waiting;
  if (!keep_on && !last_charging && (now - last_activity_ms) >= SLEEP_TIMEOUT_MS) { doze(); }
  else                                                                            { wake(); }

  if (!display_on) { return; }

  static uint32_t last_draw = 0;
  if (now - last_draw < 150) { return; }
  last_draw = now;

  if (!keep_on && last_charging) { draw_charging_screen(); }
  else                           { draw_main(summoning, counting, waiting, peer_ok, now); }

  if (now < vol_toast_until) { draw_vol_toast(); }
}
