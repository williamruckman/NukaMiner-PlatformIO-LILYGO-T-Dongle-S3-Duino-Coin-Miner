#include <Arduino.h>
#include <esp_system.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD_MMC.h>
#include <Update.h>

#include <vector>
#include <algorithm>

// ---- Duino-Coin worker grouping (shared group-id for dashboard thread aggregation)
static String getOrCreateDucoGroupId();
static String regenerateDucoGroupId();

// RGB LED on T-Dongle-S3 is an APA102/DotStar (data+clock), *not* a NeoPixel.
// Common pinout: DATA=GPIO40, CLK=GPIO39. (Backlight uses GPIO38.)
#include <Adafruit_DotStar.h>
#include <TFT_eSPI.h>
#include "display_assets.h"
#include "tdongle_png.h"
#include "web_assets.h"  // Frozen web UI JS assets (DO NOT inline-edit in main.cpp)
#include <MiningJob.h>
#include <Settings.h>

// -----------------------------
// NukaMiner (T-Dongle-S3)
// -----------------------------

// =======================
// Firmware identity (v1.0)
// =======================
// Change FW_VERSION to publish a new release.
static const char* FW_NAME    = "NukaMiner";
static const char* FW_VERSION = "1.0.0";
static const char* FW_CHANNEL = "stable";
static const char* FW_BUILD   = __DATE__ " " __TIME__;

// Reset reason captured at boot (for Status page)
static esp_reset_reason_t g_resetReason = ESP_RST_UNKNOWN;

// Hardware
static constexpr int PIN_BUTTON = 0;

static constexpr int PIN_DOTSTAR_DATA = 40;
static constexpr int PIN_DOTSTAR_CLK  = 39;
static constexpr uint8_t RGB_LED_COUNT = 1;
static Adafruit_DotStar rgb(RGB_LED_COUNT, PIN_DOTSTAR_DATA, PIN_DOTSTAR_CLK, DOTSTAR_BGR);

enum LedMode : uint8_t { LED_OFF=0, LED_YELLOW, LED_GREEN, LED_BLUE, LED_RED, LED_PURPLE };

// Forward declarations used by LED service (definitions appear later)
extern bool portalRunning;       // captive portal/AP mode flag
static bool minerIsRunning();    // mining task running?
static LedMode ledMode = LED_OFF;
static LedMode ledModeLast = LED_OFF;
static uint8_t ledBrightnessLast = 255;

// Locate mode (purple blink) + Device Control mode
static volatile bool locateMode = false;
static volatile bool deviceControlMode = false;

static void ledInit();
static void ledSetMode(LedMode m);
static void ledService();

// Backlight pins as used in the reference project (some boards strap BL differently).
// T-Dongle-S3 reference setup: BL = 38.
// Some builds also require pulling pin 37 low (observed in TzCoinMiner).
static constexpr int PIN_BL_38 = 38;
static constexpr int PIN_BL_37 = 37;

// On-board TF / uSD (hidden in the USB-A connector) wiring for LilyGo T-Dongle-S3.
// Uses SDMMC (not SPI). Pin mapping per community/board support references.
static constexpr int PIN_SD_CLK = 12;
static constexpr int PIN_SD_CMD = 16;
static constexpr int PIN_SD_D0  = 14;
static constexpr int PIN_SD_D1  = 17;
static constexpr int PIN_SD_D2  = 21;
static constexpr int PIN_SD_D3  = 18;

// Captive portal
static constexpr int DNS_PORT = 53;

// Display
static TFT_eSPI tft;
static constexpr int WIDTH  = 160; // landscape
static constexpr int HEIGHT = 80;

// Framebuffer (RGB565) - double-buffered to prevent tearing in web preview
static uint16_t *fb = nullptr;              // alias: draw buffer (back buffer)
static uint16_t *fbFront = nullptr;         // served to web (front buffer)
static uint16_t *fbBack  = nullptr;         // written by render code
static portMUX_TYPE fbMux = portMUX_INITIALIZER_UNLOCKED;
// Framebuffer generation counter used for cheap ETag change detection.
// Incremented each time we commit a new frame (swap front/back) after pushing to the physical LCD.
static volatile uint32_t fbGen = 0;

// Web LCD polling can be disabled from the status page to improve hashrate.
static volatile bool webLcdPollingEnabled = true;


static inline int idx(int x, int y) { return y * WIDTH + x; }

// -----------------------------
// Config
// -----------------------------
struct AppConfig {
  String wifi_ssid;
  String wifi_pass;
  String duco_user;
  String rig_id;
  String miner_key;
  String ntp_server = "pool.ntp.org";
  // Timezone name for web UI display (device keeps UTC internally)
  String tz_name = "UTC";
  uint32_t display_sleep_s = 30; // 0 = never
  uint8_t lcd_brightness = 50;   // 0-100

  // LCD rotation
  bool lcd_rot180 = false;      // flip 180deg (landscape)

  uint8_t hash_limit_pct = 100; // Shown as 100%

  // Allow the first mining core to be disabled independently.
  // Default OFF: Core 2 is the recommended/default miner task.
  bool core1_enabled = false;

  // Optional second miner task ("Core 2")
  // Default ON: recommended miner task.
  bool core2_enabled = true;
  uint8_t core2_hash_limit_pct = 100; // Shown as 100%

  uint8_t primary_core = 2;

  // Built-in RGB LED
  bool led_enabled = true;
  uint8_t led_brightness = 50; // 0-100

  // Auto-cycle LCD pages when on STA (not in AP/portal)
  bool carousel_enabled = true;
  uint16_t carousel_seconds = 10;
  bool duino_enabled = true;

  // Web UI (basic auth)
  bool web_enabled = true;
  bool web_always_on = true; // keep web UI available on STA even after setup
  // If web_always_on is false, the Web UI is only enabled for a limited time
  // after a physical BOOT press (idle timeout).
  uint16_t web_timeout_s = 300;
  String web_user = "admin";
  String web_pass = "nukaminer";

  // Pool lookup cache (seconds). 0 = disable caching.
  uint32_t pool_cache_s = 900;

  // Scheduled reboot
  // reboot_mode: 0=Off, 1=Daily, 2=Weekly, 3=Monthly
  uint8_t reboot_mode = 0;
  uint8_t reboot_hour = 3;    // 0-23 (UTC unless TZ set)
  uint8_t reboot_min  = 0;    // 0-59
  uint8_t reboot_wday = 0;    // 0=Sun..6=Sat (for weekly)
  uint8_t reboot_mday = 1;    // 1-31 (for monthly)
};

// -----------------------------
// WiFi Profiles
// -----------------------------
// Stored as JSON array under NVS key "wifi_profiles".
// Each profile: {"ssid":"...","pass":"...","prio":100}
struct WifiProfile {
  String ssid;
  String pass;
  int16_t prio = 100; // higher = preferred
};

static std::vector<WifiProfile> wifiProfiles;
static String wifiLastSsid; // last successfully connected SSID (hint)

static void wifiProfilesLoad();
static void wifiProfilesSave();
static void wifiProfilesMigrateLegacy();
static bool wifiHasAnyConfig();
static void wifiProfilesUpsert(const String& ssid, const String& pass, int16_t prio, bool keepExistingPrioIfPresent);
static bool wifiProfilesDelete(const String& ssid);
static void wifiProfilesSort();
static WifiProfile* wifiBestProfileForVisibleNetworks(const std::vector<String>& ssids, const std::vector<int>& rssis);

static Preferences prefs;
static AppConfig cfg;

// True while the SD file manager is actively uploading/downloading/deleting.
// Used to temporarily pause mining and keep the web server responsive.
static volatile bool sdBusy = false;

// Restore upload (no-SD) state
static String g_restoreUploadMsg;
static String g_restoreUploadErr;
static bool   g_restoreUploadOk = false;

// WiFi resilience
static uint8_t wifiReconnectFails = 0;
static uint32_t lastWifiCheckMs = 0;
static uint32_t lastWifiAttemptMs = 0;

// Web UI session gating (used when web_always_on == false)
static volatile bool webSessionActive = false;
static volatile uint32_t webSessionDeadlineMs = 0;

static inline void webSessionEnable(uint32_t nowMs) {
  webSessionActive = true;
  const uint32_t to = (uint32_t)cfg.web_timeout_s * 1000UL;
  webSessionDeadlineMs = nowMs + (to ? to : 300000UL);
}

static inline void webSessionTouch(uint32_t nowMs) {
  if (cfg.web_always_on) return;
  if (!webSessionActive) return;
  const uint32_t to = (uint32_t)cfg.web_timeout_s * 1000UL;
  webSessionDeadlineMs = nowMs + (to ? to : 300000UL);
}

static inline bool webSessionAllowed(uint32_t nowMs) {
  if (portalRunning) return true;
  if (!cfg.web_enabled) return false;
  if (cfg.web_always_on) return true;
  if (!webSessionActive) return false;
  if ((int32_t)(nowMs - webSessionDeadlineMs) >= 0) { webSessionActive = false; return false; }
  return true;
}

// -----------------------------
// High-priority service task (CPU0)
// -----------------------------
// Keep BOOT, captive portal and WebServer responsive even when miners are
// running at high duty cycle. WebServer is synchronous; it must be pumped
// frequently. Pin to CPU0 so CPU1 can focus on mining + display.
static TaskHandle_t serviceTask = nullptr;
static void serviceTaskFn(void *arg);

// -----------------------------
// Duino miner task handles (needed for suspend/resume during AP/Portal)
// -----------------------------
static TaskHandle_t minerTask0 = nullptr;
static TaskHandle_t minerTask1 = nullptr;
static volatile bool minerSuspendedForPortal = false;
static void minerSuspendForPortal();
static void minerResumeAfterPortal();


// -----------------------------
// Log ring buffer (for Web UI live console)
// -----------------------------
static constexpr size_t LOG_LINES_MAX = 220;
static String logLines[LOG_LINES_MAX];
static size_t logHead = 0;
static size_t logCount = 0;
static uint32_t logSeq = 0;

static void pushLogLine(const String &line) {
  logLines[logHead] = line;
  logHead = (logHead + 1) % LOG_LINES_MAX;
  if (logCount < LOG_LINES_MAX) logCount++;
  logSeq++;
}

// Declared in lib/NukaDuino/src/Settings.h
void NM_log(const String &line) {
  Serial.println(line);
  pushLogLine(line);
}

// Stored under NVS namespace "nukaminer"

static void loadConfig() {
  Preferences prefs;
  // Use RW mode so namespace exists; avoids noisy NOT_FOUND logs on first boot.
  prefs.begin("nukaminer", false);

  auto getStr = [&](const char* key, const char* def)->String {
    if (!prefs.isKey(key)) return String(def);
    return prefs.getString(key, def);
  };
  auto getBool = [&](const char* key, bool def)->bool {
    if (!prefs.isKey(key)) return def;
    return prefs.getBool(key, def);
  };
  auto getUInt = [&](const char* key, uint32_t def)->uint32_t {
    if (!prefs.isKey(key)) return def;
    return prefs.getUInt(key, def);
  };

  cfg.wifi_ssid = getStr("wifi_ssid", "");
  cfg.wifi_pass = getStr("wifi_pass", "");
  cfg.duco_user = getStr("duco_user", "");
  cfg.rig_id    = getStr("rig_id", "NukaMiner");
  cfg.miner_key = getStr("miner_key", "");
  cfg.ntp_server = getStr("ntp_server", "pool.ntp.org");
  cfg.tz_name = getStr("tz", "UTC");
  cfg.pool_cache_s = getUInt("pool_cache_s", 900);
  cfg.reboot_mode = (uint8_t)getUInt("rb_mode", 0);
  cfg.reboot_hour = (uint8_t)getUInt("rb_h", 3);
  cfg.reboot_min  = (uint8_t)getUInt("rb_m", 0);
  cfg.reboot_wday = (uint8_t)getUInt("rb_wd", 0);
  cfg.reboot_mday = (uint8_t)getUInt("rb_md", 1);

  if (cfg.pool_cache_s > 86400) cfg.pool_cache_s = 86400;
  if (cfg.reboot_mode > 3) cfg.reboot_mode = 0;
  if (cfg.reboot_hour > 23) cfg.reboot_hour = 3;
  if (cfg.reboot_min > 59) cfg.reboot_min = 0;
  if (cfg.reboot_wday > 6) cfg.reboot_wday = 0;
  if (cfg.reboot_mday < 1) cfg.reboot_mday = 1;
  if (cfg.reboot_mday > 31) cfg.reboot_mday = 31;

  cfg.display_sleep_s = (int)getUInt("disp_sleep", 30);
  cfg.lcd_brightness = (uint8_t)getUInt("lcd_br", 50);
  cfg.lcd_rot180 = getBool("lcd_r180", false);
  // Mining speed is system-managed. We keep the stored keys for backwards
  // compatibility but ignore them (always show 100%).
  cfg.hash_limit_pct = 100;
  cfg.core1_enabled = getBool("c1_en", false);

  cfg.core2_enabled = getBool("c2_en", true);
  cfg.core2_hash_limit_pct = 100;

  cfg.led_enabled = getBool("led_en", true);
  cfg.led_brightness = (uint8_t)getUInt("led_br", 50);

  cfg.carousel_enabled = getBool("car_en", true);
  cfg.carousel_seconds = (uint16_t)getUInt("car_s", 10);
  cfg.duino_enabled   = getBool("duco_en", true);

  cfg.web_enabled = getBool("web_en", true);
  cfg.web_always_on = getBool("web_always", true);
  cfg.web_timeout_s = (uint16_t)getUInt("web_to", 300);
  // Primary miner selection removed from UI; Core 2 is always treated as the primary.
  cfg.primary_core = 2;
  cfg.web_user    = getStr("web_user", "admin");
  cfg.web_pass    = getStr("web_pass", "nukaminer");

  prefs.end();
}


static void saveConfig() {
  prefs.begin("nukaminer", false);
  prefs.putString("wifi_ssid", cfg.wifi_ssid);
  prefs.putString("wifi_pass", cfg.wifi_pass);
  prefs.putString("duco_user", cfg.duco_user);
  prefs.putString("rig_id", cfg.rig_id);
  prefs.putString("miner_key", cfg.miner_key);
  prefs.putString("ntp_server", cfg.ntp_server);
  prefs.putString("tz", cfg.tz_name);
  prefs.putUInt("pool_cache_s", cfg.pool_cache_s);
  prefs.putUInt("rb_mode", cfg.reboot_mode);
  prefs.putUInt("rb_h", cfg.reboot_hour);
  prefs.putUInt("rb_m", cfg.reboot_min);
  prefs.putUInt("rb_wd", cfg.reboot_wday);
  prefs.putUInt("rb_md", cfg.reboot_mday);
  prefs.putUInt("disp_sleep", cfg.display_sleep_s);
  prefs.putUInt("lcd_br", cfg.lcd_brightness);
  prefs.putBool("lcd_r180", cfg.lcd_rot180);
  prefs.putUInt("hash_lim", cfg.hash_limit_pct);
  prefs.putBool("c1_en", cfg.core1_enabled);

  prefs.putBool("c2_en", cfg.core2_enabled);
  prefs.putUInt("c2_lim", cfg.core2_hash_limit_pct);
  prefs.putBool("led_en", cfg.led_enabled);
  prefs.putUInt("led_br", cfg.led_brightness);

  prefs.putBool("car_en", cfg.carousel_enabled);
  prefs.putUInt("car_s", cfg.carousel_seconds);
  prefs.putBool("duco_en", cfg.duino_enabled);
  prefs.putBool("web_en", cfg.web_enabled);
  prefs.putBool("web_always", cfg.web_always_on);
  prefs.putUInt("web_to", cfg.web_timeout_s);
  // prim_core intentionally not stored; Core 2 is the primary by design.
  prefs.putString("web_user", cfg.web_user);
  prefs.putString("web_pass", cfg.web_pass);
  prefs.end();
}

// -----------------------------
// WiFi Profiles (NVS JSON)
// -----------------------------
static void wifiProfilesSort() {
  std::sort(wifiProfiles.begin(), wifiProfiles.end(), [](const WifiProfile& a, const WifiProfile& b){
    if (a.prio != b.prio) return a.prio > b.prio;
    return a.ssid < b.ssid;
  });
}

static String wifiProfilesToJson() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& p : wifiProfiles) {
    JsonObject o = arr.createNestedObject();
    o["ssid"] = p.ssid;
    o["pass"] = p.pass;
    o["prio"] = p.prio;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

static void wifiProfilesLoad() {
  wifiProfiles.clear();
  wifiLastSsid = "";

  Preferences p; p.begin("nukaminer", false);
  wifiLastSsid = p.getString("wifi_last", "");
  String raw = p.getString("wifi_profiles", "");
  p.end();

  if (raw.length() == 0) {
    wifiProfilesMigrateLegacy();
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok || !doc.is<JsonArray>()) {
    wifiProfilesMigrateLegacy();
    return;
  }
  for (JsonObject o : doc.as<JsonArray>()) {
    WifiProfile wp;
    wp.ssid = String((const char*)(o["ssid"] | ""));
    wp.pass = String((const char*)(o["pass"] | ""));
    wp.prio = (int16_t)(o["prio"] | 100);
    wp.prio = (int16_t)constrain((int)wp.prio, -999, 999);
    if (wp.ssid.length() == 0) continue;
    wifiProfiles.push_back(wp);
    if (wifiProfiles.size() >= 12) break;
  }
  wifiProfilesSort();
}

static void wifiProfilesSave() {
  Preferences p; p.begin("nukaminer", false);
  p.putString("wifi_profiles", wifiProfilesToJson());
  if (wifiLastSsid.length()) p.putString("wifi_last", wifiLastSsid);
  p.end();
}

static void wifiProfilesMigrateLegacy() {
  // If older single-SSID config exists, turn it into a single profile.
  if (cfg.wifi_ssid.length() == 0) {
    wifiProfiles.clear();
    wifiProfilesSave();
    return;
  }
  wifiProfiles.clear();
  WifiProfile wp;
  wp.ssid = cfg.wifi_ssid;
  wp.pass = cfg.wifi_pass;
  wp.prio = 100;
  wifiProfiles.push_back(wp);
  wifiProfilesSave();
}

static bool wifiHasAnyConfig() {
  if (!wifiProfiles.empty()) return true;
  return cfg.wifi_ssid.length() > 0;
}

static void wifiProfilesUpsert(const String& ssid, const String& pass, int16_t prio, bool keepExistingPrioIfPresent) {
  if (ssid.length() == 0) return;
  for (auto &p : wifiProfiles) {
    if (p.ssid == ssid) {
      p.pass = pass;
      if (!keepExistingPrioIfPresent) p.prio = (int16_t)constrain((int)prio, -999, 999);
      wifiProfilesSort();
      wifiProfilesSave();
      return;
    }
  }
  WifiProfile wp;
  wp.ssid = ssid;
  wp.pass = pass;
  wp.prio = (int16_t)constrain((int)prio, -999, 999);
  wifiProfiles.push_back(wp);
  wifiProfilesSort();
  // Keep list bounded
  if (wifiProfiles.size() > 12) wifiProfiles.resize(12);
  wifiProfilesSave();
}

static bool wifiProfilesDelete(const String& ssid) {
  for (size_t i=0;i<wifiProfiles.size();i++) {
    if (wifiProfiles[i].ssid == ssid) {
      wifiProfiles.erase(wifiProfiles.begin()+i);
      wifiProfilesSave();
      return true;
    }
  }
  return false;
}

static WifiProfile* wifiProfileBySsid(const String& ssid) {
  for (auto &p : wifiProfiles) if (p.ssid == ssid) return &p;
  return nullptr;
}

static WifiProfile* wifiBestProfileForVisibleNetworks(const std::vector<String>& ssids, const std::vector<int>& rssis) {
  WifiProfile* best = nullptr;
  int bestPrio = -32768;
  int bestRssi = -9999;
  for (size_t i=0;i<ssids.size();i++) {
    WifiProfile* p = wifiProfileBySsid(ssids[i]);
    if (!p) continue;
    int pr = (int)p->prio;
    int rs = (i < rssis.size()) ? rssis[i] : -9999;
    if (!best || pr > bestPrio || (pr == bestPrio && rs > bestRssi)) {
      best = p;
      bestPrio = pr;
      bestRssi = rs;
    }
  }
  return best;
}

// -----------------------------
// SD backup / restore
// -----------------------------
static bool sdMounted = false;

// T-Dongle-S3 uSD (TF) is wired to the ESP32-S3 SDMMC peripheral (4-bit), not SPI.
// Pins from the vendor BSP documentation:
//   CLK=GPIO12, CMD=GPIO16, D0=GPIO14, D1=GPIO17, D2=GPIO21, D3=GPIO18
static constexpr int PIN_SDMMC_CLK = 12;
static constexpr int PIN_SDMMC_CMD = 16;
static constexpr int PIN_SDMMC_D0  = 14;
static constexpr int PIN_SDMMC_D1  = 17;
static constexpr int PIN_SDMMC_D2  = 21;
static constexpr int PIN_SDMMC_D3  = 18;

static bool sdBegin() {
  if (sdMounted) return true;

  // Try 4-bit first.
  SD_MMC.setPins(PIN_SDMMC_CLK, PIN_SDMMC_CMD, PIN_SDMMC_D0,
                 PIN_SDMMC_D1, PIN_SDMMC_D2, PIN_SDMMC_D3);
  if (SD_MMC.begin("/sdcard", /*mode1bit=*/false, /*format_if_failed=*/false, /*max_files=*/4)) {
    sdMounted = true;
    return true;
  }

  // Fallback: 1-bit using D0 only (some boards/slots are wired this way).
  SD_MMC.end();
  SD_MMC.setPins(PIN_SDMMC_CLK, PIN_SDMMC_CMD, PIN_SDMMC_D0, -1, -1, -1);
  if (SD_MMC.begin("/sdcard", /*mode1bit=*/true, /*format_if_failed=*/false, /*max_files=*/4)) {
    sdMounted = true;
    return true;
  }

  return false;
}

// Forward declarations for the SD backup browser helpers.
// These are implemented further below.
static bool ensureBackupDir();
static String makeBackupName();
static String backupPathFor(const String& name);
static bool sdBackupConfigToFile(const String& fullPath);
static bool sdRestoreConfigFromFile(const String& fullPath);
static void timeSyncOnce();

static bool sdBackupConfig() {
  // Compatibility: create a dated backup in /backups and also overwrite /nukaminer.json
  if (!ensureBackupDir()) return false;
  String name = makeBackupName();
  if (!sdBackupConfigToFile(backupPathFor(name))) return false;
  // Also keep a stable filename for older workflows
  return sdBackupConfigToFile("/nukaminer.json");
}

static bool sdRestoreConfig() {
  if (!sdBegin()) return false;
  if (!SD_MMC.exists("/nukaminer.json")) return false;

  const String old_ntp_server = cfg.ntp_server;
  if (!sdRestoreConfigFromFile("/nukaminer.json")) return false;

  if (cfg.ntp_server != old_ntp_server && WiFi.isConnected()) {
    NM_log(String("[NukaMiner] NTP server changed -> resync: ") + cfg.ntp_server);
    timeSyncOnce();
  }
  // Successful request counts as activity for the idle timeout.
  webSessionTouch(millis());
  return true;
}



// -----------------------------
// SD backup browser (multiple backups)
// -----------------------------
#include <time.h>

static const char* BACKUP_DIR = "/backups";
static bool timeInited = false;

static void timeSyncOnce() {
  if (timeInited) return;
  if (!WiFi.isConnected()) return;
  NM_log(String("[NukaMiner] NTP sync using ") + cfg.ntp_server);
  configTime(0, 0, cfg.ntp_server.c_str(), "time.nist.gov", "time.google.com");
  struct tm t; 
  if (getLocalTime(&t, 2000)) {
    timeInited = true;
  }
}

static void scheduledRebootCheck() {
  if (cfg.reboot_mode == 0) return;

  // Make sure time is available (non-blocking if already synced)
  timeSyncOnce();
  if (!timeInited) return;

  // Only check once per second
  static uint32_t lastCheckMs = 0;
  uint32_t nowMs = millis();
  if ((uint32_t)(nowMs - lastCheckMs) < 1000) return;
  lastCheckMs = nowMs;

  struct tm t;
  if (!getLocalTime(&t, 10)) return;

  if ((uint8_t)t.tm_hour != cfg.reboot_hour) return;
  if ((uint8_t)t.tm_min  != cfg.reboot_min)  return;

  bool due = false;
  if (cfg.reboot_mode == 1) {
    due = true; // Daily
  } else if (cfg.reboot_mode == 2) {
    due = ((uint8_t)t.tm_wday == cfg.reboot_wday); // Weekly
  } else if (cfg.reboot_mode == 3) {
    due = ((uint8_t)t.tm_mday == cfg.reboot_mday); // Monthly
  }
  if (!due) return;

  // Marker: yymmddhhmm (UTC unless TZ set)
  uint32_t marker =
    (uint32_t)(((t.tm_year + 1900) % 100) * 100000000UL) +
    (uint32_t)((t.tm_mon + 1) * 1000000UL) +
    (uint32_t)(t.tm_mday * 10000UL) +
    (uint32_t)(t.tm_hour * 100UL) +
    (uint32_t)(t.tm_min);

  static bool loaded = false;
  static uint32_t lastMarker = 0;
  if (!loaded) {
    Preferences p; p.begin("nukaminer", false);
    lastMarker = p.getUInt("last_sched_rb", 0);
    p.end();
    loaded = true;
  }
  if (marker == lastMarker) return;

  // Persist marker before reboot to avoid reboot loops on the same minute.
  {
    Preferences p; p.begin("nukaminer", false);
    p.putUInt("last_sched_rb", marker);
    p.end();
  }
  lastMarker = marker;

  NM_log("[NukaMiner] Scheduled reboot triggered");
  delay(200);
  ESP.restart();
}



static bool ensureBackupDir() {
  if (!sdBegin()) return false;
  if (!SD_MMC.exists(BACKUP_DIR)) {
    SD_MMC.mkdir(BACKUP_DIR);
  }
  // Successful request counts as activity for the idle timeout.
  webSessionTouch(millis());
  return true;
}

static bool isSafeBackupName(const String& name) {
  if (name.length() < 5) return false;
  if (!name.endsWith(".json")) return false;
  if (!name.startsWith("backup-")) return false;
  for (size_t i=0;i<name.length();i++) {
    char c = name[i];
    bool ok = (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.';
    if (!ok) return false;
  }
  // Successful request counts as activity for the idle timeout.
  webSessionTouch(millis());
  return true;
}

static String backupPathFor(const String& name) {
  return String(BACKUP_DIR) + "/" + name;
}

static String makeBackupName() {
  timeSyncOnce();
  time_t now = time(nullptr);
  if (now > 1609459200) { // 2021-01-01
    struct tm tmv;
    gmtime_r(&now, &tmv);
    char buf[32];
    snprintf(buf, sizeof(buf), "backup-%04d%02d%02d-%02d%02d%02d.json",
      tmv.tm_year+1900, tmv.tm_mon+1, tmv.tm_mday,
      tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return String(buf);
  }
  // No valid wall-clock: use monotonically increasing sequence
  Preferences pp; pp.begin("nukaminer", false);
  uint32_t seq = pp.getUInt("backup_seq", 0) + 1;
  pp.putUInt("backup_seq", seq);
  pp.end();
  char buf[32];
  snprintf(buf, sizeof(buf), "backup-seq%lu.json", (unsigned long)seq);
  return String(buf);
}

static void buildBackupJson(JsonDocument& doc) {
  // Schema header
  doc["schema_version"] = 2;

  // Export timestamp (UTC seconds). If time is not synced, this may be 0.
  time_t now = time(nullptr);
  doc["exported_at_unix"] = (uint32_t)((now > 0) ? now : 0);

  // Keep WiFi profiles at top-level for easy management.
  JsonArray wparr = doc.createNestedArray("wifi_profiles");
  for (const auto& p : wifiProfiles) {
    JsonObject o = wparr.createNestedObject();
    o["ssid"] = p.ssid;
    o["pass"] = p.pass;
    o["prio"] = p.prio;
  }
  doc["wifi_last"] = wifiLastSsid;

  // Configuration payload
  JsonObject c = doc.createNestedObject("config");

  // Main
  c["wifi_ssid"]    = cfg.wifi_ssid;
  c["wifi_pass"]    = cfg.wifi_pass;
  c["duco_user"]    = cfg.duco_user;
  c["rig_id"]       = cfg.rig_id;
  c["miner_key"]    = cfg.miner_key;
  c["ntp_server"]   = cfg.ntp_server;
  c["tz"]           = cfg.tz_name;
  c["pool_cache_s"] = cfg.pool_cache_s;

  // Mining (performance mode replaces old per-core toggles)
  const bool maxPerf = (cfg.core1_enabled && cfg.core2_enabled);
  c["performance_mode"] = maxPerf ? "c12" : "c2";
  c["duco_enabled"] = cfg.duino_enabled;

  // Display
  c["display_sleep_s"]  = cfg.display_sleep_s;
  c["lcd_brightness"]   = cfg.lcd_brightness;
  c["lcd_rot180"]       = cfg.lcd_rot180;
  c["carousel_enabled"] = cfg.carousel_enabled;
  c["carousel_seconds"] = cfg.carousel_seconds;

  // LED
  c["led_enabled"]      = cfg.led_enabled;
  c["led_brightness"]   = cfg.led_brightness;

  // Web
  c["web_enabled"]      = cfg.web_enabled;
  c["web_always_on"]    = cfg.web_always_on;
  c["web_timeout_s"]    = cfg.web_timeout_s;
  c["web_user"]         = cfg.web_user;
  c["web_pass"]         = cfg.web_pass;

  // Scheduled reboot
  JsonObject rb = c.createNestedObject("scheduled_reboot");
  rb["mode"] = cfg.reboot_mode;   // 0=Off,1=Daily,2=Weekly,3=Monthly
  rb["hour"] = cfg.reboot_hour;   // 0-23
  rb["min"]  = cfg.reboot_min;    // 0-59
  rb["wday"] = cfg.reboot_wday;   // 0-6 (Sun-Sat)
  rb["mday"] = cfg.reboot_mday;   // 1-31
}

static bool sdBackupConfigToFile(const String& fullPath) {
  if (!ensureBackupDir()) return false;
  if (SD_MMC.exists(fullPath.c_str())) SD_MMC.remove(fullPath.c_str());
  File f = SD_MMC.open(fullPath.c_str(), FILE_WRITE);
  if (!f) return false;

  JsonDocument doc;
  buildBackupJson(doc);

  serializeJson(doc, f);
  f.close();
  return true;
}

static bool applyConfigFromJson(JsonDocument& doc) {
  // Start from sane defaults (like a fresh device), then apply any fields that exist.
  cfg = AppConfig();
  cfg.rig_id = "NukaMiner"; // struct default is empty; match loadConfig() default

  wifiProfiles.clear();
  wifiLastSsid = "";

  // Support both {config:{...}} and legacy flat JSON
  JsonVariant cfgVar = doc["config"];
  JsonObject src = cfgVar.is<JsonObject>() ? cfgVar.as<JsonObject>() : doc.as<JsonObject>();

  // WiFi profiles (preferred)
  if (doc["wifi_profiles"].is<JsonArray>()) {
    for (JsonObject o : doc["wifi_profiles"].as<JsonArray>()) {
      WifiProfile wp;
      wp.ssid = String((const char*)(o["ssid"] | ""));
      wp.pass = String((const char*)(o["pass"] | ""));
      wp.prio = (int16_t)(o["prio"] | 100);
      wp.prio = (int16_t)constrain((int)wp.prio, -999, 999);
      if (wp.ssid.length() == 0) continue;
      wifiProfiles.push_back(wp);
      if (wifiProfiles.size() >= 12) break;
    }
    wifiProfilesSort();
    wifiLastSsid = String((const char*)(doc["wifi_last"] | ""));
    wifiProfilesSave();
    if (!wifiProfiles.empty()) { cfg.wifi_ssid = wifiProfiles[0].ssid; cfg.wifi_pass = wifiProfiles[0].pass; }
  }

  // Basic WiFi (fallback)
  cfg.wifi_ssid = src["wifi_ssid"] | cfg.wifi_ssid;
  cfg.wifi_pass = src["wifi_pass"] | cfg.wifi_pass;

  // Main
  cfg.duco_user   = src["duco_user"] | cfg.duco_user;
  cfg.rig_id      = src["rig_id"] | cfg.rig_id;
  cfg.miner_key   = src["miner_key"] | cfg.miner_key;
  cfg.ntp_server  = src["ntp_server"] | cfg.ntp_server;
  cfg.tz_name     = src["tz"] | (src["tz_name"] | cfg.tz_name);
  cfg.pool_cache_s = (uint32_t)(src["pool_cache_s"] | cfg.pool_cache_s);
  if (cfg.pool_cache_s > 86400) cfg.pool_cache_s = 86400;

  // Mining / performance mode
  String pm = String((const char*)(src["performance_mode"] | (src["core_mode"] | "")));
  pm.trim();
  if (pm == "c12") { cfg.core1_enabled = true; cfg.core2_enabled = true; }
  else if (pm == "c2") { cfg.core1_enabled = false; cfg.core2_enabled = true; }
  else {
    // Best-effort import of legacy fields if present
    cfg.core1_enabled = src["core1_enabled"] | (src["c1_en"] | cfg.core1_enabled);
    cfg.core2_enabled = src["core2_enabled"] | (src["c2_en"] | cfg.core2_enabled);
  }
  cfg.duino_enabled = src["duco_enabled"] | (src["duino_enabled"] | cfg.duino_enabled);

  // Display
  cfg.display_sleep_s  = (uint32_t)(src["display_sleep_s"] | (src["disp_sleep"] | cfg.display_sleep_s));
  cfg.lcd_brightness   = (uint8_t)(src["lcd_brightness"] | (src["lcd_br"] | cfg.lcd_brightness));
  cfg.lcd_rot180       = src["lcd_rot180"] | (src["lcd_r180"] | cfg.lcd_rot180);
  cfg.carousel_enabled = src["carousel_enabled"] | (src["car_en"] | cfg.carousel_enabled);
  cfg.carousel_seconds = (uint16_t)(src["carousel_seconds"] | (src["car_s"] | cfg.carousel_seconds));

  // LED
  cfg.led_enabled      = src["led_enabled"] | (src["led_en"] | cfg.led_enabled);
  cfg.led_brightness   = (uint8_t)(src["led_brightness"] | (src["led_br"] | cfg.led_brightness));
  if (cfg.led_brightness > 100) cfg.led_brightness = 100;

  // Web
  cfg.web_enabled      = src["web_enabled"] | (src["web_en"] | cfg.web_enabled);
  cfg.web_always_on    = src["web_always_on"] | (src["web_always"] | cfg.web_always_on);
  cfg.web_timeout_s    = (uint16_t)(src["web_timeout_s"] | (src["web_to"] | cfg.web_timeout_s));
  cfg.web_user         = src["web_user"] | cfg.web_user;
  cfg.web_pass         = src["web_pass"] | cfg.web_pass;

  // Scheduled reboot (nested object preferred)
  if (src["scheduled_reboot"].is<JsonObject>()) {
    JsonObject rb = src["scheduled_reboot"].as<JsonObject>();
    cfg.reboot_mode = (uint8_t)(rb["mode"] | cfg.reboot_mode);
    cfg.reboot_hour = (uint8_t)(rb["hour"] | cfg.reboot_hour);
    cfg.reboot_min  = (uint8_t)(rb["min"]  | cfg.reboot_min);
    cfg.reboot_wday = (uint8_t)(rb["wday"] | cfg.reboot_wday);
    cfg.reboot_mday = (uint8_t)(rb["mday"] | cfg.reboot_mday);
  } else {
    cfg.reboot_mode = (uint8_t)(src["reboot_mode"] | (src["rb_mode"] | cfg.reboot_mode));
    cfg.reboot_hour = (uint8_t)(src["reboot_hour"] | (src["rb_h"] | cfg.reboot_hour));
    cfg.reboot_min  = (uint8_t)(src["reboot_min"]  | (src["rb_m"] | cfg.reboot_min));
    cfg.reboot_wday = (uint8_t)(src["reboot_wday"] | (src["rb_wd"] | cfg.reboot_wday));
    cfg.reboot_mday = (uint8_t)(src["reboot_mday"] | (src["rb_md"] | cfg.reboot_mday));
  }

  // Clamp reboot values
  if (cfg.reboot_mode > 3) cfg.reboot_mode = 0;
  if (cfg.reboot_hour > 23) cfg.reboot_hour = 3;
  if (cfg.reboot_min > 59) cfg.reboot_min = 0;
  if (cfg.reboot_wday > 6) cfg.reboot_wday = 0;
  if (cfg.reboot_mday < 1) cfg.reboot_mday = 1;
  if (cfg.reboot_mday > 31) cfg.reboot_mday = 31;

  // Clamp LCD brightness
  if (cfg.lcd_brightness > 100) cfg.lcd_brightness = 100;

  saveConfig();
  return true;
}

static bool sdRestoreConfigFromFile(const String& fullPath) {
  if (!sdBegin()) return false;
  if (!SD_MMC.exists(fullPath.c_str())) return false;
  File f = SD_MMC.open(fullPath.c_str(), FILE_READ);
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  return applyConfigFromJson(doc);
}

static void listBackupFiles(std::vector<String>& out) {
  out.clear();
  if (!ensureBackupDir()) return;
  File dir = SD_MMC.open(BACKUP_DIR);
  if (!dir) return;
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String name = f.name();
      // f.name() may include '/backups/' prefix depending on FS; normalize
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash+1);
      if (isSafeBackupName(name)) out.push_back(name);
    }
    f = dir.openNextFile();
  }
  // Sort descending (newest first) by filename
  std::sort(out.begin(), out.end(), [](const String& a, const String& b){ return a > b; });
}

// -----------------------------
// Display helpers
// -----------------------------
static constexpr int BL_PWM_CH = 0;
static constexpr int BL_PWM_FREQ = 5000;
static constexpr int BL_PWM_RES  = 8; // 0-255
static bool blInited = false;
// Runtime backlight level. IMPORTANT: Do NOT tie temporary runtime actions
// (like display sleep/off) to the persisted config value.
static uint8_t blRuntimePercent = 50;

static void blInitOnce() {
  if (blInited) return;
  // Some LilyGO T-Dongle-S3 revisions expose two BL pins; keep both enabled.
  pinMode(PIN_BL_37, OUTPUT);
  digitalWrite(PIN_BL_37, LOW); // enable (active-low on many boards)

  ledcSetup(BL_PWM_CH, BL_PWM_FREQ, BL_PWM_RES);
  ledcAttachPin(PIN_BL_38, BL_PWM_CH);
  blInited = true;
}

// If updateConfig==true, also keep cfg.lcd_brightness in sync (used when the user
// changes brightness). For temporary actions, pass updateConfig=false.
static void blSet(uint8_t percent, bool updateConfig = true) {
  blInitOnce();
  if (percent > 100) percent = 100;
  blRuntimePercent = percent;
  if (updateConfig) {
    cfg.lcd_brightness = percent; // keep in sync for immediate UI effect
  }

  // Backlight is often active-low: LOW = on, HIGH = off.
  // PWM duty is inverted so 100% brightness => duty 0 (always LOW)
  // and 0% brightness => duty 255 (always HIGH).
  uint8_t duty = (uint8_t)(255 - (uint32_t)percent * 255 / 100);
  ledcWrite(BL_PWM_CH, duty);

  // Also toggle the secondary BL pin (if present) for full off at 0%.
  pinMode(PIN_BL_37, OUTPUT);
  digitalWrite(PIN_BL_37, (percent == 0) ? HIGH : LOW);
}

// -----------------------------
// RGB LED helpers
// -----------------------------
static uint32_t ledColorForMode(LedMode m) {
  switch (m) {
    case LED_YELLOW: return rgb.Color(255, 180, 0);
    case LED_GREEN:  return rgb.Color(0, 255, 0);
    case LED_BLUE:   return rgb.Color(0, 80, 255);
    case LED_RED:    return rgb.Color(255, 0, 0);
    case LED_PURPLE: return rgb.Color(160, 0, 200);
    case LED_OFF:
    default:         return rgb.Color(0, 0, 0);
  }
}

static void ledInit() {
  rgb.begin();
  rgb.clear();
  rgb.show();
  ledModeLast = LED_OFF;
  ledBrightnessLast = 255;
}

static void ledSetMode(LedMode m) {
  ledMode = m;
}

static void ledApplyNow() {
  if (!cfg.led_enabled) {
    rgb.setBrightness(0);
    rgb.clear();
    rgb.show();
    ledModeLast = LED_OFF;
    return;
  }
  uint8_t b = cfg.led_brightness;
  if (b > 100) b = 100;
  uint8_t b255 = (uint8_t)((uint32_t)b * 255 / 100);
  if (b255 != ledBrightnessLast) {
    rgb.setBrightness(b255);
    ledBrightnessLast = b255;
  }
  if (ledMode != ledModeLast) {
    rgb.setPixelColor(0, ledColorForMode(ledMode));
    rgb.show();
    ledModeLast = ledMode;
  }
}

static void ledService() {
  // Locate mode overrides all other LED behavior
  if (locateMode) {
    const bool on = ((millis() / 450UL) % 2UL) == 0UL;

    // Locate must work even if the RGB LED is disabled in settings.
    // We deliberately bypass cfg.led_enabled here for the duration of locate mode.
    uint8_t b = cfg.led_brightness;
    if (b < 15) b = 15;
    if (b > 100) b = 100;
    uint8_t b255 = (uint8_t)((uint32_t)b * 255 / 100);

    rgb.setBrightness(b255);
    rgb.setPixelColor(0, on ? ledColorForMode(LED_PURPLE) : 0);
    rgb.show();

    ledModeLast = on ? LED_PURPLE : LED_OFF;
    ledBrightnessLast = b255;
    return;
  }

  // Determine desired LED state
  LedMode m = LED_OFF;

  // AP / captive portal mode
  if (portalRunning || WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
    m = LED_BLUE;
  } else {
    // If the SD file manager is actively transferring a file, pause mining and show yellow.
    if (sdBusy) {
      m = LED_YELLOW;
    } else {
    const bool wantMining = cfg.duino_enabled;
    const bool wifiOk = WiFi.isConnected();
    const bool mining = minerIsRunning();

    if (!wantMining) {
      m = LED_YELLOW;
    } else if (!wifiOk) {
      // supposed to be mining but wifi is down
      m = LED_RED;
    } else if (mining) {
      m = LED_GREEN;
    } else {
      // wifi ok but mining not active
      m = LED_YELLOW;
    }
    }
  }

  ledSetMode(m);
  ledApplyNow();
}


// Framebuffer color handling
// Store standard RGB565 values (same encoding as TFT_eSPI color constants).
// Do NOT swap channels here. Color order is handled by the TFT controller init
// (via TFT_eSPI User_Setup). Swapping in software will distort colors
// (e.g., RED appearing as GREEN/PURPLE on some setups).
static inline uint16_t fbEnc(uint16_t c) {
  return c;
}

static void fbFill(uint16_t c) {
  for (int i = 0; i < WIDTH * HEIGHT; i++) fb[i] = fbEnc(c);
}

static void fbPixel(int x, int y, uint16_t c) {
  if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT) return;
  fb[idx(x,y)] = fbEnc(c);
}

static void fbRect(int x, int y, int w, int h, uint16_t c) {
  for (int i=0;i<w;i++) { fbPixel(x+i,y,c); fbPixel(x+i,y+h-1,c); }
  for (int j=0;j<h;j++) { fbPixel(x,y+j,c); fbPixel(x+w-1,y+j,c); }
}

// Simple line drawing (Bresenham) into framebuffer
static void fbLine(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1 - x0);
  int sx = (x0 < x1) ? 1 : -1;
  int dy = -abs(y1 - y0);
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx + dy;
  while (true) {
    fbPixel(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

static void fbFillRect(int x, int y, int w, int h, uint16_t c) {
  int x2 = min(WIDTH, x+w);
  int y2 = min(HEIGHT, y+h);
  for (int yy=max(0,y); yy<y2; yy++)
    for (int xx=max(0,x); xx<x2; xx++)
      fb[idx(xx,yy)] = fbEnc(c);
}

static void fbDrawChar(int x, int y, char ch, uint16_t color, int scale) {
  const uint8_t* bm = getCharBitmap(ch);
  for (int row = 0; row < 8; row++) {
    uint8_t line = bm[row];
    for (int col = 0; col < 8; col++) {
      if (line & (1 << (7 - col))) {
        for (int sy = 0; sy < scale; sy++) {
          for (int sx = 0; sx < scale; sx++) {
            fbPixel(x + col * scale + sx, y + row * scale + sy, color);
          }
        }
      }
    }
  }
}

static void fbText(const char* txt, int x, int y, uint16_t color, uint8_t size, bool center=false) {
  if (!txt) return;
  int len = (int)strlen(txt);
  int totalW = len * 8 * (int)size;
  int startX = center ? (x - totalW/2) : x;
  for (int i = 0; i < len; i++) {
    fbDrawChar(startX + i * 8 * (int)size, y, txt[i], color, (int)size);
  }
}

static void fbTextClip(const String& s, int x, int y, uint16_t color, uint8_t size, int maxW) {
  // Clip text to fit in maxW pixels (no wrap). Adds "…" if truncated.
  int charW = 8 * (int)size;
  if (charW <= 0) return;
  int maxChars = maxW / charW;
  if (maxChars <= 0) return;

  String out = s;
  if ((int)out.length() > maxChars) {
    if (maxChars >= 1) {
      // Use single-character ellipsis where possible.
      out = out.substring(0, maxChars - 1) + String("…");
    } else {
      out = "";
    }
  }
  fbText(out.c_str(), x, y, color, size, false);
}

static void fbPush() {
  if (!fbBack) return;

  // Push the completed back buffer to the physical LCD.
  // This avoids some ESP32-S3 + TFT_eSPI combinations crashing when using pushImage
  // or when using sprites for text rendering.
  digitalWrite(PIN_BL_37, LOW);
  digitalWrite(PIN_BL_38, LOW);
  tft.startWrite();
  tft.setAddrWindow(0, 0, WIDTH, HEIGHT);
  tft.pushPixels(fbBack, WIDTH * HEIGHT);
  tft.endWrite();

  // Atomically swap buffers so the web always sees a complete frame (no tearing).
  portENTER_CRITICAL(&fbMux);
  uint16_t* tmp = fbFront;
  fbFront = fbBack;
  fbBack = tmp;
  fb = fbBack; // keep draw helpers writing into the back buffer
  fbGen++;
  portEXIT_CRITICAL(&fbMux);
}

// -----------------------------
// Captive portal (initial setup)
// -----------------------------
static DNSServer dns;
static WebServer web(80);
static bool webBegun=false;
bool portalRunning=false;
// Runtime display sleep state (do NOT persist into config)
static bool displaySleeping = false;
// True when portal was started automatically because WiFi wasn't configured/connected.
// If the user explicitly starts the portal (long-press), we keep it running even
// while STA is connected.
static bool portalAuto=false;
// If the user disables the Web UI, AP/Portal mode must still bring up HTTP so the
// device can be recovered. We force-enable Web UI at runtime while the portal is
// running. This is NOT persisted unless the user saves settings.
static bool portalForcedWeb = false;
static bool webEnabledBeforePortal = true;
static IPAddress apIP(192,168,4,1);

// Forward declarations (used in route lambdas before definitions)
// Miner control (forward declarations needed for web handlers)
static void minerStart();
static void minerStop();
static bool minerIsRunning();

static void portalRenderRoot();
static void portalHandleSave();
static void portalHandleBackup();
static void portalHandleRestore();

static String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (char ch: s) {
    if (ch=='&') out += F("&amp;");
    else if (ch=='<') out += F("&lt;");
    else if (ch=='>') out += F("&gt;");
    else if (ch=='"') out += F("&quot;");
    else out += ch;
  }
  return out;
}


static String urlEncode(const String& s) {
  String o; o.reserve(s.length()*3);
  const char* hex = "0123456789ABCDEF";
  for (size_t i=0;i<s.length();i++) {
    uint8_t c = (uint8_t)s[i];
    const bool safe = (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.' || c=='~';
    if (safe) { o += (char)c; }
    else {
      o += '%';
      o += hex[(c>>4)&0xF];
      o += hex[c&0xF];
    }
  }
  return o;
}

static bool requireAuthOrPortal() {
  if (portalRunning) return true; // captive portal shouldn't require auth
  const uint32_t nowMs = millis();
  if (!cfg.web_enabled) { web.send(404, "text/plain", "Not found"); return false; }
  if (!webSessionAllowed(nowMs)) {
    web.sendHeader("Cache-Control", "no-store");
    web.send(403, "text/plain", "Web UI is disabled. Press the BOOT button on the device to enable it temporarily.");
    return false;
  }
  if (cfg.web_user.length() == 0) cfg.web_user = "admin";
  // Basic auth
  if (!web.authenticate(cfg.web_user.c_str(), cfg.web_pass.c_str())) {
    web.requestAuthentication();
    return false;
  }
  // Successful request counts as activity for the idle timeout.
  webSessionTouch(nowMs);
  return true;
}

static String htmlHeader(const String& title) {
  String h;
  h.reserve(512);
  h += F("<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>");
  h += htmlEscape(title);
  h += F("</title>"
         "<style>body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Cantarell,sans-serif;background:#0b1220;color:#e7eaf0;margin:0;padding:18px}.card{max-width:820px;margin:0 auto;background:#111b33;border:1px solid #1f2c52;border-radius:14px;padding:16px}a{color:#7fb3ff} input,select{width:95%;padding:10px;border-radius:10px;border:1px solid #2a3a67;background:#0b1220;color:#e7eaf0}label{display:block;margin-top:12px;margin-bottom:6px;color:#b9c2d6}button{margin-top:16px;padding:12px 14px;border:0;border-radius:12px;background:#2f6fff;color:white;font-weight:700;cursor:pointer}.row{display:grid;grid-template-columns:1fr 1fr;gap:12px}.muted{color:#9aa6c7}pre{background:#0b1220;border:1px solid #1f2c52;border-radius:12px;padding:12px;overflow:auto}.top{display:flex;justify-content:space-between;align-items:flex-start;gap:14px;flex-wrap:wrap}.topLeft{min-width:220px}.smallBtn{display:inline-block;margin-top:8px;padding:10px 12px;border-radius:12px;background:#2f6fff;color:#fff;text-decoration:none;font-weight:700}.danger{background:#d33}.table{width:100%;border-collapse:collapse;margin-top:10px}.table td,.table th{border-bottom:1px solid #1f2c52;padding:8px;text-align:left}.section{margin-top:18px;padding-top:10px;border-top:1px solid #1f2c52}.duco-broken{display:inline-block;margin-left:6px;animation:duco-blink 1s steps(2,end) infinite;color:#ffb36b;text-decoration:line-through}@keyframes duco-blink{50%{opacity:0}}</style></head><body><div class='card'>");
  return h;
}
static String htmlFooter() { return F("</div></body></html>"); }

// JavaScript for the /status page (kept in flash).
extern const char STATUS_JS[];

// Simulate BOOT button actions from the web UI.
static void bootButtonShortPress();
// Start captive portal / AP mode (defined later in this file).
static void portalStart(bool isAutoStart);
static void webHandleBootPress();
static void webHandleStartAP();
static void webHandleLocate();
static void webHandleDeviceControl();

static void webRenderStatus() {
  if (!requireAuthOrPortal()) return;

  String page = htmlHeader("NukaMiner");
  // NOTE: Everything on the Status page must remain inside the main .card div opened by htmlHeader().
  // A previous edit accidentally closed an extra </div> here, causing the dashboard sections to render
  // outside the card (as seen in the screenshot).
  page += F("<div class='top'>"
           "<div class='topLeft'><h2>NukaMiner</h2><p class='muted'>Web dashboard</p><div id='ducoLine' class='muted' style='margin-top:4px;display:none'></div><p><b>Status</b> &nbsp;|&nbsp; <a href='/config'>Config</a></p></div>"

           // Buttons (Screenshot / Boot / Polling)
           "<div class='topRight'>"
           "  <button id='deviceCtlBtn' type='button' class='smallBtn' style='margin:0'>Device Control</button>"
           "</div>"

           // Screenshot modal (Cancel / Download / Save to SD)
           "<div id='dcModal' style='display:none;position:fixed;inset:0;z-index:9999;background:rgba(0,0,0,.55)'>"
"  <div style='max-width:560px;margin:8vh auto;background:#111b33;border:1px solid #1f2c52;border-radius:14px;padding:14px'>"
"    <div style='font-weight:800;margin-bottom:8px'>Device Control</div>"
"    <div class='muted' style='margin-bottom:10px'>Carousel is paused while this window is open. Use Boot to change pages, then take a screenshot.</div>"
"    <canvas id='dcCanvas' width='160' height='80' style='width:100%;height:auto;aspect-ratio:160/80;border-radius:12px;border:1px solid #1f2c52;background:#0b1220;image-rendering:pixelated;display:block'></canvas>"
"    <div class='muted' style='margin-top:8px'>Next refresh in <span id='dcCountdown'>5</span>s</div>"
"    <div id='dcViewMain'>"
"      <div class='row' style='margin-top:12px;gap:10px;flex-wrap:wrap'>"
"        <button id='dcApBtn' type='button' class='smallBtn'>Start AP Mode</button>"
"        <button id='dcRebootBtn' type='button' class='smallBtn danger'>Reboot</button>"
"        <button id='dcLocateBtn' type='button' class='smallBtn'>Locate: OFF</button>"
"        <button id='dcBootBtn' type='button' class='smallBtn'>Boot Button</button>"
"        <button id='dcShotBtn' type='button' class='smallBtn'>Take Screenshot</button>"
"        <button id='dcCloseBtn' type='button' class='smallBtn'>Exit</button>"
"      </div>"
"      <div id='dcMsg' class='muted' style='margin-top:10px;min-height:18px'></div>"
"    </div>"
"    <div id='dcViewShot' style='display:none;margin-top:12px'>"
"      <div style='font-weight:800;margin-bottom:6px'>Screenshot ready</div>"
"      <div class='muted' style='margin-bottom:10px'>Choose what to do with the PNG screenshot.</div>"
"      <div class='row' style='gap:10px;flex-wrap:wrap'>"
"        <button id='dcDl2Btn' type='button' class='smallBtn'>Download PNG</button>"
"        <button id='dcSd2Btn' type='button' class='smallBtn'>Save PNG to SD</button>"
"        <div style='flex:1'></div>"
"        <button id='dcBackBtn' type='button' class='smallBtn'>Back</button>"
"      </div>"
"      <div id='dcShotMsg' class='muted' style='margin-top:10px;min-height:18px'></div>"
"    </div>"
"  </div>"
"</div>"
           "</div><br>");

  page += F("<div class='row'>"
	            "<div><b>Device</b><div id='dev' class='muted'>...</div><div id='webui' class='muted' style='margin-top:6px'>...</div></div>"
	            "<div><b>Network</b><div id='net' class='muted'>...</div></div>"
            "</div>");

  page += F("<h3>Mining</h3><pre id='mine'>Loading...</pre>");
  page += F("<h3>Hashrate (kH/s)</h3><div style='display:flex;gap:10px;align-items:stretch'>  <div style='flex:1'>    <canvas id='hr' width='780' height='180' style='width:100%;border:1px solid #1f2c52;border-radius:12px;background:#0b1220'></canvas>  </div></div>");

  // Temperature graph (web-only; uses temp_c already present in status.json)
  page += F("<h3>Temperature (&deg;C)</h3><div style='display:flex;gap:10px;align-items:stretch'>  <div style='flex:1'>    <canvas id='tg' width='780' height='160' style='width:100%;border:1px solid #1f2c52;border-radius:12px;background:#0b1220'></canvas>  </div></div><br>");

  page += F("<div style='display:flex;justify-content:space-between;align-items:center;gap:10px'>"
            "<h3 id='console' style='margin:0'>Console</h3>"
            "<div style='display:flex;gap:10px;align-items:center'>"
              "<button id='consoleToggleBtn' type='button' class='smallBtn' style='margin:0'>Console: OFF</button>"
              "<button id='followBtn' type='button' class='smallBtn' style='margin:0'>Follow: OFF</button>"
            "</div>"
            "</div>"
            "<pre id='log' style='height:240px;overflow:auto;display:none'>Loading...</pre>");
  // Status page: device actions are in Device Control.

  // Inline JS (avoid /status.js 404 issues and keep the page self-contained).
  page += F("<script>");
  page += FPSTR(STATUS_JS);
  page += F("</script>");

  page += htmlFooter();
  web.send(200, "text/html", page);
}

// JavaScript for the /status page (inlined into /status HTML).
// STATUS_JS moved to src/web_assets.h

static void webRenderConfig() {
  if (!requireAuthOrPortal()) return;

  String page = htmlHeader("NukaMiner Settings");
  page += F("<h2>Config</h2><p class='muted'>Change settings (requires reboot for some).</p>");

  if (portalRunning) { page += F("<div style='padding:10px 12px;border-radius:12px;background:#2f6fff;color:#fff;font-weight:800;margin:10px 0'>AP MODE: You are connected to the device's setup WiFi (captive portal).</div>"); }

  if (portalRunning) page += F("<p><b>Config</b></p>");
  else page += F("<p><a href='/status'>Status</a> &nbsp;|&nbsp; <b>Config</b></p>");
  page += F("<form method='post' action='/save_settings'>");
  page += String("<div id='tzName' data-tz='") + htmlEscape(cfg.tz_name) + "' style='display:none'></div>";

  // --- MAIN ---
  const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  const String connectedSsid = wifiConnected ? WiFi.SSID() : String("");
  const String ssidShown = wifiConnected ? connectedSsid : cfg.wifi_ssid;
  page += F("<div class='section'><h3>Main</h3>");

  page += F("<div class='row'><div><label>WiFi SSID</label>"
            "<div style='display:flex;gap:8px;align-items:center'>"
            "<input id='wifi_ssid' name='wifi_ssid' value='");
  page += htmlEscape(ssidShown);
  page += F("' style='flex:1'>"
            "<button type='button' id='wifiScanBtn' class='smallBtn'>Scan</button>"
            "<button type='button' class='smallBtn' onclick=\"location.href='/wifi'\">Manage WiFi profiles</button>"
            "</div>"
            "<select id='wifiScanList' style='margin-top:8px;display:none'></select>"
            "<div class='muted' id='wifiScanMsg' style='min-height:18px'></div>"
            "</div>"
            "<div><label>WiFi profiles</label>"
            "<div class='muted'>Saving here will add/update a WiFi profile. New SSIDs are added to the bottom (lowest priority).</div></div></div>");

  // Do not prefill password. If left blank, we'll keep the existing password for the current SSID.
  page += F("<div class='row'><div><label>WiFi Password</label><input type='password' name='wifi_pass' value='' placeholder='(unchanged)'></div><div></div></div>");

  page += F("<div class='row'><div><label>Duino-Coin username</label>"
            "<div style='display:flex;gap:8px;align-items:center'>"
            "<input name='duco_user' value='");
  page += htmlEscape(cfg.duco_user);
  page += F("' style='flex:1'>"
            "<button type='button' class='smallBtn' onclick=\"window.open('https://duinocoin.com/','_blank')\">Create Account</button>"
            "</div>"
            "</div>"
            "<div><label>Duino-Coin account</label>"
            "<div class='muted'>Need an account?<br>Press the button and create one at DuinoCoin.com</div>"
            "</div></div>");

  page += F("<div class='row'><div><label>Rig identifier</label><input name='rig_id' value='");
  page += htmlEscape(cfg.rig_id);
  page += F("'></div><div><label>Mining key (optional)</label><input name='miner_key' value='");
  page += htmlEscape(cfg.miner_key);
  page += F("'></div></div>");
page += F("<label>NTP server</label><input name='ntp_server' value='");
  page += htmlEscape(cfg.ntp_server);
  page += F("'><div class='muted'>Defaults to <code>pool.ntp.org</code>. Used after WiFi connects.</div>");

  // Timezone for web UI display
  page += F("<label>Timezone</label><select name='tz'>");
  {
    const char* tzs[] = {"UTC","America/New_York","America/Chicago","America/Denver","America/Los_Angeles","America/Phoenix","America/Anchorage","Pacific/Honolulu","Europe/London","Europe/Paris","Europe/Berlin","Asia/Tokyo","Asia/Shanghai","Asia/Kolkata","Australia/Sydney"};
    for (auto tz : tzs) {
      page += String("<option value='") + tz + "' " + (cfg.tz_name == tz ? "selected" : "") + ">" + tz + "</option>";
    }
  }
  page += F("</select><div class='muted'>Used only by the web UI for displaying UTC device times in the selected zone.</div>");

  page += F("</div>"); // section

  // WiFi scan helper for the SSID field
  page += F(
    "<script>\n"
    "window.addEventListener('DOMContentLoaded', ()=>{\n"
    "  const btn=document.getElementById('wifiScanBtn');\n"
    "  const sel=document.getElementById('wifiScanList');\n"
    "  const msg=document.getElementById('wifiScanMsg');\n"
    "  const inSsid=document.getElementById('wifi_ssid');\n"
    "  if(!btn||!sel||!msg||!inSsid) return;\n"
    "  function bars(rssi){ const v=Math.max(0,Math.min(4,Math.round((rssi+90)/12))); return ' ' + '▂▃▄▆█'.slice(0,v+1); }\n"
    "  btn.addEventListener('click', async ()=>{\n"
    "    btn.disabled=true; msg.textContent='Scanning...'; sel.style.display='none';\n"
    "    try{\n"
    "      const r=await fetch('/wifi/scan.json',{cache:'no-store',credentials:'include'});\n"
    "      if(!r.ok){ msg.textContent='Scan failed (HTTP '+r.status+')'; btn.disabled=false; return; }\n"
    "      const j=await r.json();\n"
    "      const nets=(j&&j.networks)||[]; sel.innerHTML='';\n"
    "      if(!nets.length){ msg.textContent='No networks found.'; btn.disabled=false; return; }\n"
    "      for(const n of nets){\n"
    "        const o=document.createElement('option'); o.value=n.ssid||'';\n"
    "        const sec=(n.sec && n.sec!=='open') ? ' 🔒' : '';\n"
    "        o.textContent=(n.ssid||'(hidden)')+'  '+(n.rssi||0)+' dBm'+bars(n.rssi)+sec;\n"
    "        sel.appendChild(o);\n"
    "      }\n"
    "      sel.style.display='block'; msg.textContent='Select a network to fill the SSID.';\n"
    "    }catch(e){ msg.textContent='Scan failed.'; }\n"
    "    btn.disabled=false;\n"
    "  });\n"
    "  sel.addEventListener('change', ()=>{ if(sel.value) inSsid.value=sel.value; });\n"
    "});\n"
    "</script>\n"
  );

  // --- MINING ---
  page += F("<div class='section'><h3>Mining</h3>");

  page += F("<div class='row'><div><label>Mining enabled</label><select name='duco_en'>"
            "<option value='1' ");
  page += (cfg.duino_enabled ? "selected" : "");
  page += F(">Yes</option><option value='0' ");
  page += (!cfg.duino_enabled ? "selected" : "");
  page += F(">No</option></select></div>"
            "<div><label>Miner core assignment</label>"
            "<div class='muted'><b>Core 2</b> is the default miner as <b>Core 1</b> controls WiFi and Web UI.<br>Mining speed is <b>auto-managed</b> to keep Web/WiFi/Watchdog responsive.<br><b>Max Performance</b> uses both cores and will increase CPU temperature.</div></div></div><br>");

  // Friendly performance mode selector (replaces individual Core 1/Core 2 toggles)
  page += F("<div class='row'>"
            "<div><label>Performance mode</label>"
            "<select name='core_mode'>");
  const bool maxPerf = (cfg.core1_enabled && cfg.core2_enabled);
  page += String("<option value='c2' ") + (!maxPerf ? "selected" : "") + ">Core 2 only (Default)</option>";
  page += String("<option value='c12' ") + (maxPerf ? "selected" : "") + ">Core 1 and 2 (Max Performance)</option>";
  page += F("</select></div>"
            "<div></div>"
            "</div>");

  // Dashboard grouping id (shared across workers)
  page += F("<div class='row'><div><label>Group ID (threads)</label>"
            "<div style='display:flex;gap:10px;align-items:center'>"
              "<input id='duco_gid' readonly style='flex:1' value='");
  page += htmlEscape(getOrCreateDucoGroupId());
  page += F("'>"
              "<button type='button' id='regenGid' class='smallBtn' style='margin-top:0;white-space:nowrap'>Regenerate</button>"
            "</div>"
            "<div class='muted' style='margin-top:6px'>Workers share this ID so the Duino-Coin dashboard shows one miner with multiple threads.</div>"
            "<div id='gidMsg' class='muted' style='min-height:18px;margin-top:6px'></div>"
            "</div><div></div></div>");

  page += F("<script>"
            "document.getElementById('regenGid').addEventListener('click', async ()=>{"
            "  const b=document.getElementById('regenGid');"
            "  const m=document.getElementById('gidMsg');"
            "  b.disabled=true; m.textContent='Regenerating...';"
            "  try{"
            "    const r=await fetch('/duco_gid/regenerate',{method:'POST'});"
            "    const j=await r.json();"
            "    if(j && j.duco_gid){ document.getElementById('duco_gid').value=j.duco_gid; m.textContent='Updated.'; }"
            "    else { m.textContent='Failed.'; }"
            "  }catch(e){ m.textContent='Failed.'; }"
            "  b.disabled=false;"
            "});"
            "</script>");


  page += F("<div class='row'><div><label>Pool lookup cache (seconds)</label>"
            "<input type='number' min='0' max='86400' name='pool_cache_s' value='");
  page += String(cfg.pool_cache_s);
  page += F("'>"
            "<div class='muted'>Caches the HTTPS <code>/getPool</code> lookup to reduce TLS/JSON overhead. Set 0 to disable.</div>"
            "</div></div>");

  page += F("</div>"); // section

  // --- DISPLAY ---
  page += F("<div class='section'><h3>Display</h3>");

  page += F("<div class='row'><div><label>Display sleep (seconds, 0 = never)</label>"
            "<input name='disp_sleep' type='number' min='0' max='86400' value='");
  page += String(cfg.display_sleep_s);
  page += F("'></div><div><label>LCD orientation</label><select name='lcd_r180'>"
            "<option value='0' ");
  page += (!cfg.lcd_rot180 ? "selected" : "");
  page += F(">Normal</option><option value='1' ");
  page += (cfg.lcd_rot180 ? "selected" : "");
  page += F(">Rotated 180°</option></select>"
            "<div class='muted'>Applies at boot and immediately after saving</div></div></div>");

  page += F("<div class='row'><div><label>LCD Brightness (0-100)</label>"
            "<input type='range' min='0' max='100' name='lcd_br' value='");
  page += String(cfg.lcd_brightness);
  page += F("' oninput=\"document.getElementById('brv').textContent=this.value+'%';\">"
            "<div class='muted'>Current: <span id='brv'>");
  page += String(cfg.lcd_brightness);
  page += F("%</span></div></div>"
            "<div><label>Carousel mode (STA only)</label><select name='car_en'>"
            "<option value='1' ");
  page += (cfg.carousel_enabled ? "selected" : "");
  page += F(">Enabled</option><option value='0' ");
  page += (!cfg.carousel_enabled ? "selected" : "");
  page += F(">Disabled</option></select>"
            "<div class='muted'>Auto-cycle pages when connected to WiFi</div></div></div>");

  page += F("<div class='row'><div><label>Carousel seconds</label>"
            "<input name='car_s' type='number' min='2' max='3600' value='");
  page += String(cfg.carousel_seconds);
  page += F("'></div><div></div></div>");

  page += F("</div>"); // section

  // --- LED ---
  page += F("<div class='section'><h3>LED</h3>");

  page += F("<div class='row'><div><label>RGB LED</label><select name='led_en'>"
            "<option value='1' ");
  page += (cfg.led_enabled ? "selected" : "");
  page += F(">Enabled</option><option value='0' ");
  page += (!cfg.led_enabled ? "selected" : "");
  page += F(">Disabled</option></select>"
            "<div class='muted'>BLUE=AP mode, GREEN=mining, YELLOW=paused, RED=error</div></div>"
            "<div><label>LED Brightness (0-100)</label>"
            "<input type='range' min='0' max='100' name='led_br' value='");
  page += String(cfg.led_brightness);
  page += F("' oninput=\"document.getElementById('ledv').textContent=this.value+'%';\">"
            "<div class='muted'>Current: <span id='ledv'>");
  page += String(cfg.led_brightness);
  page += F("%</span></div></div></div>");

  page += F("</div>"); // section

  // --- WEB ---
  page += F("<div class='section'><h3>Web</h3>");

  page += F("<div class='row'><div><label>Web UI enabled</label><select name='web_en'>"
            "<option value='1' ");
  page += (cfg.web_enabled ? "selected" : "");
  page += F(">Yes</option><option value='0' ");
  page += (!cfg.web_enabled ? "selected" : "");
  page += F(">No</option></select></div>"
            "<div><label>Web UI always on</label><select name='web_always'>"
            "<option value='1' ");
  page += (cfg.web_always_on ? "selected" : "");
  page += F(">Yes</option><option value='0' ");
  page += (!cfg.web_always_on ? "selected" : "");
  page += F(">No</option></select></div></div>");

  page += F("<div class='row'><div><label>Web UI timeout (seconds)</label><input name='web_to' type='number' min='30' max='86400' value='");
  page += String(cfg.web_timeout_s);
  page += F("'><div class='muted'>Used only when &quot;Web UI always on&quot; is No (press BOOT to enable temporarily).</div></div><div></div></div>");

  page += F("<div class='row'><div><label>Web UI username</label><input name='web_user' value='");
  page += htmlEscape(cfg.web_user);
  page += F("'></div></div>");

  page += F("<label>Web UI password</label><input name='web_pass' type='password' value='");
  page += htmlEscape(cfg.web_pass);
  page += F("'>");

  page += F("</div>"); // section

  page += F("<div class='section'><h3>Maintenance</h3>");
  // Scheduled reboot
  page += F("<div class='row'><div><label>Scheduled reboot</label><select name='rb_mode'>"
            "<option value='0' ");
  page += (cfg.reboot_mode==0 ? "selected" : "");
  page += F(">Off</option><option value='1' ");
  page += (cfg.reboot_mode==1 ? "selected" : "");
  page += F(">Daily</option><option value='2' ");
  page += (cfg.reboot_mode==2 ? "selected" : "");
  page += F(">Weekly</option><option value='3' ");
  page += (cfg.reboot_mode==3 ? "selected" : "");
  page += F(">Monthly</option></select></div>"
            "<div><label>Time (UTC)</label>"
            "<div style='display:flex;gap:8px;align-items:center'>"
            "<input type='number' min='0' max='23' name='rb_h' value='");
  page += String(cfg.reboot_hour);
  page += F("' style='width:90px'>"
            "<span>:</span>"
            "<input type='number' min='0' max='59' name='rb_m' value='");
  page += String(cfg.reboot_min);
  page += F("' style='width:90px'>"
            "</div>"
            "<div class='muted' id='rb_local'></div>"
            "<div class='muted'>Uses the device clock from NTP. Times are UTC unless a timezone is configured in firmware.</div>"
            "</div></div>");

  page += F("<div class='row'><div><label>Weekly day</label><select name='rb_wd'>"
            "<option value='0' ");
  page += (cfg.reboot_wday==0 ? "selected" : "");
  page += F(">Sun</option><option value='1' ");
  page += (cfg.reboot_wday==1 ? "selected" : "");
  page += F(">Mon</option><option value='2' ");
  page += (cfg.reboot_wday==2 ? "selected" : "");
  page += F(">Tue</option><option value='3' ");
  page += (cfg.reboot_wday==3 ? "selected" : "");
  page += F(">Wed</option><option value='4' ");
  page += (cfg.reboot_wday==4 ? "selected" : "");
  page += F(">Thu</option><option value='5' ");
  page += (cfg.reboot_wday==5 ? "selected" : "");
  page += F(">Fri</option><option value='6' ");
  page += (cfg.reboot_wday==6 ? "selected" : "");
  page += F(">Sat</option></select>"
            "<div class='muted'>Used only for Weekly mode.</div>"
            "</div>"
            "<div><label>Monthly day (1-31)</label><input type='number' min='1' max='31' name='rb_md' value='");
  page += String(cfg.reboot_mday);
  page += F("'>"
            "<div class='muted'>Used only for Monthly mode.</div>"
            "</div></div>");

  
  // Locate mode
  page += F("<div class='row'><div><label>Locate device</label>"
            "<button id='locateBtn' type='button' class='smallBtn'>");
  page += (locateMode ? "On" : "Off");
  page += F("</button><div class='muted'>Blinks purple to help find the device.</div></div><div></div></div>");

  page += F("</div>"); // end Maintenance section

  if (portalRunning) page += F("<button type='submit'>Save & Restart</button></form>");
  else page += F("<button type='submit'>Save</button></form>");

  page += F("<p><a href='/update'>Firmware update</a></p>"
            "<p><a href='/backup'>Backup</a> &nbsp;|&nbsp; <a href='/restore'>Restore</a> &nbsp;|&nbsp; <a href='/files'>SD files</a></p>");

page += F("<form method='post' action='/reboot' onsubmit=\"return confirm('Reboot device?');\">"
            "<button type='submit'>Reboot</button></form>");
  page += F("<form method='post' action='/factory_reset' onsubmit=\"return confirm('Factory reset will erase all saved settings and reboot. Continue?');\">"
            "<button type='submit'>Factory Reset</button></form>");
  page += F("<p style='margin-top:14px'><a href='/status'>Back to Status</a></p>");
  page += F("</div>");
  page += F("<script>"
          "(function(){"
          "function pad2(n){n=Number(n)||0;return (n<10?'0':'')+n;}"
          "function update(){"
          "var h=document.querySelector(\"input[name='rb_h']\");"
          "var m=document.querySelector(\"input[name='rb_m']\");"
          "var out=document.getElementById('rb_local');"
          "if(!h||!m||!out) return;"
          "var uh=parseInt(h.value||'0',10);"
          "var um=parseInt(m.value||'0',10);"
          "var d=new Date(Date.UTC(2000,0,1,uh,um,0));"
          "var tzEl=document.getElementById('tzName');var tz=(tzEl&&tzEl.dataset)?(tzEl.dataset.tz||''):'';var loc='';var dayShift=0;try{var fmt=new Intl.DateTimeFormat(undefined,{timeZone:tz||undefined,hour:'2-digit',minute:'2-digit'});loc=fmt.format(d);var dayStr=new Intl.DateTimeFormat('en-US',{timeZone:tz||undefined,day:'2-digit'}).format(d);var dd=parseInt(dayStr,10);if(dd===31) dayShift=-1; else if(dd===2) dayShift=1; else dayShift=0;}catch(e){loc=d.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'});dayShift=d.getDate()-1;}"
          "/* dayShift computed above */"
          "var shiftTxt='';"
          "if(dayShift<0) shiftTxt=' (prev day local)';"
          "else if(dayShift>0) shiftTxt=' (next day local)';"
          "out.textContent='Local time: '+loc+shiftTxt;"
          "}"
          "document.addEventListener('input', function(e){"
          "if(e.target && (e.target.name==='rb_h' || e.target.name==='rb_m')) update();"
          "});"
          "update();"
          "var lb=document.getElementById('locateBtn');"
          "if(lb){lb.addEventListener('click', async function(){try{"
          "var on=(lb.textContent||'').toLowerCase().indexOf('on')>=0;"
          "var want=on?0:1;"
          "lb.disabled=true;"
          "var r=await fetch('/locate?enable='+want,{method:'POST'});" 
          "var j=await r.json();"
          "lb.textContent=(j.locate?'On':'Off');"
          "}catch(e){} lb.disabled=false;});}"
          "})();"
          "</script>");
page += htmlFooter();
  web.send(200, "text/html", page);
}

// -----------------------------
// WiFi scan + profiles (Web UI)
// -----------------------------
static void webSendRedirect(const char* loc);
static const char* wifiAuthToStr(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa+wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-ent";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2+wpa3";
    case WIFI_AUTH_WAPI_PSK: return "wapi";
    default: return "unknown";
  }
}

static void webHandleWifiScanJson() {
  if (!requireAuthOrPortal()) return;

  // Cache scan results so the browser can't hammer the radio/CPU.
  static String cached;
  static uint32_t cachedAtMs = 0;
  const uint32_t nowMs = millis();
  if (cached.length() && (uint32_t)(nowMs - cachedAtMs) < 5000) {
    web.send(200, "application/json", cached);
    return;
  }

  // Scanning pauses radio TX for a moment; keep it short and return quickly.
  WiFi.scanDelete();
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  struct Net { String ssid; int rssi; wifi_auth_mode_t auth; };
  std::vector<Net> nets;
  if (n > 0) {
    nets.reserve((size_t)n);
    for (int i=0;i<n;i++) {
      Net x{ WiFi.SSID(i), WiFi.RSSI(i), WiFi.encryptionType(i) };
      // Skip empty SSIDs (hidden) from the list; user can still type manually.
      if (x.ssid.length() == 0) continue;
      nets.push_back(x);
    }
  }
  WiFi.scanDelete();
  std::sort(nets.begin(), nets.end(), [](const Net& a, const Net& b){ return a.rssi > b.rssi; });

  JsonDocument doc;
  JsonArray arr = doc.createNestedArray("networks");
  size_t lim = std::min<size_t>(nets.size(), 30);
  for (size_t i=0;i<lim;i++) {
    JsonObject o = arr.createNestedObject();
    o["ssid"] = nets[i].ssid;
    o["rssi"] = nets[i].rssi;
    o["sec"] = wifiAuthToStr(nets[i].auth);
  }
  String out;
  serializeJson(doc, out);

  cached = out;
  cachedAtMs = nowMs;
  web.send(200, "application/json", out);
}

static void webRenderWifiPage() {
  if (!requireAuthOrPortal()) return;

  String page = htmlHeader("WiFi Profiles");
  page += F("<h2>WiFi</h2><p class='muted'>Manage saved WiFi profiles. <b>Priority</b>: higher connects first. On boot the device chooses the highest-priority saved network that is currently visible (tie-break by signal). <b>Last OK</b> marks the last SSID that connected successfully; it is preferred only when priorities tie.</p>");
  if (portalRunning) page += F("<p><a href='/config'>Config</a> &nbsp;|&nbsp; <b>WiFi</b></p>");
  else page += F("<p><a href='/status'>Status</a> &nbsp;|&nbsp; <a href='/config'>Config</a> &nbsp;|&nbsp; <b>WiFi</b></p>");

  // Current connection
  page += F("<div class='section'><h3>Current</h3>");
  if (WiFi.isConnected()) {
    page += F("<div class='row'><div><label>Connected SSID</label><input readonly value='");
    page += htmlEscape(WiFi.SSID());
    page += F("'></div><div><label>RSSI</label><input readonly value='");
    page += String((long)WiFi.RSSI());
    page += F(" dBm'></div></div>");
    page += F("<div class='row'><div><label>IP</label><input readonly value='");
    page += htmlEscape(WiFi.localIP().toString());
    page += F("'></div><div></div></div>");
  } else {
    page += F("<p class='muted'>Not connected.</p>");
  }
  page += F("</div>");

  // Profiles table
  page += F("<div class='section'><h3>Saved profiles</h3>");
  if (wifiProfiles.empty()) {
    page += F("<p class='muted'>No profiles saved yet.</p>");
  } else {
    page += F("<div class='muted' style='margin-bottom:8px'>Tip: Larger priority wins. If two profiles have the same priority, the stronger signal wins.</div>");
    page += F("<table style='width:100%;border-collapse:collapse'>"
              "<tr><th style='text-align:left;padding:8px 6px;border-bottom:1px solid #333'>SSID</th>"
              "<th style='text-align:left;padding:8px 6px;border-bottom:1px solid #333'>Priority</th>"
              "<th style='text-align:left;padding:8px 6px;border-bottom:1px solid #333'>Password</th>"
              "<th style='text-align:left;padding:8px 6px;border-bottom:1px solid #333'>Actions</th></tr>");
    for (const auto& p : wifiProfiles) {
      page += F("<tr>");
      page += F("<td style='padding:8px 6px;vertical-align:top'>");
      page += htmlEscape(p.ssid);
      if (wifiLastSsid.length() && p.ssid == wifiLastSsid) {
        page += F(" <span title='Last successful connection (wifi_last). Used only when priorities tie.' style='display:inline-block;padding:2px 6px;border:1px solid #555;border-radius:999px;font-size:12px;opacity:.9'>Last OK</span>");
      }
      if (WiFi.isConnected() && p.ssid == WiFi.SSID()) {
        page += F(" <span style='display:inline-block;padding:2px 6px;border:1px solid #2a7;border-radius:999px;font-size:12px;opacity:.9'>Connected</span>");
      }
      page += F("</td>");
      page += F("<td style='padding:8px 6px;vertical-align:top'>"
                "<form method='post' action='/wifi/profile/save' style='display:flex;gap:8px;align-items:center'>"
                "<input name='ssid' readonly style='width:0;opacity:0;position:absolute' value='");
      page += htmlEscape(p.ssid);
      page += F("'>"
                "<input name='prio' type='number' min='-999' max='999' style='max-width:110px' value='");
      page += String((int)p.prio);
      page += F("'>"
                "<button type='button' class='smallBtn' style='padding:2px 6px;min-width:auto' onclick='nudgePrio(this,10)'>▲</button>"
                "<button type='button' class='smallBtn' style='padding:2px 6px;min-width:auto' onclick='nudgePrio(this,-10)'>▼</button>"
                "</td><td style='padding:8px 6px;vertical-align:top'>"
                "<input name='pass' type='password' value='");
      page += htmlEscape(p.pass);
      page += F("' placeholder='(leave blank for open)'>"
                "</td><td style='padding:8px 6px;vertical-align:top;white-space:nowrap'>"
                "<button type='submit' class='smallBtn'>Save</button>"
                "</form>"
                "<form method='post' action='/wifi/profile/connect' style='display:inline' onsubmit=\"return confirm('Connect to this WiFi now?');\">"
                "<input name='ssid' type='hidden' value='");
      page += htmlEscape(p.ssid);
      page += F("'>"
                "<button type='submit' class='smallBtn'>Connect</button>"
                "</form> "
                "<form method='post' action='/wifi/profile/delete' style='display:inline' onsubmit=\"return confirm('Delete this WiFi profile?');\">"
                "<input name='ssid' type='hidden' value='");
      page += htmlEscape(p.ssid);
      page += F("'>"
                "<button type='submit' class='smallBtn'>Delete</button>"
                "</form>"
                "</td>");
      page += F("</tr>");
    }
    page += F("</table>");
  }
  page += F("</div>");

  // Add profile
  page += F("<div class='section'><h3>Add profile</h3>");
  page += F("<div class='row'><div><label>SSID</label>"
            "<div style='display:flex;gap:8px;align-items:center'>"
            "<input id='newSsid' name='ssid' form='addProf' style='flex:1'>"
            "<button type='button' id='wifiScanBtn2' class='smallBtn'>Scan</button>"
            "</div>"
            "<select id='wifiScanList2' style='margin-top:8px;display:none'></select>"
            "<div class='muted' id='wifiScanMsg2' style='min-height:18px'></div>"
            "</div><div><label>Priority</label><input name='prio' form='addProf' type='number' min='-999' max='999' value='100'></div></div>");
  page += F("<form id='addProf' method='post' action='/wifi/profile/save'>"
            "<label>Password</label><input name='pass' type='password' value='' placeholder='(leave blank for open)'>"
            "<button type='submit'>Save profile</button>"
            "</form>");
  page += F("</div>");

  page += F(
    "<script>\n"
    "window.nudgePrio = function(btn, delta){\n"
    "  try{\n"
    "    const form = btn.closest('form'); if(!form) return;\n"
    "    const inp = form.querySelector('input[name=prio]'); if(!inp) return;\n"
    "    const cur = parseInt(inp.value||'0',10)||0; inp.value = cur + (parseInt(delta,10)||0);\n"
    "    if(form.requestSubmit) form.requestSubmit(); else form.submit();\n"
    "  }catch(e){}\n"
    "};\n"
    "(function(){\n"
    "  const btn=document.getElementById('wifiScanBtn2');\n"
    "  const sel=document.getElementById('wifiScanList2');\n"
    "  const msg=document.getElementById('wifiScanMsg2');\n"
    "  const inSsid=document.getElementById('newSsid');\n"
    "  if(!btn||!sel||!msg||!inSsid) return;\n"
    "  function bars(rssi){ const v=Math.max(0,Math.min(4,Math.round((rssi+90)/12))); return ' ' + '▂▃▄▆█'.slice(0,v+1); }\n"
    "  btn.addEventListener('click', async ()=>{\n"
    "    btn.disabled=true; msg.textContent='Scanning...'; sel.style.display='none';\n"
    "    try{\n"
    "      const r=await fetch('/wifi/scan.json',{cache:'no-store',credentials:'include'});\n"
    "      if(!r.ok){ msg.textContent='Scan failed (HTTP '+r.status+')'; btn.disabled=false; return; }\n"
    "      const j=await r.json();\n"
    "      const nets=(j&&j.networks)||[]; sel.innerHTML='';\n"
    "      if(!nets.length){ msg.textContent='No networks found.'; btn.disabled=false; return; }\n"
    "      for(const n of nets){\n"
    "        const o=document.createElement('option'); o.value=n.ssid||'';\n"
    "        const sec=(n.sec && n.sec!=='open') ? ' 🔒' : '';\n"
    "        o.textContent=(n.ssid||'(hidden)')+'  '+(n.rssi||0)+' dBm'+bars(n.rssi)+sec;\n"
    "        sel.appendChild(o);\n"
    "      }\n"
    "      sel.style.display='block'; msg.textContent='Select a network to fill the SSID.';\n"
    "    }catch(e){ msg.textContent='Scan failed.'; }\n"
    "    btn.disabled=false;\n"
    "  });\n"
    "  sel.addEventListener('change', ()=>{ if(sel.value) inSsid.value=sel.value; });\n"
    "})();\n"
    "</script>\n"
  );

  page += F("<p><a href='/config'>Back to Config</a></p>");
  page += htmlFooter();
  web.send(200, "text/html", page);
}

static void webHandleWifiProfileSave() {
  if (!requireAuthOrPortal()) return;
  String ssid = web.arg("ssid");
  String pass = web.arg("pass");
  int16_t prio = (int16_t)constrain(web.arg("prio").toInt(), -999, 999);
  if (ssid.length() == 0) { web.send(400, "text/plain", "Missing ssid"); return; }
  // If profile list is empty but legacy cfg has a value, ensure migration happened.
  if (wifiProfiles.empty() && cfg.wifi_ssid.length()) wifiProfilesMigrateLegacy();
  wifiProfilesUpsert(ssid, pass, prio, /*keepExistingPrioIfPresent=*/false);

  // Do NOT mirror into cfg.wifi_ssid/pass here.
  // The Config page should reflect the *connected* SSID, and cfg.wifi_ssid/pass
  // acts as a legacy/quick-config mirror only when saved from /config or when
  // explicitly connecting.
  saveConfig();

  webSendRedirect("/wifi");
}

static void webHandleWifiProfileDelete() {
  if (!requireAuthOrPortal()) return;
  String ssid = web.arg("ssid");
  if (ssid.length() == 0) { webSendRedirect("/wifi"); return; }
  wifiProfilesDelete(ssid);
  // Do not modify legacy cfg mirror here; profiles are the real source of truth.
  saveConfig();
  webSendRedirect("/wifi");
}

static void webHandleWifiProfileConnect() {
  if (!requireAuthOrPortal()) return;
  String ssid = web.arg("ssid");
  WifiProfile* p = wifiProfileBySsid(ssid);
  if (!p) { web.send(404, "text/plain", "No such profile"); return; }

  // Mirror into cfg for UI/backups and attempt connect.
  cfg.wifi_ssid = p->ssid;
  cfg.wifi_pass = p->pass;
  saveConfig();

  // If in portal mode, leave AP running; STA can connect in the background.
  WiFi.disconnect(true, true);
  delay(50);
  if (portalRunning) WiFi.mode(WIFI_AP_STA);
  else WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(p->ssid.c_str(), p->pass.c_str());

  String page = htmlHeader("Connecting");
  page += F("<h2>Connecting...</h2><p class='muted'>Trying to connect to <b>");
  page += htmlEscape(p->ssid);
  page += F("</b>. This may take a few seconds.</p>"
            "<script>\n"
            "const start=Date.now();\n"
            "async function poll(){\n"
            "  try{\n"
            "    const r=await fetch('/status.json',{cache:'no-store',credentials:'include'});\n"
            "    if(r.ok){\n"
            "      const s=await r.json();\n"
            "      if(s && s.ip && s.ip.length){ location.href='/status'; return; }\n"
            "    }\n"
            "  }catch(e){}\n"
            "  if(Date.now()-start>15000) location.href='/wifi';\n"
            "}\n"
            "setInterval(poll,500); poll();\n"
            "</script>\n");
  page += htmlFooter();
  web.send(200, "text/html", page);
}



// -----------------------------
// SD backup UI / file browser
// -----------------------------
static String humanSize(uint64_t bytes) {
  const char* suff[] = {"B","KB","MB","GB"};
  double b = (double)bytes;
  int i=0;
  while (b >= 1024.0 && i < 3) { b /= 1024.0; i++; }
  char buf[24];
  if (i==0) snprintf(buf, sizeof(buf), "%llu %s", (unsigned long long)bytes, suff[i]);
  else snprintf(buf, sizeof(buf), "%.1f %s", b, suff[i]);
  return String(buf);
}

static void webSendRedirect(const char* loc) {
  web.sendHeader("Location", loc);
  web.send(302, "text/plain", "");
}

static void webRenderBackupPage() {
  if (!requireAuthOrPortal()) return;
  // Attempt NTP sync (once) so the page reflects accurate time without requiring a refresh.
  timeSyncOnce();
  std::vector<String> files;
  listBackupFiles(files);

  String page = htmlHeader("Backup");
  page += F("<h2>Backup</h2><p class='muted'>Create and manage backups on the SD card, or download the current configuration directly.</p>");
  if (portalRunning) page += F("<p><a href='/config'>Config</a> &nbsp;|&nbsp; <b>Backup</b> &nbsp;|&nbsp; <a href='/restore'>Restore</a></p>");
  else page += F("<p><a href='/status'>Status</a> &nbsp;|&nbsp; <a href='/config'>Config</a> &nbsp;|&nbsp; <b>Backup</b> &nbsp;|&nbsp; <a href='/restore'>Restore</a></p>");

  // You can always download the current config, even without an SD card.
  page += F("<p><a class='smallBtn' href='/backup/download_current'>Download current config</a> <span class='muted'>No SD required.</span></p>");

  // Optional message
  if (web.hasArg("msg")) {
    page += F("<p class='ok'>");
    page += htmlEscape(web.arg("msg"));
    page += F("</p>");
  }

  if (!sdBegin()) {
    page += F("<p class='muted'>SD not available for SD backups.</p>");
  } else {
    page += F("<form method='post' action='/backup/create'>"
              "<button type='submit'>Create backup now</button></form>");

    if (!timeInited) {
      page += F("<p class='muted'>Note: device time is not set yet; filenames may use a sequence number until WiFi time sync occurs.</p>");
    }

    page += F("<h3>Available backups</h3>");
    if (files.empty()) {
      page += F("<p class='muted'>No backups found in /backups.</p>");
    } else {
      page += F("<table class='table'><tr><th>File</th><th>Size</th><th>Actions</th></tr>");
      for (auto &name : files) {
        String path = backupPathFor(name);
        uint64_t sz = 0;
        File f = SD_MMC.open(path.c_str(), FILE_READ);
        if (f) { sz = f.size(); f.close(); }

        page += F("<tr><td><code>");
        page += htmlEscape(name);
        page += F("</code></td><td>");
        page += humanSize(sz);
        page += F("</td><td>");
        page += F("<form style='display:inline' method='post' action='/restore'>");
        page += F("<input type='hidden' name='file' value='");
        page += htmlEscape(name);
        page += F("'><button type='submit' style='margin-top:0'>Restore</button></form> ");

        page += F("<a class='smallBtn' href='/files/download?file=");
        page += urlEncode(name);
        page += F("'>Download</a> ");

        page += F("<form style='display:inline' method='post' action='/backup/delete' onsubmit=\"return confirm('Delete this backup?');\">");
        page += F("<input type='hidden' name='file' value='");
        page += htmlEscape(name);
        page += F("'><button type='submit' class='danger' style='margin-top:0'>Delete</button></form>");

        page += F("</td></tr>");
      }
      page += F("</table>");
    }
  }

  page += F("<p><a href='/files'>Open SD file browser</a></p>");
  page += htmlFooter();
  // Prevent browsers from caching the page (time-sync banner can become stale quickly).
  web.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  web.sendHeader("Pragma", "no-cache");
  web.sendHeader("Expires", "0");
  web.send(200, "text/html", page);
}

static void webHandleBackupCreate() {
  if (!requireAuthOrPortal()) return;
  bool ok = false;
  if (ensureBackupDir()) {
    String name = makeBackupName();
    ok = sdBackupConfigToFile(backupPathFor(name));
  }
  if (!ok) {
    web.send(500, "text/plain", "SD not available\n");
    return;
  }
  webSendRedirect("/backup");
}


static void webHandleBackupDownloadCurrent() {
  if (!requireAuthOrPortal()) return;

  // Build the same JSON used for SD backups, but stream it directly (no SD required)
  JsonDocument doc;
  buildBackupJson(doc);

  String payload;
  serializeJson(doc, payload);

  String fname = makeBackupName();
  if (!fname.endsWith(".json")) fname += ".json";

  web.sendHeader("Content-Type", "application/json");
  web.sendHeader("Content-Disposition", String("attachment; filename=\"") + fname + "\"");
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", payload);
}

static void webHandleBackupDelete() {
  if (!requireAuthOrPortal()) return;
  String name = web.arg("file");
  if (!isSafeBackupName(name)) { web.send(400, "text/plain", "Bad file\n"); return; }
  if (!sdBegin()) { web.send(500, "text/plain", "SD not available\n"); return; }
  String path = backupPathFor(name);
  if (SD_MMC.exists(path.c_str())) SD_MMC.remove(path.c_str());
  webSendRedirect("/backup");
}

static void webRenderRestorePage() {
  if (!requireAuthOrPortal()) return;
  std::vector<String> files;
  listBackupFiles(files);

  String page = htmlHeader("Restore");
  page += F("<h2>Restore</h2><p class='muted'>Restore settings from an SD backup or an uploaded JSON file (saved to NVS).</p>");
  if (portalRunning) page += F("<p><a href='/config'>Config</a> &nbsp;|&nbsp; <a href='/backup'>Backup</a> &nbsp;|&nbsp; <b>Restore</b></p>");
  else page += F("<p><a href='/status'>Status</a> &nbsp;|&nbsp; <a href='/config'>Config</a> &nbsp;|&nbsp; <a href='/backup'>Backup</a> &nbsp;|&nbsp; <b>Restore</b></p>");

  // Optional message
  if (web.hasArg("msg")) {
    page += F("<p class='ok'>");
    page += htmlEscape(web.arg("msg"));
    page += F("</p>");
  }

  // Upload a backup file directly (no SD required)
  page += F("<h3>Upload backup file</h3>"
            "<p class='muted'>Upload a previously downloaded backup JSON to restore settings (saved to NVS).</p>"
            "<form method='post' action='/restore/upload' enctype='multipart/form-data' onsubmit=\"return confirm('Restore settings from uploaded file?');\">"
            "<input type='file' name='file' accept='.json,application/json' required>"
            "<button type='submit'>Upload and restore</button>"
            "</form>");

  if (!sdBegin()) {
    page += F("<p class='muted'>SD not available.</p>");
  } else if (files.empty()) {
    page += F("<p class='muted'>No backups found in /backups.</p>");
    page += F("<p><a href='/backup'>Create a backup</a></p>");
  } else {
    page += F("<form method='post' action='/restore' onsubmit=\"return confirm('Restore this backup and reboot?');\">"
              "<label>Backup file</label><select name='file'>");
    for (auto &name : files) {
      page += F("<option value='");
      page += htmlEscape(name);
      page += F("'>");
      page += htmlEscape(name);
      page += F("</option>");
    }
    page += F("</select><button type='submit'>Restore selected</button></form>");
  }

  page += htmlFooter();
  web.send(200, "text/html", page);
}

static void webHandleRestoreSelected() {
  if (!requireAuthOrPortal()) return;
  String name = web.arg("file");
  bool ok = false;
  if (isSafeBackupName(name)) {
    ok = sdRestoreConfigFromFile(backupPathFor(name));
  } else {
    // Back-compat: allow old /nukaminer.json workflow
    ok = sdRestoreConfig();
  }

  if (!ok) { web.send(500, "text/plain", "Restore failed\n"); return; }

  // After restoring settings, reboot to apply everything cleanly.
  web.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='5;url=/status'></head><body><h2>Restored!</h2><p>Rebooting...</p></body></html>");
  delay(800);
  ESP.restart();
}

// SD File manager (full)
// -----------------------------

static bool isSafePath(const String& in, String& out) {
  out = in;
  if (out.length() == 0) out = "/";
  out.replace("\\", "/");
  if (!out.startsWith("/")) out = "/" + out;
  // Disallow traversal and control chars
  if (out.indexOf("..") >= 0) return false;
  for (size_t i = 0; i < out.length(); i++) {
    char c = out[i];
    if (c < 32) return false;
  }
  // Collapse repeated slashes
  while (out.indexOf("//") >= 0) out.replace("//", "/");
  // Trim trailing slash (except root)
  if (out.length() > 1 && out.endsWith("/")) out.remove(out.length() - 1);
  return true;
}

static String parentDirOf(const String& pathIn) {
  String path;
  if (!isSafePath(pathIn, path)) return "/";
  if (path == "/") return "/";
  int slash = path.lastIndexOf('/');
  if (slash <= 0) return "/";
  return path.substring(0, slash);
}

static bool sdRemoveRecursive(const String& pathIn) {
  String path;
  if (!isSafePath(pathIn, path)) return false;
  if (path == "/") return false; // never

  File f = SD_MMC.open(path.c_str());
  if (!f) return false;
  bool isDir = f.isDirectory();
  f.close();

  if (!isDir) {
    return SD_MMC.remove(path.c_str());
  }

  File d = SD_MMC.open(path.c_str());
  if (!d || !d.isDirectory()) { if (d) d.close(); return false; }

  File entry = d.openNextFile();
  while (entry) {
    String child = String(path) + "/" + String(entry.name());
    bool childDir = entry.isDirectory();
    entry.close();
    if (childDir) {
      if (!sdRemoveRecursive(child)) { d.close(); return false; }
    } else {
      if (!SD_MMC.remove(child.c_str())) { d.close(); return false; }
    }
    entry = d.openNextFile();
  }
  d.close();
  return SD_MMC.rmdir(path.c_str());
}

static void webRenderFilesPage() {
  if (!requireAuthOrPortal()) return;

  String curRaw = web.arg("path");
  String cur;
  if (!isSafePath(curRaw, cur)) cur = "/";

  String page = htmlHeader("SD Files");
  page += F("<h2>SD Files</h2><p class='muted'>Full SD card file manager. Backup/Restore and Firmware pages filter by extension, but this page shows everything.</p>");
  if (portalRunning) page += F("<p><a href='/config'>Config</a> &nbsp;|&nbsp; <a href='/backup'>Backup</a> &nbsp;|&nbsp; <a href='/restore'>Restore</a> &nbsp;|&nbsp; <b>SD Files</b></p>");
  else page += F("<p><a href='/status'>Status</a> &nbsp;|&nbsp; <a href='/config'>Config</a> &nbsp;|&nbsp; <a href='/backup'>Backup</a> &nbsp;|&nbsp; <a href='/restore'>Restore</a> &nbsp;|&nbsp; <b>SD Files</b></p>");

  if (!sdBegin()) {
    page += F("<p class='muted'>SD not available.</p>");
    page += htmlFooter();
    web.send(200, "text/html", page);
    return;
  }

  // Breadcrumbs
  page += F("<div class='muted'>Path: ");
  page += F("<a href='/files?path=%2F'>/</a>");
  if (cur != "/") {
    String accum;
    int start = 1;
    while (start < (int)cur.length()) {
      int slash = cur.indexOf('/', start);
      String part;
      if (slash < 0) { part = cur.substring(start); start = cur.length(); }
      else { part = cur.substring(start, slash); start = slash + 1; }
      if (part.length() == 0) continue;
      accum += "/" + part;
      page += F(" / <a href='/files?path=");
      page += urlEncode(accum);
      page += F("'>");
      page += htmlEscape(part);
      page += F("</a>");
    }
  }
  page += F("</div>");

  // Controls
  page += F("<div class='row'>");
  page += F("<form method='post' action='/files/mkdir' style='flex:1'>"
            "<input type='hidden' name='path' value='");
  page += htmlEscape(cur);
  page += F("'>"
            "<label>New folder</label>"
            "<input name='name' placeholder='folder-name'>"
            "<button type='submit'>Create</button>"
            "</form>");

  // Upload w/ progress (XHR) to avoid confusing browser resets on long SD writes.
  page += F("<div style='flex:1'>"
            "<label>Upload file</label>"
            "<div class='muted' style='margin-top:4px'>Note: uploads/downloads temporarily pause mining during the transfer due to device limitations.</div>"
            "<input type='file' id='upFile' required>"
            "<div class='row' style='align-items:center'>"
              "<button id='upBtn' type='button'>Upload</button>"
              "<div style='flex:1'>"
                "<div style='height:10px;border:1px solid #2b3240;border-radius:10px;overflow:hidden'>"
                  "<div id='upBar' style='height:10px;width:0%'></div>"
                "</div>"
                "<div id='upMsg' class='muted' style='margin-top:6px'></div>"
              "</div>"
            "</div>"
            "</div>");
  page += F("</div>");

  // Upload script
  page += F("<script>\n"
            "(function(){\n"
            "  const btn=document.getElementById('upBtn');\n"
            "  const fileEl=document.getElementById('upFile');\n"
            "  const bar=document.getElementById('upBar');\n"
            "  const msg=document.getElementById('upMsg');\n"
	            "  const target='/files/upload?path=");
  page += urlEncode(cur);
  page += F("';\n"
	            "  const listUrl='/files/list.json?path=");
  page += urlEncode(cur);
  page += F("';\n"
            "  function setMsg(t){msg.textContent=t||'';}\n"
            "  function warnOnce(){\n"
            "    try{ if(localStorage.getItem('nmXferWarned')==='1') return true; }catch(e){}\n"
            "    if(!confirm('Uploads and downloads temporarily pause mining during the transfer due to device limitations. Continue?')) return false;\n"
            "    try{ localStorage.setItem('nmXferWarned','1'); }catch(e){}\n"
            "    return true;\n"
            "  }\n"
            "  async function nameExists(n){\n"
            "    try{\n"
            "      const r=await fetch(listUrl,{cache:'no-store'});\n"
            "      if(!r.ok) return false;\n"
            "      const j=await r.json();\n"
            "      for(const e of (j.entries||[])){ if(e && e.name===n) return true; }\n"
            "    }catch(e){}\n"
            "    return false;\n"
            "  }\n"
            "  document.addEventListener('click',(ev)=>{\n"
            "    const a=ev.target && ev.target.closest ? ev.target.closest('a[data-dl]') : null;\n"
            "    if(!a) return;\n"
            "    if(!warnOnce()){ ev.preventDefault(); }\n"
            "  });\n"
            "  btn.addEventListener('click', ()=>{\n"
            "    const f=fileEl.files&&fileEl.files[0];\n"
            "    if(!f){setMsg('Choose a file first.');return;}\n"
            "    (async()=>{\n"
            "      if(!warnOnce()){ setMsg('Upload cancelled.'); return; }\n"
            "      if(await nameExists(f.name)){\n"
            "        if(!confirm('File '+f.name+' already exists in this folder. Overwrite it?')){\n"
            "          setMsg('Upload cancelled.');\n"
            "          return;\n"
            "        }\n"
            "      }\n"
            "    btn.disabled=true;\n"
            "    bar.style.width='0%';\n"
            "    setMsg('Uploading...');\n"
            "    const fd=new FormData();\n"
            "    fd.append('file', f, f.name);\n"
            "    const xhr=new XMLHttpRequest();\n"
            "    xhr.open('POST', target, true);\n"
            "    xhr.upload.onprogress=(e)=>{\n"
            "      if(e.lengthComputable){\n"
            "        const p=Math.floor((e.loaded/e.total)*100);\n"
            "        bar.style.width=p+'%';\n"
            "        setMsg('Uploading... '+p+'% ('+Math.floor(e.loaded/1024)+' KB)');\n"
            "      }\n"
            "    };\n"
            "    xhr.onload=()=>{\n"
            "      btn.disabled=false;\n"
            "      if(xhr.status>=200 && xhr.status<300){\n"
            "        bar.style.width='100%';\n"
            "        setMsg('Upload complete. Refreshing...');\n"
            "        // Force reload without cache so the new file shows immediately.\n"
            "        setTimeout(()=>{ window.location.href = window.location.pathname + window.location.search; }, 350);\n"
            "      } else {\n"
            "        setMsg('Upload failed: HTTP '+xhr.status+' '+xhr.responseText);\n"
            "      }\n"
            "    };\n"
            "    xhr.onerror=()=>{btn.disabled=false;setMsg('Upload failed: connection error');};\n"
            "    xhr.send(fd);\n"
            "    })();\n"
            "  });\n"
            "})();\n"
            "</script>\n");

  File dir = SD_MMC.open(cur.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    page += F("<p class='muted'>Not a directory.</p>");
    page += htmlFooter();
    web.send(200, "text/html", page);
    return;
  }

  // Parent link
  if (cur != "/") {
    int s = cur.lastIndexOf('/');
    String parent = (s <= 0) ? "/" : cur.substring(0, s);
    page += F("<p><a href='/files?path=");
    page += urlEncode(parent);
    page += F("'>&larr; Up</a></p>");
  }

  struct Entry { String name; bool isDir; uint64_t size; };
  std::vector<Entry> ents;

  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    Entry en;
    en.name = String(e.name());
    en.isDir = e.isDirectory();
    en.size = en.isDir ? 0 : (uint64_t)e.size();
    ents.push_back(en);
    e.close();
  }
  dir.close();

  // Sort: dirs first, then files; alpha by name
  std::sort(ents.begin(), ents.end(), [](const Entry& a, const Entry& b){
    if (a.isDir != b.isDir) return a.isDir > b.isDir;
    return a.name < b.name;
  });

  page += F("<table class='table'><tr><th>Name</th><th>Type</th><th>Size</th><th>Actions</th></tr>");
  for (auto &e : ents) {
    String full = (cur == "/") ? (String("/") + e.name) : (cur + "/" + e.name);
    page += F("<tr><td>");
    if (e.isDir) {
      page += F("<a href='/files?path=");
      page += urlEncode(full);
      page += F("'><code>");
      page += htmlEscape(e.name);
      page += F("/</code></a>");
    } else {
      page += F("<code>");
      page += htmlEscape(e.name);
      page += F("</code>");
    }
    page += F("</td><td>");
    page += (e.isDir ? "dir" : "file");
    page += F("</td><td>");
    page += (e.isDir ? "" : humanSize(e.size));
    page += F("</td><td>");

    // Rename
    page += F("<form style='display:inline' method='post' action='/files/rename' onsubmit=\"return nmRename(this);\">");
    page += F("<input type='hidden' name='path' value='");
    page += htmlEscape(cur);
    page += F("'>");
    page += F("<input type='hidden' name='old' value='");
    page += htmlEscape(e.name);
    page += F("'>");
    page += F("<input type='hidden' name='new' value=''>");
    page += F("<button type='submit' class='smallBtn' style='margin-top:0'>Rename</button></form> ");

    if (!e.isDir) {
      page += F("<a class='smallBtn' data-dl='1' href='/files/download?path=");
      page += urlEncode(full);
      page += F("'>Download</a> ");
    }

    page += F("<form style='display:inline' method='post' action='/files/delete' onsubmit=\"return confirm('Delete this item?');\">"
              "<input type='hidden' name='path' value='");
    page += htmlEscape(full);
    page += F("'><button type='submit' class='danger' style='margin-top:0'>Delete</button></form>");

    page += F("</td></tr>");
  }
  page += F("</table>");

  
  // Rename helper
  page += F("<script>\n"
            "function nmRename(form){\n"
            "  try{\n"
            "    const old=form.old.value||'';\n"
            "    const nn=prompt('Rename to:', old);\n"
            "    if(!nn || nn===old) return false;\n"
            "    if(nn.includes('/')||nn.includes('\\\\')||nn.includes('..')){alert('Invalid name');return false;}\n"
            "    form.new.value=nn;\n"
            "    return confirm('Rename '+old+' -> '+nn+'?');\n"
            "  }catch(e){return false;}\n"
            "}\n"
            "</script>");

page += htmlFooter();
  web.send(200, "text/html", page);
}

static void webHandleFileDownload() {
  if (!requireAuthOrPortal()) return;
  if (!sdBegin()) { web.send(500, "text/plain", "SD not available\n"); return; }

  // Back-compat: older links used ?file=<backup.json> (download from /backups).
  String raw = web.arg("path");
  if (raw.length() == 0) {
    String name = web.arg("file");
    if (isSafeBackupName(name)) raw = backupPathFor(name);
  }

  String path;
  if (!isSafePath(raw, path)) { web.send(400, "text/plain", "Bad path\n"); return; }

  File f = SD_MMC.open(path.c_str(), FILE_READ);
  if (!f || f.isDirectory()) { if (f) f.close(); web.send(404, "text/plain", "Not found\n"); return; }

  String name = path;
  int slash = name.lastIndexOf('/');
  if (slash >= 0) name = name.substring(slash + 1);

  // Large downloads can stall if the device is busy (mining, SD latency) and the
  // browser will report "connection error". Stream manually with yields and a
  // longer socket timeout.
  sdBusy = true;
  ledService();
  WiFiClient client = web.client();
  client.setTimeout(120000);

  web.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  web.sendHeader("Cache-Control", "no-store");
  web.setContentLength((size_t)f.size());
  web.send(200, "application/octet-stream", "");

  static uint8_t buf[2048];
  while (client.connected()) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    size_t w = client.write(buf, (size_t)n);
    if (w != (size_t)n) {
      NM_log(String("[NukaMiner] SD download short write: wrote=") + String((unsigned long)w) +
             String(" expected=") + String((unsigned long)n));
      break;
    }
    // Keep WiFi + watchdog happy
    delay(0);
  }
  f.close();
  sdBusy = false;
  ledService();
}

static void webHandleSaveSettings() {
  if (!requireAuthOrPortal()) return;

  // Snapshot old values so we can decide whether a reboot is required.
  const String old_wifi_ssid = cfg.wifi_ssid;
  const String old_wifi_pass = cfg.wifi_pass;
  const bool old_web_enabled = cfg.web_enabled;
  const bool old_web_always  = cfg.web_always_on;
  const String old_web_user  = cfg.web_user;
  const String old_web_pass  = cfg.web_pass;
  const bool old_duino_en    = cfg.duino_enabled;
  const bool old_core1_en    = cfg.core1_enabled;
  const bool old_core2_en    = cfg.core2_enabled;
  const uint16_t old_web_to = cfg.web_timeout_s;
  const String old_duco_user = cfg.duco_user;
  const String old_rig_id    = cfg.rig_id;
  const String old_miner_key = cfg.miner_key;
  const String old_ntp_server = cfg.ntp_server;

  {
    // WiFi password field on /config is intentionally not pre-filled.
    // If the user leaves it blank and SSID is unchanged, keep the existing password.
    const String newSsid = web.arg("wifi_ssid");
    String newPass = web.arg("wifi_pass");
    if (newPass.length() == 0 && newSsid == old_wifi_ssid) newPass = old_wifi_pass;
    cfg.wifi_ssid = newSsid;
    cfg.wifi_pass = newPass;
  }
  cfg.duco_user = web.arg("duco_user");
  cfg.rig_id = web.arg("rig_id");
  cfg.miner_key = web.arg("miner_key");
  if (web.hasArg("ntp_server")) cfg.ntp_server = web.arg("ntp_server");
  if (cfg.ntp_server != old_ntp_server) { timeInited = false; }

  if (web.hasArg("tz")) cfg.tz_name = web.arg("tz");

  // Pool lookup cache + scheduled reboot
  if (web.hasArg("pool_cache_s")) {
    long v = web.arg("pool_cache_s").toInt();
    if (v < 0) v = 0;
    if (v > 86400) v = 86400;
    cfg.pool_cache_s = (uint32_t)v;
  }

  if (web.hasArg("rb_mode")) {
    int m = web.arg("rb_mode").toInt();
    if (m < 0) m = 0;
    if (m > 3) m = 3;
    cfg.reboot_mode = (uint8_t)m;
  }
  if (web.hasArg("rb_h")) {
    int h = web.arg("rb_h").toInt();
    if (h < 0) h = 0;
    if (h > 23) h = 23;
    cfg.reboot_hour = (uint8_t)h;
  }
  if (web.hasArg("rb_m")) {
    int mn = web.arg("rb_m").toInt();
    if (mn < 0) mn = 0;
    if (mn > 59) mn = 59;
    cfg.reboot_min = (uint8_t)mn;
  }
  if (web.hasArg("rb_wd")) {
    int wd = web.arg("rb_wd").toInt();
    if (wd < 0) wd = 0;
    if (wd > 6) wd = 6;
    cfg.reboot_wday = (uint8_t)wd;
  }
  if (web.hasArg("rb_md")) {
    int md = web.arg("rb_md").toInt();
    if (md < 1) md = 1;
    if (md > 31) md = 31;
    cfg.reboot_mday = (uint8_t)md;
  }

  cfg.display_sleep_s = (uint32_t) web.arg("disp_sleep").toInt();
  cfg.lcd_brightness = (uint8_t) constrain(web.arg("lcd_br").toInt(), 0, 100);
  cfg.lcd_rot180 = (web.arg("lcd_r180") != "0");
  // Mining speed is system-managed; ignore any legacy limiter inputs.
  cfg.hash_limit_pct = 100;
  // Friendly performance mode selector (new). Keep legacy c1_en/c2_en for backwards compatibility.
  if (web.hasArg("core_mode")) {
    const String mode = web.arg("core_mode");
    cfg.core2_enabled = true;               // Core 2 is always available
    cfg.core1_enabled = (mode == "c12");   // Max Performance uses both cores
  } else {
    cfg.core1_enabled = (web.arg("c1_en") != "0");
    cfg.core2_enabled = (web.arg("c2_en") != "0");
  }
  // Primary miner selection removed from UI; Core 2 is always treated as the primary.
  cfg.primary_core = 2;
  cfg.core2_hash_limit_pct = 100;
  cfg.led_enabled = (web.arg("led_en") != "0");
  cfg.led_brightness = (uint8_t) constrain(web.arg("led_br").toInt(), 0, 100);
  cfg.carousel_enabled = (web.arg("car_en") != "0");
  cfg.carousel_seconds = (uint16_t) std::max<long>(2L, web.arg("car_s").toInt());
  cfg.duino_enabled = (web.arg("duco_en") != "0");
  cfg.web_enabled = (web.arg("web_en") != "0");
  cfg.web_always_on = (web.arg("web_always") != "0");
  if (web.hasArg("web_to")) cfg.web_timeout_s = (uint16_t) std::max<long>(30L, web.arg("web_to").toInt());
  cfg.web_user = web.arg("web_user");
  cfg.web_pass = web.arg("web_pass");

  // Keep WiFi profiles in sync: treat the Config page WiFi fields as a quick
  // profile editor.
  // - If SSID exists: update password, preserve priority.
  // - If SSID is new: append it to the bottom (lowest priority).
  if (cfg.wifi_ssid.length()) {
    int16_t prioForNew = 100;
    if (wifiProfileBySsid(cfg.wifi_ssid) == nullptr) {
      if (!wifiProfiles.empty()) {
        int minPrio = 999;
        for (const auto& p : wifiProfiles) minPrio = std::min(minPrio, (int)p.prio);
        prioForNew = (int16_t)constrain(minPrio - 10, -999, 999);
      }
    }
    wifiProfilesUpsert(cfg.wifi_ssid, cfg.wifi_pass, prioForNew, /*keepExistingPrioIfPresent=*/true);
  }
  saveConfig();

  // If the user turned off "always on", immediately require a physical BOOT press.
  if (!cfg.web_always_on && (old_web_always != cfg.web_always_on || old_web_to != cfg.web_timeout_s)) {
    webSessionActive = false;
    webSessionDeadlineMs = 0;
  }

  // Decide whether we should prompt for reboot. We keep this conservative:
  // - WiFi changes: easiest + most reliable is a reboot
  // - Web auth / enable changes: avoids weird half-applied sessions
  bool needsReboot = false;
  if (cfg.wifi_ssid != old_wifi_ssid || cfg.wifi_pass != old_wifi_pass) needsReboot = true;
  if (cfg.web_enabled != old_web_enabled || cfg.web_always_on != old_web_always) needsReboot = true;
  if (cfg.web_user != old_web_user || cfg.web_pass != old_web_pass) needsReboot = true;
  const bool perfChanged = (cfg.core1_enabled != old_core1_en) || (cfg.core2_enabled != old_core2_en);
  if (perfChanged) needsReboot = true;

  const bool miningToggled = (cfg.duino_enabled != old_duino_en);
  if (miningToggled) needsReboot = true;

  // Apply immediately (no reboot required)
  // If mining was disabled/enabled, we require a reboot for a clean state.
  // However, when disabling, stop miners immediately to honor the setting.
  if (miningToggled && !cfg.duino_enabled) {
    minerStop();
  }

  // Apply the user-selected brightness immediately (cfg already updated + saved above).
  blSet(cfg.lcd_brightness, false);
  NM_hash_limit_pct = cfg.hash_limit_pct; // alias
  NM_hash_limit_pct_job0 = cfg.hash_limit_pct;
  NM_hash_limit_pct_job1 = cfg.core2_enabled ? cfg.core2_hash_limit_pct : 100;
  tft.setRotation(cfg.lcd_rot180 ? 3 : 1);
  ledService();

  // If miner-related settings changed, restart the miner task(s) so changes apply immediately.
// NOTE: Performance mode (core enable/disable) changes can require a full reboot on some builds,
// so we avoid stopping/starting miners here when perfChanged is true. The user will be prompted
// to reboot instead.
  const bool minerConfigChanged =
            (cfg.duco_user != old_duco_user) ||
      (cfg.rig_id != old_rig_id) ||
      (cfg.miner_key != old_miner_key);

  if (minerConfigChanged && !perfChanged && !miningToggled) {
    minerStop();
    delay(50);
    minerStart();
  }

// In AP / Setup mode, apply changes immediately by rebooting after save.
  if (portalRunning) {
    web.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='8;url=/config'></head><body><h2>Saved!</h2><p>Rebooting to apply settings...</p></body></html>");
    delay(800);
    ESP.restart();
    return;
  }

  if (needsReboot) {
    String page = htmlHeader("Reboot required");
    page += F("<h2>Saved</h2>"
              "<p class='muted'>One or more changes require a reboot to take effect.</p>"
              "<div class='row'>"
              "<form method='post' action='/reboot' onsubmit=\"return confirm('Reboot now?');\">"
              "<button type='submit'>Reboot now</button></form>"
              "<div style='display:flex;align-items:flex-end'>"
              "<a href='/config'>Not now</a>"
              "</div>"
              "</div>");
    page += htmlFooter();
    web.send(200, "text/html", page);
    return;
  }

  web.sendHeader("Location", "/settings");
  web.send(303, "text/plain", "Saved");
}

static void webHandleStatusJson() {
  if (!requireAuthOrPortal()) return;

  // Cache the rendered JSON briefly to keep aggressive polling from stealing
  // CPU time from mining. This endpoint is often hit multiple times per second.
  static String cached;
  static uint32_t cachedAtMs = 0;
  const uint32_t nowMs = millis();
  if (cached.length() && (uint32_t)(nowMs - cachedAtMs) < 500) {
    web.send(200, "application/json", cached);
    return;
  }

  // Keep this reasonably small; status.json is polled frequently.
  // Increased slightly as we add a few cheap UI/power-related fields.
  StaticJsonDocument<1280> doc;
  doc["chip"] = String("ESP32-S3");
  doc["fw_name"] = FW_NAME;
  doc["fw_version"] = FW_VERSION;
  doc["fw_channel"] = FW_CHANNEL;
  doc["fw_build"] = FW_BUILD;
  doc["reset_reason"] = (int)g_resetReason;
  doc["heap"] = (uint32_t)ESP.getFreeHeap();
  doc["heap_total"] = (uint32_t)ESP.getHeapSize();

  // Internal temperature sensor (ESP32-S3). Note: accuracy is limited.
  // Arduino-ESP32 exposes temperatureRead() on ESP32 targets.
#if defined(ARDUINO_ARCH_ESP32)
  doc["temp_c"] = (double)temperatureRead();
#endif

  uint32_t up = millis()/1000;
  char upbuf[32];
  snprintf(upbuf, sizeof(upbuf), "%lu:%02lu:%02lu", (unsigned long)(up/3600), (unsigned long)((up%3600)/60), (unsigned long)(up%60));
  doc["uptime"] = upbuf;
  doc["uptime_s"] = (uint32_t)up;

  doc["ssid"] = WiFi.isConnected() ? WiFi.SSID() : "";
  doc["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : "";
  doc["rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
  doc["gw"] = WiFi.isConnected() ? WiFi.gatewayIP().toString() : "";
  doc["dns"] = WiFi.isConnected() ? WiFi.dnsIP(0).toString() : "";
  doc["sn"] = WiFi.isConnected() ? WiFi.subnetMask().toString() : "";
  doc["mac"] = WiFi.macAddress();

  // Time / NTP
  doc["ntp_server"] = cfg.ntp_server;
  time_t nowUtc = time(nullptr);
  // If time is not yet synced, time(nullptr) will be low (often 0).
  doc["utc_unix"] = (uint32_t)((nowUtc > 0) ? nowUtc : 0);
  doc["time_valid"] = (bool)(nowUtc > 1700000000);


  doc["tz"] = cfg.tz_name;
  doc["locate"] = (bool)locateMode;
  doc["device_control"] = (bool)deviceControlMode;

  doc["mining_enabled"] = cfg.duino_enabled;
  doc["user"] = cfg.duco_user;
  doc["rig"] = cfg.rig_id;
  doc["hash_limit_pct"] = cfg.hash_limit_pct;
  doc["core1_enabled"] = cfg.core1_enabled;
  doc["core2_enabled"] = cfg.core2_enabled;
  doc["primary_core"] = cfg.primary_core;
  doc["lcd_polling"] = (bool)webLcdPollingEnabled;
  // UI/power-related signals (cheap to report; used for browser-only power estimates)
  doc["lcd_on"] = (bool)(!displaySleeping);
  doc["lcd_brightness"] = (uint32_t)cfg.lcd_brightness; // 0-100
  doc["led_enabled"] = (bool)cfg.led_enabled;
  doc["led_brightness"] = (uint32_t)cfg.led_brightness; // 0-100
  doc["led_on"] = (bool)(cfg.led_enabled && (cfg.led_brightness > 0) && (ledMode != LED_OFF));
  doc["core2_hash_limit_pct"] = cfg.core2_hash_limit_pct;
  // Expose hashrates in kH/s for UI consistency.
  const double hr1_khs = ((double)(cfg.core1_enabled ? hashrate : 0.0)) / 1000.0;
  const double hr2_khs = ((double)(cfg.core2_enabled ? hashrate_core_two : 0.0)) / 1000.0;
  const double hrt_khs = hr1_khs + hr2_khs;
  doc["hashrate1"] = hr1_khs;
  doc["hashrate2"] = hr2_khs;
  doc["hashrate"]  = hrt_khs;
  doc["hashrate_unit"] = "kH/s";
  doc["difficulty"] = difficulty;
  doc["shares"] = share_count;
  doc["accepted"] = accepted_share_count;
  doc["rejected"] = (share_count >= accepted_share_count) ? (share_count - accepted_share_count) : 0;
  doc["node"] = node_id;

  // Web UI gating status (useful when Web UI always-on is disabled)
  doc["web_enabled"] = cfg.web_enabled;
  doc["web_always_on"] = cfg.web_always_on;
  doc["web_session_active"] = (bool)webSessionActive;
  if (webSessionActive) {
    int32_t leftMs = (int32_t)(webSessionDeadlineMs - nowMs);
    if (leftMs < 0) leftMs = 0;
    doc["web_session_left_s"] = (uint32_t)(leftMs / 1000);
  } else {
    doc["web_session_left_s"] = 0;
  }
  doc["ap_mode"] = (bool)portalRunning;

  cached = "";
  serializeJson(doc, cached);
  cachedAtMs = nowMs;
  web.send(200, "application/json", cached);
}


// Stream the current framebuffer as raw RGB565 (WIDTH*HEIGHT*2 bytes).
// Uses ETag + If-None-Match so the browser only downloads when the buffer changes.
static void webHandleLcdUiJson();

static void webHandleLcdRaw() {
  if (!requireAuthOrPortal()) return;
  if (!webLcdPollingEnabled) {
    web.sendHeader("Cache-Control", "no-store");
    web.sendHeader("X-LCD-Polling", "0");
    web.send(423, "text/plain", "Polling disabled");
    return;
  }
  if (!fbFront) { web.send(503, "text/plain", "No framebuffer"); return; }

  const uint32_t len = WIDTH * HEIGHT * 2;
  // If the device display is asleep, the physical LCD is blank. To match that in the
  // web preview, we serve a black buffer and a header the JS can use to overlay text.
  const bool asleep = displaySleeping;
  // IMPORTANT: do NOT hash the framebuffer per request. That was expensive enough
  // to significantly reduce mining hashrate when polling is enabled. Use a cheap
  // generation counter instead.
  const uint32_t tag = asleep ? 0xA51EE1u : (uint32_t)fbGen;
  char etag[24];
  snprintf(etag, sizeof(etag), asleep ? "asleep%08lx" : "fb%08lx", (unsigned long)tag);

  const String inm = web.header("If-None-Match");
  web.sendHeader("ETag", etag);
  web.sendHeader("Cache-Control", "no-store");
  web.sendHeader("X-LCD-Asleep", asleep ? "1" : "0");
  if (inm.length() && inm == etag) {
    web.send(304, "application/octet-stream", "");
    return;
  }

  web.setContentLength(len);
  web.send(200, "application/octet-stream", "");
  WiFiClient c = web.client();
  if (!asleep) {
    c.write((const uint8_t*)fbFront, len);
  } else {
    // Stream a black RGB565 buffer efficiently.
    static uint8_t zeros[256];
    memset(zeros, 0, sizeof(zeros));
    uint32_t remaining = len;
    while (remaining) {
      const uint32_t n = (remaining > sizeof(zeros)) ? sizeof(zeros) : remaining;
      c.write(zeros, n);
      remaining -= n;
    }
  }
}

// Stream the current framebuffer as a small 24-bit BMP.
// This lets the status page show a live LCD preview without any heavy image codecs.
static void webHandleLcdBmp() {
  if (!requireAuthOrPortal()) return;
  if (!webLcdPollingEnabled) {
    web.sendHeader("Cache-Control", "no-store");
    web.sendHeader("X-LCD-Polling", "0");
    web.send(423, "text/plain", "Polling disabled");
    return;
  }
  if (!fbFront) { web.send(503, "text/plain", "No framebuffer"); return; }

  const bool asleep = displaySleeping;
  web.sendHeader("Cache-Control", "no-store");
  web.sendHeader("X-LCD-Asleep", asleep ? "1" : "0");

  const uint32_t w = WIDTH;
  const uint32_t h = HEIGHT;
  const uint32_t rowSize = (w * 3 + 3) & ~3U; // pad to 4 bytes
  const uint32_t dataSize = rowSize * h;
  const uint32_t fileSize = 54 + dataSize;

  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  // file size
  hdr[2] = (uint8_t)(fileSize);
  hdr[3] = (uint8_t)(fileSize >> 8);
  hdr[4] = (uint8_t)(fileSize >> 16);
  hdr[5] = (uint8_t)(fileSize >> 24);
  // pixel data offset
  hdr[10] = 54;
  // DIB header size
  hdr[14] = 40;
  // width
  hdr[18] = (uint8_t)(w);
  hdr[19] = (uint8_t)(w >> 8);
  hdr[20] = (uint8_t)(w >> 16);
  hdr[21] = (uint8_t)(w >> 24);
  // height
  hdr[22] = (uint8_t)(h);
  hdr[23] = (uint8_t)(h >> 8);
  hdr[24] = (uint8_t)(h >> 16);
  hdr[25] = (uint8_t)(h >> 24);
  // planes
  hdr[26] = 1;
  // bpp
  hdr[28] = 24;
  // raw RGB
  hdr[30] = 0;
  // image size
  hdr[34] = (uint8_t)(dataSize);
  hdr[35] = (uint8_t)(dataSize >> 8);
  hdr[36] = (uint8_t)(dataSize >> 16);
  hdr[37] = (uint8_t)(dataSize >> 24);

  web.setContentLength(fileSize);
  web.send(200, "image/bmp", "");
  WiFiClient c = web.client();
  c.write(hdr, sizeof(hdr));

  // BMP stores rows bottom->top.
  uint8_t pad[3] = {0,0,0};
  const uint32_t padLen = rowSize - w * 3;
  for (int y = (int)h - 1; y >= 0; y--) {
    for (uint32_t x = 0; x < w; x++) {
      const uint16_t p = asleep ? 0 : fbFront[idx((int)x, y)];
      // RGB565 -> 8-bit per channel
      const uint8_t r = (uint8_t)((((p >> 11) & 0x1F) * 255) / 31);
      const uint8_t g = (uint8_t)((((p >> 5) & 0x3F) * 255) / 63);
      const uint8_t b = (uint8_t)((((p) & 0x1F) * 255) / 31);
      const uint8_t bgr[3] = {b, g, r};
      c.write(bgr, 3);
    }
    if (padLen) c.write(pad, padLen);
  }
}


// Toggle/inspect live LCD polling (web preview).
static void webHandleLcdPolling() {
  if (!requireAuthOrPortal()) return;
  const String en = web.arg("enable");
  if (en.length()) {
    webLcdPollingEnabled = (en != "0");
  }
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", String("{\"enabled\":") + (webLcdPollingEnabled ? "true" : "false") + "}");
}

static void webHandleLcdPollingJson() {
  if (!requireAuthOrPortal()) return;
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", String("{\"enabled\":") + (webLcdPollingEnabled ? "true" : "false") + "}");
}

static String makeScreenshotFilename() {
  // Prefer epoch time if available, else millis().
  char buf[48];
  time_t nowT = time(nullptr);
  if (nowT > 1700000000) {
    struct tm tmv;
    localtime_r(&nowT, &tmv);
    snprintf(buf, sizeof(buf), "screen_%04d%02d%02d_%02d%02d%02d.bmp", tmv.tm_year+1900, tmv.tm_mon+1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  } else {
    snprintf(buf, sizeof(buf), "screen_%lu.bmp", (unsigned long)millis());
  }
  return String(buf);
}

static void writeFramebufferBmpToStream(Stream &out, bool asleep) {
  const uint32_t w = WIDTH;
  const uint32_t h = HEIGHT;
  const uint32_t rowSize = (w * 3 + 3) & ~3U; // padded to 4 bytes

  // BMP is bottom-up.
  static uint8_t row[WIDTH * 3 + 4];
  for (int y = (int)h - 1; y >= 0; y--) {
    uint32_t o = 0;
    for (uint32_t x = 0; x < w; x++) {
      uint16_t p = 0;
      if (!asleep) p = fbFront[idx((int)x, y)];
      // Convert RGB565 -> BGR888
      const uint8_t r8 = (uint8_t)((((p >> 11) & 0x1F) * 255) / 31);
      const uint8_t g8 = (uint8_t)((((p >> 5)  & 0x3F) * 255) / 63);
      const uint8_t b8 = (uint8_t)((((p)       & 0x1F) * 255) / 31);
      row[o++] = b8;
      row[o++] = g8;
      row[o++] = r8;
    }
    while (o < rowSize) row[o++] = 0;
    out.write(row, rowSize);
    delay(0);
  }
}

static void webHandleLcdScreenshot() {
  if (!requireAuthOrPortal()) return;
  if (!fbFront) { web.send(503, "text/plain", "No framebuffer"); return; }

  const bool asleep = displaySleeping;
  const String mode = web.arg("mode"); // 'sd' or 'download'
  const bool toSd = (mode == "sd");

  // Pause mining during SD writes (minerTaskFn watches sdBusy).
  sdBusy = true;
  ledService();

  const uint32_t w = WIDTH;
  const uint32_t h = HEIGHT;
  const uint32_t rowSize = (w * 3 + 3) & ~3U; // pad to 4 bytes
  const uint32_t dataSize = rowSize * h;
  const uint32_t fileSize = 54 + dataSize;

  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  hdr[2] = (uint8_t)(fileSize);
  hdr[3] = (uint8_t)(fileSize >> 8);
  hdr[4] = (uint8_t)(fileSize >> 16);
  hdr[5] = (uint8_t)(fileSize >> 24);
  hdr[10] = 54;
  hdr[14] = 40;
  hdr[18] = (uint8_t)(w);
  hdr[19] = (uint8_t)(w >> 8);
  hdr[20] = (uint8_t)(w >> 16);
  hdr[21] = (uint8_t)(w >> 24);
  hdr[22] = (uint8_t)(h);
  hdr[23] = (uint8_t)(h >> 8);
  hdr[24] = (uint8_t)(h >> 16);
  hdr[25] = (uint8_t)(h >> 24);
  hdr[26] = 1;
  hdr[28] = 24;
  hdr[34] = (uint8_t)(dataSize);
  hdr[35] = (uint8_t)(dataSize >> 8);
  hdr[36] = (uint8_t)(dataSize >> 16);
  hdr[37] = (uint8_t)(dataSize >> 24);

  const String fname = makeScreenshotFilename();

  if (toSd) {
    if (!sdBegin()) {
      sdBusy = false; ledService();
      web.send(500, "application/json", "{\"error\":\"sd_not_available\"}");
      return;
    }
    if (!SD_MMC.exists("/screenshots")) { SD_MMC.mkdir("/screenshots"); }
    const String full = String("/screenshots/") + fname;
    File f = SD_MMC.open(full.c_str(), FILE_WRITE);
    if (!f) {
      sdBusy = false; ledService();
      web.send(500, "application/json", "{\"error\":\"open_failed\"}");
      return;
    }
    f.write(hdr, sizeof(hdr));
    writeFramebufferBmpToStream(f, asleep);
    f.close();
    sdBusy = false; ledService();
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "application/json", String("{\"saved\":\"") + full + "\"}");
    return;
  }

  // Download directly
  web.sendHeader("Cache-Control", "no-store");
  web.sendHeader("Content-Disposition", String("attachment; filename=\"") + fname + "\"");
  web.setContentLength(fileSize);
  web.send(200, "image/bmp", "");
  WiFiClient c = web.client();
  c.write(hdr, sizeof(hdr));
  writeFramebufferBmpToStream(c, asleep);
  sdBusy = false; ledService();
}

// Upload a PNG (generated by the browser) and save it to SD.
// This avoids doing any image encoding work on the ESP32.
static File lcdPngUploadFile;
static String lcdPngUploadPath;
static bool lcdPngUploadOk = false;
static bool lcdPngUploadAuthOk = false;

static String makeScreenshotPngFilename() {
  char buf[48];
  time_t nowT = time(nullptr);
  if (nowT > 1700000000) {
    struct tm tmv;
    localtime_r(&nowT, &tmv);
    snprintf(buf, sizeof(buf), "screen_%04d%02d%02d_%02d%02d%02d.png",
             tmv.tm_year+1900, tmv.tm_mon+1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  } else {
    snprintf(buf, sizeof(buf), "screen_%lu.png", (unsigned long)millis());
  }
  return String(buf);
}

static String sanitizeFilename(const String &in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (ok) out += c;
  }
  // Prevent empty / dotfiles.
  while (out.startsWith(".")) out.remove(0, 1);
  if (out.length() == 0) out = makeScreenshotPngFilename();
  return out;
}

static void webHandleLcdUploadPng() {
  if (!requireAuthOrPortal()) return;
  web.sendHeader("Cache-Control", "no-store");
  if (!lcdPngUploadAuthOk) {
    web.send(403, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }
  if (!lcdPngUploadOk || lcdPngUploadPath.length() == 0) {
    web.send(500, "application/json", "{\"error\":\"upload_failed\"}");
    return;
  }
  String out = String("{\"saved\":\"") + lcdPngUploadPath + "\"}";
  web.send(200, "application/json", out);
}

static void webHandleLcdUploadPngUpload() {
  HTTPUpload &up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    lcdPngUploadOk = false;
    // Auth is enforced by the final request handler (webHandleLcdUploadPng).
    // Do NOT call requireAuthOrPortal() from the upload callback because it may
    // try to send a response mid-upload.
    lcdPngUploadAuthOk = true;
    lcdPngUploadPath = "";
    if (!lcdPngUploadAuthOk) return;

    // Pause mining during SD writes.
    sdBusy = true;
    ledService();

    if (!sdBegin()) {
      sdBusy = false; ledService();
      return;
    }
    if (!SD_MMC.exists("/screenshots")) { SD_MMC.mkdir("/screenshots"); }

    String fn = up.filename;
    fn = sanitizeFilename(fn);
    if (!fn.endsWith(".png") && !fn.endsWith(".PNG")) fn += ".png";
    lcdPngUploadPath = String("/screenshots/") + fn;

    lcdPngUploadFile = SD_MMC.open(lcdPngUploadPath.c_str(), FILE_WRITE);
    if (!lcdPngUploadFile) {
      sdBusy = false; ledService();
      lcdPngUploadPath = "";
      return;
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (lcdPngUploadFile) {
      lcdPngUploadFile.write(up.buf, up.currentSize);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (lcdPngUploadFile) {
      lcdPngUploadFile.close();
      lcdPngUploadOk = true;
    }
    sdBusy = false;
    ledService();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (lcdPngUploadFile) lcdPngUploadFile.close();
    lcdPngUploadOk = false;
    lcdPngUploadPath = "";
    sdBusy = false;
    ledService();
  }
}


static void webHandleTdonglePng() {
  if (!requireAuthOrPortal()) return;
  web.sendHeader("Cache-Control", "public, max-age=86400");
  web.send_P(200, "image/png", (PGM_P)TDONGLE_PNG, TDONGLE_PNG_LEN);
}

static void webHandleReboot() {
  if (!requireAuthOrPortal()) return;
  // Show a friendly reboot page that returns the browser to /status.
  // Many browsers will keep retrying the image/js fetches automatically,
  // but we also actively poll so the redirect happens as soon as the device is back.
  String page = htmlHeader("Rebooting");
  page += F("<h2>Rebooting...</h2>"
            "<p class='muted'>Device is restarting. This page will return you to the Status dashboard automatically.</p>"
            "<p class='muted'>If it doesn't come back, refresh or reconnect to the correct WiFi network.</p>"
            "<script>\n"
            "const start=Date.now();\n"
            "function tryBack(){\n"
            "  fetch('/status.json',{cache:'no-store',credentials:'include'}).then(r=>{\n"
            "    if(r.ok) location.href='/status';\n"
            "  }).catch(()=>{});\n"
            "  if(Date.now()-start>5000) location.href='/status';\n"
            "}\n"
            "setInterval(tryBack,500);\n"
            "setTimeout(tryBack,200);\n"
            "</script>");
  page += htmlFooter();
  web.send(200, "text/html", page);
  delay(200);
  ESP.restart();
}

static void webHandleBootPress() {
  if (!requireAuthOrPortal()) return;
  // Extra debounce for the web UI: some browsers/devices can generate
  // multiple pointer events from a single tap/click (e.g. touch + mouse).
  static uint32_t lastWebBootMs = 0;
  uint32_t now = millis();
  if (now - lastWebBootMs < 350) {
    web.send(200, "application/json", "{\"ok\":true,\"debounced\":true}");
    return;
  }
  lastWebBootMs = now;

  bootButtonShortPress();
  web.send(200, "application/json", "{\"ok\":true}");
}

// Web UI: start the captive portal / AP mode.
// This mirrors a long BOOT press (user-invoked portal start).
static void webHandleStartAP() {
  if (!requireAuthOrPortal()) return;
  if (!portalRunning) {
    // Start captive portal / AP mode.
    // Note: we don't force the LCD to the Setup page here because the Page
    // enum/page variable are defined later in this file; starting the portal
    // is enough to switch networking into AP/captive-portal mode.
    portalStart(false);
  }
  web.send(200, "application/json", "{\"ok\":true,\"portal\":true}");
}

void webHandleLocate() {
  if (!requireAuthOrPortal()) return;

  // Accept enable=1/0 via query or body
  if (web.hasArg("enable")) {
    const String e = web.arg("enable");
    locateMode = (e == "1" || e == "true" || e == "on" || e == "yes");
  }

  StaticJsonDocument<96> doc;
  doc["locate"] = (bool)locateMode;
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

void webHandleDeviceControl() {
  if (!requireAuthOrPortal()) return;

  if (web.hasArg("enable")) {
    const String e = web.arg("enable");
    deviceControlMode = (e == "1" || e == "true" || e == "on" || e == "yes");
  }

  StaticJsonDocument<96> doc;
  doc["device_control"] = (bool)deviceControlMode;
  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}



static void webHandleFactoryReset() {
  if (!requireAuthOrPortal()) return;
  web.send(200, "text/plain", "Factory reset... rebooting");

  // Clear our NVS namespace, then reboot.
  Preferences p;
  p.begin("nukaminer", false);
  p.clear();
  p.end();

  delay(300);
  ESP.restart();
}

static void webHandleRestartMiner() {
  if (!requireAuthOrPortal()) return;
  web.send(200, "text/plain", "Restarting miner...");
  minerStop();
  delay(50);
  minerStart();
}

static void webHandleLogsJson() {
  if (!requireAuthOrPortal()) return;
  uint32_t since = (uint32_t) web.arg("since").toInt();

  // Fast path: nothing new. Avoid JSON building/allocations.
  if (since >= logSeq) {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"seq\":%lu,\"lines\":[]}", (unsigned long)logSeq);
    web.send(200, "application/json", buf);
    return;
  }

  // Copy out up to 60 newest lines newer than `since`.
  StaticJsonDocument<2048> doc;
  doc["seq"] = logSeq;
  JsonArray arr = doc["lines"].to<JsonArray>();

  // If client is too far behind, just send the last 60 lines.
  uint32_t available = logCount;
  uint32_t oldestSeq = (logSeq > available) ? (logSeq - available) : 0;
  if (since < oldestSeq) since = oldestSeq;

  uint32_t startSeq = since;
  uint32_t endSeq = logSeq;
  uint32_t maxSend = 60;
  uint32_t toSend = endSeq - startSeq;
  if (toSend > maxSend) startSeq = endSeq - maxSend;

  for (uint32_t s = startSeq; s < endSeq; s++) {
    // Map sequence to ring index
    uint32_t offsetFromEnd = (endSeq - 1) - s;
    if (offsetFromEnd >= logCount) continue;
    int idx = (int)logHead - 1 - (int)offsetFromEnd;
    while (idx < 0) idx += (int)LOG_LINES_MAX;
    idx %= (int)LOG_LINES_MAX;
    arr.add(logLines[idx]);
  }

  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

static void webRenderConsole() {
  if (!requireAuthOrPortal()) return;
  String page = htmlHeader("NukaMiner Console");
  page += F("<h2>Live Console</h2><p class='muted'>Shows recent miner/app logs (polls /logs.json).</p>");
  page += F("<div class='row'>"
            "<form method='post' action='/miner/restart'><button type='submit'>Restart miner</button></form>"
            "<form method='post' action='/reboot' onsubmit=\"return confirm('Reboot device?');\"><button type='submit'>Reboot</button></form>"
            "</div>");
  page += F("<pre id='log' style='height:360px'>Loading...</pre>");
  page += F(
    "<script>"
    "let seq=0;"
    "const el=document.getElementById('log');"
    "function tick(){"
      "fetch('/logs.json?since='+seq)"
        ".then(r=>r.json())"
        ".then(j=>{"
          "seq=j.seq||seq;"
          "if(j.lines&&j.lines.length){"
            "if(el.textContent==='Loading...') el.textContent='';"
            "for(const l of j.lines){el.textContent+=l+'\\n';}"
            "el.scrollTop=el.scrollHeight;"
          "}"
        "})"
        ".catch(()=>{});"
    "}"
    "setInterval(tick,1000);"
    "tick();"
    "</script>"
  );
  page += F("<p><a href='/'>Back</a></p>");
  page += htmlFooter();
  web.send(200, "text/html", page);
}

static void webRenderUpdate() {
  if (!requireAuthOrPortal()) return;
  String page = htmlHeader("NukaMiner Update");
  page += F("<h2>Firmware Update</h2><p class='muted'>Upload a compiled .bin (OTA via HTTP).</p>");
  page += F("<form method='post' action='/update' enctype='multipart/form-data'>"
            "<input type='file' name='firmware' accept='.bin' required>"
            "<button type='submit'>Upload &amp; Flash</button>"
            "</form>");
  page += F("<p class='muted'>After upload completes, the device will reboot.</p>");
  page += F("<p><a href='/config'>Back to Config</a></p>");
  page += htmlFooter();
  web.send(200, "text/html", page);
}

static void webHandleUpdate() {
  if (!requireAuthOrPortal()) return;
  HTTPUpload& up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    NM_log(String("[NukaMiner] OTA upload start: ") + up.filename);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      NM_log(String("[NukaMiner] OTA upload success, bytes=") + up.totalSize);
    } else {
      Update.printError(Serial);
      NM_log("[NukaMiner] OTA upload failed");
    }
  }
}

static void webHandleUpdateDone() {
  if (!requireAuthOrPortal()) return;
  if (Update.hasError()) {
    web.send(500, "text/plain", "Update failed");
  } else {
    web.send(200, "text/plain", "Update OK, rebooting...");
    delay(200);
    ESP.restart();
  }
}

static void registerWebHandlers() {
  // Root: redirect to the right section
  web.on("/", HTTP_GET, [](){
    if (portalRunning) { web.sendHeader("Location", "/config"); web.send(302, "text/plain", ""); }
    else { web.sendHeader("Location", "/status"); web.send(302, "text/plain", ""); }
  });

  // Status + Config sections
  web.on("/status", HTTP_GET, [](){
    if (portalRunning) { web.sendHeader("Location", "/config"); web.send(302, "text/plain", ""); return; }
    webRenderStatus();
  });

  web.on("/config", HTTP_GET, [](){
    webRenderConfig();
  });

  // Duino-Coin dashboard grouping id helpers
  web.on("/duco_gid", HTTP_GET, [](){
    if (!requireAuthOrPortal()) return;
    web.send(200, "text/plain", getOrCreateDucoGroupId());
  });

  web.on("/duco_gid/regenerate", HTTP_POST, [](){
    if (!requireAuthOrPortal()) return;
    String gid = regenerateDucoGroupId();
    web.send(200, "application/json", String("{\"duco_gid\":\"") + gid + "\"}");
  });

  // Backwards-compat aliases
  web.on("/settings", HTTP_GET, [](){ web.sendHeader("Location", "/config"); web.send(302, "text/plain", ""); });
  web.on("/console", HTTP_GET, [](){ web.sendHeader("Location", "/status#console"); web.send(302, "text/plain", ""); });
// Captive portal handlers (work in both AP and STA, but mainly for AP)
  web.on("/save", HTTP_POST, [](){
    if (!portalRunning) { web.send(404,"text/plain","Not found"); return; }
    portalHandleSave();
  });


  // SD backup / restore UI + file browser
  web.on("/backup", HTTP_GET, webRenderBackupPage);
  web.on("/backup/create", HTTP_POST, webHandleBackupCreate);
  web.on("/backup/delete", HTTP_POST, webHandleBackupDelete);
  web.on("/backup/download_current", HTTP_GET, webHandleBackupDownloadCurrent);


  web.on("/restore", HTTP_GET, webRenderRestorePage);
  web.on("/restore", HTTP_POST, webHandleRestoreSelected);
  web.on("/restore/upload", HTTP_POST, [](){
    if (!requireAuthOrPortal()) return;
    // Result is set by upload callback
    if (!g_restoreUploadOk) {
      web.send(400, "text/plain", g_restoreUploadErr.length() ? g_restoreUploadErr : "Restore failed\n");
      return;
    }
    web.sendHeader("Location", "/restore?msg=" + urlEncode(g_restoreUploadMsg));
    web.send(303, "text/plain", "OK");
  }, [](){
    if (!requireAuthOrPortal()) return;
    HTTPUpload& up = web.upload();
    static String buf;
    static bool started = false;
    static bool tooBig = false;

    if (up.status == UPLOAD_FILE_START) {
      started = true;
      tooBig = false;
      buf = "";
      g_restoreUploadOk = false;
      g_restoreUploadErr = "";
      g_restoreUploadMsg = "";
      // Slightly longer timeout for slower clients
      web.client().setTimeout(120000);
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (!started) return;
      if (tooBig) return;
      if (buf.length() + up.currentSize > 65536) {
        tooBig = true;
        g_restoreUploadErr = "Uploaded file is too large\n";
        return;
      }
      buf.reserve(buf.length() + up.currentSize);
      for (size_t i=0;i<up.currentSize;i++) buf += (char)up.buf[i];
    } else if (up.status == UPLOAD_FILE_END) {
      if (!started) return;
      if (tooBig) return;
      // Parse and apply
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, buf);
      if (err) {
        g_restoreUploadErr = String("Invalid JSON: ") + err.c_str() + "\n";
        g_restoreUploadOk = false;
      } else {
        bool ok = applyConfigFromJson(doc);
        g_restoreUploadOk = ok;
        if (ok) {
          g_restoreUploadMsg = "Restore complete";
        } else {
          g_restoreUploadErr = "Restore failed\n";
        }
      }
      started = false;
      buf = "";
    }
  });


  web.on("/files", HTTP_GET, webRenderFilesPage);
  // Lightweight JSON directory listing used by the SD Files page (for overwrite prompts, etc.).
  web.on("/files/list.json", HTTP_GET, [](){
    if (!requireAuthOrPortal()) return;
    if (!sdBegin()) { web.send(500,"application/json","{\"error\":\"sd_not_available\"}"); return; }
    String raw = web.arg("path");
    String path;
    if (!isSafePath(raw, path)) path = "/";
    File d = SD_MMC.open(path.c_str());
    if (!d || !d.isDirectory()) { if (d) d.close(); web.send(404,"application/json","{\"error\":\"not_a_directory\"}"); return; }
    StaticJsonDocument<4096> doc;
    doc["path"] = path;
    JsonArray arr = doc.createNestedArray("entries");
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
      JsonObject o = arr.createNestedObject();
      o["name"] = String(e.name());
      o["isDir"] = e.isDirectory();
      if (!e.isDirectory()) o["size"] = (uint32_t)e.size();
      e.close();
      if (arr.size() > 300) break;
    }
    d.close();
    String out;
    serializeJson(doc, out);
    web.send(200, "application/json", out);
  });

  web.on("/files/download", HTTP_GET, webHandleFileDownload);

  web.on("/files/delete", HTTP_POST, [](){
    if (!requireAuthOrPortal()) return;
    if (!sdBegin()) { web.send(500,"text/plain","SD not available"); return; }
    String raw = web.arg("path");
    String path;
    if (!isSafePath(raw, path)) { web.send(400,"text/plain","Bad path"); return; }
    if (path == "/") { web.send(400,"text/plain","Refusing to delete root"); return; }
    File f = SD_MMC.open(path.c_str());
    if (!f) { web.send(404,"text/plain","Not found"); return; }
    bool isDir = f.isDirectory();
    f.close();
    bool ok = isDir ? sdRemoveRecursive(path) : SD_MMC.remove(path.c_str());
    if (!ok) { web.send(500,"text/plain","Delete failed"); return; }
    String parent = parentDirOf(path);
    web.sendHeader("Location", "/files?path=" + urlEncode(parent));
    web.send(303,"text/plain","Deleted");
  });

    web.on("/files/rename", HTTP_POST, [](){
    if (!requireAuthOrPortal()) return;
    if (!sdBegin()) { web.send(500, "text/plain", "SD not available\n"); return; }

    String curRaw = web.arg("path");
    String cur;
    if (!isSafePath(curRaw, cur)) cur = "/";

    String oldName = web.arg("old");
    String newName = web.arg("new");

    // Basic validation: no slashes, no traversal, and non-empty
    oldName.replace("\\", "");
    oldName.replace("/", "");
    newName.replace("\\", "");
    newName.replace("/", "");
    if (newName.length() == 0 || newName.indexOf("..") >= 0) {
      web.send(400, "text/plain", "Invalid new name\n");
      return;
    }

    String oldFull = (cur == "/") ? (String("/") + oldName) : (cur + "/" + oldName);
    String newFull = (cur == "/") ? (String("/") + newName) : (cur + "/" + newName);

    String oldNorm, newNorm;
    if (!isSafePath(oldFull, oldNorm) || !isSafePath(newFull, newNorm)) {
      web.send(400, "text/plain", "Invalid path\n");
      return;
    }

    if (SD_MMC.exists(newNorm.c_str())) {
      web.send(409, "text/plain", "Target already exists\n");
      return;
    }

    if (!SD_MMC.rename(oldNorm.c_str(), newNorm.c_str())) {
      web.send(500, "text/plain", "Rename failed\n");
      return;
    }

    web.sendHeader("Location", "/files?path=" + urlEncode(cur));
    web.send(303, "text/plain", "OK");
  });

web.on("/files/mkdir", HTTP_POST, [](){
    if (!requireAuthOrPortal()) return;
    if (!sdBegin()) { web.send(500,"text/plain","SD not available"); return; }
    String baseRaw = web.arg("path");
    String base;
    if (!isSafePath(baseRaw, base)) base = "/";
    String name = web.arg("name");
    name.replace("/", "");
    if (name.length() == 0) { web.send(400,"text/plain","Bad name"); return; }
    String full = (base=="/") ? (String("/")+name) : (base+"/"+name);
    String norm;
    if (!isSafePath(full, norm)) { web.send(400,"text/plain","Bad path"); return; }
    if (!SD_MMC.mkdir(norm.c_str())) { web.send(500,"text/plain","mkdir failed"); return; }
    web.sendHeader("Location", "/files?path=" + urlEncode(base));
    web.send(303,"text/plain","OK");
  });

  web.on("/files/upload", HTTP_POST, [](){
    if (!requireAuthOrPortal()) return;
    sdBusy = false;
    ledService();
    String redir = web.arg("redir");
    if (redir.length()) {
      web.sendHeader("Location", redir);
      web.send(303,"text/plain","OK");
    } else {
      web.send(200,"text/plain","OK");
    }
  }, [](){
    if (!requireAuthOrPortal()) return;
    if (!sdBegin()) return;
    String dirRaw = web.arg("path");
    String dir;
    if (!isSafePath(dirRaw, dir)) dir = "/";

    HTTPUpload& up = web.upload();
    static File uploadFile;
    static uint32_t uploadBytesSinceFlush = 0;
    if (up.status == UPLOAD_FILE_START) {
      sdBusy = true;
      ledService();
      web.client().setTimeout(120000);
      String fname = up.filename;
      fname.replace("/", "");
      String full = (dir=="/") ? (String("/")+fname) : (dir+"/"+fname);
      String norm;
      if (!isSafePath(full, norm)) return;
      if (SD_MMC.exists(norm.c_str())) SD_MMC.remove(norm.c_str());
      uploadFile = SD_MMC.open(norm.c_str(), FILE_WRITE);
      uploadBytesSinceFlush = 0;
      NM_log(String("[NukaMiner] SD upload start: " ) + norm);
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (uploadFile) {
        size_t w = uploadFile.write(up.buf, up.currentSize);
        if (w != (size_t)up.currentSize) {
          NM_log(String("[NukaMiner] SD upload write short: wrote=") + String((unsigned long)w) + String(" expected=") + String((unsigned long)up.currentSize));
        }
        uploadBytesSinceFlush += (uint32_t)w;
        if (uploadBytesSinceFlush >= 32768) { uploadFile.flush(); uploadBytesSinceFlush = 0; }
        delay(0);
      }
    } else if (up.status == UPLOAD_FILE_END) {
      if (uploadFile) { uploadFile.flush(); uploadFile.close(); NM_log(String("[NukaMiner] SD upload done, bytes=") + up.totalSize); }
      sdBusy = false;
      ledService();
    } else if (up.status == UPLOAD_FILE_ABORTED) {
      if (uploadFile) uploadFile.close();
      sdBusy = false;
      ledService();
      NM_log("[NukaMiner] SD upload aborted");
    }
  });

  // Compatibility endpoints (plain text)
  web.on("/backup_raw", HTTP_GET, [](){
    if (!portalRunning && !requireAuthOrPortal()) return;
    portalHandleBackup();
  });
  web.on("/restore_raw", HTTP_GET, [](){
    if (!portalRunning && !requireAuthOrPortal()) return;
    portalHandleRestore();
  });

  // Web UI endpoints
  web.on("/save_settings", HTTP_POST, webHandleSaveSettings);
  web.on("/status.json", HTTP_GET, webHandleStatusJson);
  web.on("/tdongle.png", HTTP_GET, webHandleTdonglePng);
  web.on("/lcd.raw", HTTP_GET, webHandleLcdRaw);
  web.on("/lcd.ui.json", HTTP_GET, webHandleLcdUiJson);
  web.on("/lcd.bmp", HTTP_GET, webHandleLcdBmp);
  // PNG upload (browser-encoded) -> save to SD
  web.on("/lcd/upload_png", HTTP_POST, webHandleLcdUploadPng, webHandleLcdUploadPngUpload);
  // Legacy BMP screenshot endpoint (kept for backward compatibility)
  web.on("/lcd/screenshot", HTTP_POST, webHandleLcdScreenshot);
  web.on("/logs.json", HTTP_GET, webHandleLogsJson);
  web.on("/btn/boot", HTTP_POST, webHandleBootPress);
  web.on("/locate", HTTP_POST, webHandleLocate);
  web.on("/locate", HTTP_GET,  webHandleLocate);
  web.on("/device_control", HTTP_POST, webHandleDeviceControl);
  web.on("/device_control", HTTP_GET,  webHandleDeviceControl);
  web.on("/ap/start", HTTP_POST, webHandleStartAP);
  web.on("/miner/restart", HTTP_POST, webHandleRestartMiner);

  // WiFi helpers
  web.on("/wifi", HTTP_GET, webRenderWifiPage);
  web.on("/wifi/scan.json", HTTP_GET, webHandleWifiScanJson);
  web.on("/wifi/profile/save", HTTP_POST, webHandleWifiProfileSave);
  web.on("/wifi/profile/delete", HTTP_POST, webHandleWifiProfileDelete);
  web.on("/wifi/profile/connect", HTTP_POST, webHandleWifiProfileConnect);

  // Simple HTTP OTA update
  web.on("/update", HTTP_GET, webRenderUpdate);
  web.on("/update", HTTP_POST, webHandleUpdateDone, webHandleUpdate);
  // Allow both POST (from forms) and GET (from links/bookmarks) so reboot behaves
  // consistently across the UI.
  web.on("/reboot", HTTP_POST, webHandleReboot);
  web.on("/reboot", HTTP_GET,  webHandleReboot);
  web.on("/factory_reset", HTTP_POST, webHandleFactoryReset);


  // Captive portal OS-specific probe URLs
  web.on("/generate_204", HTTP_GET, [](){ if (portalRunning) { web.sendHeader("Location","/"); web.send(302,"text/plain",""); } else web.send(204,"text/plain",""); });
  web.on("/hotspot-detect.html", HTTP_GET, [](){ if (portalRunning) { web.sendHeader("Location","/"); web.send(302,"text/plain",""); } else web.send(404,"text/plain",""); });
  web.on("/ncsi.txt", HTTP_GET, [](){ if (portalRunning) { web.sendHeader("Location","/"); web.send(302,"text/plain",""); } else web.send(404,"text/plain",""); });
  web.on("/connecttest.txt", HTTP_GET, [](){ if (portalRunning) { web.sendHeader("Location","/"); web.send(302,"text/plain",""); } else web.send(404,"text/plain",""); });

  web.onNotFound([](){
    // captive portal redirect when AP is running
    if (portalRunning) {
      web.sendHeader("Location","/");
      web.send(302,"text/plain","");
      return;
    }
    web.send(404, "text/plain", "Not found");
  });
}

static void portalRenderRoot() {
  String page = F(
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>NukaMiner setup</title>"
    "<style>"
    "body{font-family:system-ui;margin:24px;max-width:640px}"
    "h1{margin:0 0 8px 0} .sub{color:#666;margin:0 0 18px 0}"
    "label{display:block;margin:14px 0 6px 0;font-weight:600}"
    "input{width:100%;padding:10px;font-size:16px}"
    "button{margin-top:18px;padding:12px 14px;font-size:16px}"
    ".row{display:flex;gap:12px} .row>div{flex:1}"
    "</style></head><body>"
    "<h1>NukaMiner</h1><p class=sub>WiFi + Duino-Coin configuration</p>"
    "<form action='/save' method='post'>"
  );

  page += F("<label>WiFi SSID</label><input name='wifi_ssid' value='");
  page += htmlEscape(cfg.wifi_ssid);
  page += F("'>");

  page += F("<label>WiFi password</label><input name='wifi_pass' type='password' value='");
  page += htmlEscape(cfg.wifi_pass);
  page += F("'>");

  page += F("<div class='row'><div><label>Duino-Coin username</label>"
            "<div style='display:flex;gap:8px;align-items:center'>"
            "<input name='duco_user' value='");
  page += htmlEscape(cfg.duco_user);
  page += F("' style='flex:1'>"
            "<button type='button' class='smallBtn' onclick=\"window.open('https://duinocoin.com/','_blank')\">Create Account</button>"
            "</div>"
            "</div>"
            "<div><label>Duino-Coin account</label>"
            "<div class='muted'>Need an account?<br>Press the button and create one at DuinoCoin.com</div>"
            "</div></div>");

  page += F("<div class='row'><div><label>Rig identifier</label><input name='rig_id' value='");
  page += htmlEscape(cfg.rig_id);
  page += F("'></div><div><label>Mining key (optional)</label><input name='miner_key' value='");
  page += htmlEscape(cfg.miner_key);
  page += F("'></div></div>");
page += F("<label>NTP server</label><input name='ntp_server' value='");
  page += htmlEscape(cfg.ntp_server);
  page += F("'><div class='hint'><code>Defaults to pool.ntp.org</code></div>");


  page += F("<label>Display sleep (seconds, 0 = never)</label><input name='disp_sleep' type='number' min='0' max='86400' value='");
  page += String(cfg.display_sleep_s);
  page += F("'>");

  page += F("<label>LCD brightness (0-100)</label><input name='lcd_br' type='range' min='0' max='100' value='");
  page += String(cfg.lcd_brightness);
  page += F("' oninput=\"this.nextElementSibling.textContent=this.value+'%';\"><div style='color:#666;margin-top:4px'>");
  page += String(cfg.lcd_brightness);
  page += F("%</div>");

  page += F("<div class='row'><div><label>LCD orientation</label><select name='lcd_r180'>");
  page += cfg.lcd_rot180 ? F("<option value='0'>Normal</option><option value='1' selected>Rotated 180&deg;</option>")
                         : F("<option value='0' selected>Normal</option><option value='1'>Rotated 180&deg;</option>");
  page += F("</select></div><div><label>Hashrate limit (0-100)</label><input name='hash_lim' type='range' min='0' max='100' value='");
  page += String(cfg.hash_limit_pct);
  page += F("' oninput=\"this.nextElementSibling.textContent=this.value+'%';\"><div style='color:#666;margin-top:4px'>");
  page += String(cfg.hash_limit_pct);
  page += F("%</div></div></div>");

  page += F("<div class='row'><div><label>Second mining core</label><select name='c2_en'>");
  page += cfg.core2_enabled ? F("<option value='1' selected>Enabled</option><option value='0'>Disabled</option>")
                            : F("<option value='1'>Enabled</option><option value='0' selected>Disabled</option>");
  page += F("</select><div class='hint'>Uses the other CPU core for additional hashrate. Higher limits may reduce responsiveness and increase heat.</div></div>"
            "<div><label>Core 2 limit (0-100)</label><input name='c2_lim' type='range' min='0' max='100' value='");
  page += String(cfg.core2_hash_limit_pct);
  page += F("' oninput=\"this.nextElementSibling.textContent=this.value+'%';\"><div style='color:#666;margin-top:4px'>");
  page += String(cfg.core2_hash_limit_pct);
  page += F("%</div></div></div>");

  page += F("<div class='row'><div><label>RGB LED</label><select name='led_en'>");
  page += cfg.led_enabled ? F("<option value='1' selected>Enabled</option><option value='0'>Disabled</option>")
                         : F("<option value='1'>Enabled</option><option value='0' selected>Disabled</option>");
  page += F("</select></div><div><label>LED brightness (0-100)</label><input name='led_br' type='range' min='0' max='100' value='");
  page += String(cfg.led_brightness);
  page += F("' oninput=\"this.nextElementSibling.textContent=this.value+'%';\"><div style='color:#666;margin-top:4px'>");
  page += String(cfg.led_brightness);
  page += F("%</div></div></div>");

  page += F("<div class='row'><div><label>Carousel (auto page flip)</label><select name='car_en'>");
  page += cfg.carousel_enabled ? F("<option value='1' selected>Enabled</option><option value='0'>Disabled</option>")
                              : F("<option value='1'>Enabled</option><option value='0' selected>Disabled</option>");
  page += F("</select></div><div><label>Carousel seconds</label><input name='car_s' type='number' min='2' max='3600' value='");
  page += String(cfg.carousel_seconds);
  page += F("'></div></div>");

  page += F("<label><input type='checkbox' name='duco_en' ");
  if (cfg.duino_enabled) page += F("checked ");
  page += F("> Enable mining</label>");

  page += F("<button type='submit'>Save & reboot</button></form>"
            "<p style='margin-top:18px;color:#666'>Tip: If you have an SD card, you can visit <code>/backup</code> or <code>/restore</code>.</p>"
            "</body></html>");
  web.send(200, "text/html", page);
}

static void portalHandleSave() {
  cfg.wifi_ssid = web.arg("wifi_ssid");
  cfg.wifi_pass = web.arg("wifi_pass");
  cfg.duco_user = web.arg("duco_user");
  cfg.rig_id = web.arg("rig_id");
  cfg.miner_key = web.arg("miner_key");
  cfg.display_sleep_s = (uint32_t) web.arg("disp_sleep").toInt();
  if (web.hasArg("lcd_br")) cfg.lcd_brightness = (uint8_t) constrain(web.arg("lcd_br").toInt(), 0, 100);
  if (web.hasArg("lcd_r180")) cfg.lcd_rot180 = (web.arg("lcd_r180") != "0");
  if (web.hasArg("hash_lim")) cfg.hash_limit_pct = (uint8_t) constrain(web.arg("hash_lim").toInt(), 0, 100);
  if (web.hasArg("c2_en")) cfg.core2_enabled = (web.arg("c2_en") != "0");
  if (web.hasArg("c2_lim")) cfg.core2_hash_limit_pct = (uint8_t) constrain(web.arg("c2_lim").toInt(), 0, 100);
  if (web.hasArg("led_en")) cfg.led_enabled = (web.arg("led_en") != "0");
  if (web.hasArg("led_br")) cfg.led_brightness = (uint8_t) constrain(web.arg("led_br").toInt(), 0, 100);
  if (web.hasArg("car_en")) cfg.carousel_enabled = (web.arg("car_en") != "0");
  if (web.hasArg("car_s"))  cfg.carousel_seconds = (uint16_t) std::max<long>(2L, web.arg("car_s").toInt());
  // Portal does not expose web auth fields by default; don't clobber them.
  if (web.hasArg("web_en")) cfg.web_enabled = (web.arg("web_en") != "0");
  if (web.hasArg("web_user")) cfg.web_user = web.arg("web_user");
  if (web.hasArg("web_pass")) cfg.web_pass = web.arg("web_pass");
  cfg.duino_enabled = web.hasArg("duco_en");

  // Keep WiFi profiles in sync (portal setup page)
  if (cfg.wifi_ssid.length()) {
    if (wifiProfiles.empty()) wifiProfilesMigrateLegacy();
    wifiProfilesUpsert(cfg.wifi_ssid, cfg.wifi_pass, 100, /*keepExistingPrioIfPresent=*/true);
  }
  saveConfig();

  // Use the same styled reboot page as normal mode so AP-mode reboot isn't a plain white page.
  String page = htmlHeader("Rebooting");
  page += F("<h2>Saved!</h2>"
            "<p class='muted'>Rebooting to apply settings...</p>"
            "<p class='muted'>After restart, reconnect to the device's WiFi (AP mode) or your configured WiFi network.</p>"
            "<script>\n"
            "const start=Date.now();\n"
            "function tryBack(){\n"
            "  fetch('/status.json',{cache:'no-store'}).then(r=>{ if(r.ok) location.href='/status'; }).catch(()=>{});\n"
            "  if(Date.now()-start>6000) location.href='/';\n"
            "}\n"
            "setInterval(tryBack,500);\n"
            "setTimeout(tryBack,200);\n"
            "</script>\n");
  page += htmlFooter();
  web.send(200, "text/html", page);
  delay(200);
  ESP.restart();
}

static void portalHandleBackup() {
  bool ok = sdBackupConfig();
  web.send(ok ? 200 : 500, "text/plain", ok ? "OK\n" : "SD not available\n");
}

static void portalHandleRestore() {
  bool ok = sdRestoreConfig();
  web.send(ok ? 200 : 500, "text/plain", ok ? "OK (saved to NVS)\n" : "No SD/backup found\n");
}

static void portalStart(bool isAutoStart) {
  if (portalRunning) return;

  portalAuto = isAutoStart;

  // AP/Portal mode should pause mining to keep the portal responsive and avoid wasting cycles.
  // IMPORTANT: Do not "stop" (tear down) mining tasks here; that can block long enough to trip the
  // watchdog if a miner is mid-connection/hash on the same core. We suspend the miner tasks instead.
  if (minerTask0 || minerTask1) {
    NM_log("[NukaMiner] Portal starting: suspending miner tasks");
    minerSuspendForPortal();
  }

  // Even if the user disabled the Web UI, AP/Portal mode must still expose HTTP
  // so they can recover the device. Force-enable at runtime (not persisted).
  portalForcedWeb = false;
  if (!cfg.web_enabled) {
    webEnabledBeforePortal = cfg.web_enabled;
    cfg.web_enabled = true;
    portalForcedWeb = true;
    NM_log("[NukaMiner] Portal forcing Web UI enabled (runtime)");
  }

  // If we have saved WiFi credentials, keep STA enabled so the device can
  // still connect while the portal is running (AP+STA fallback).
  if (wifiHasAnyConfig()) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_AP);
  }
  String apSsid = "NukaMiner-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  WiFi.softAP(apSsid.c_str(), "nukaminer");
  delay(200);
  // softAPConfig already applied
  //(apIP, apIP, IPAddress(255,255,255,0));

  dns.start(DNS_PORT, "*", apIP);

  // Start HTTP server for captive portal
  if (!webBegun) { web.begin(); webBegun = true; }

  portalRunning = true;
}

static void portalLoop() {
  if (!portalRunning) return;
  dns.processNextRequest();
}

// -----------------------------
// Duino miner task
// -----------------------------
static volatile bool minerRun = false;

static bool minerIsRunning() {
  return ((minerTask0 != nullptr) || (minerTask1 != nullptr)) && minerRun;
}

static MiningConfig* ducoConfig0 = nullptr;
static MiningConfig* ducoConfig1 = nullptr;
static MiningJob* ducoJob0 = nullptr;
static MiningJob* ducoJob1 = nullptr;

static String ducoGroupId = ""; // shared group-id to aggregate workers on Duino-Coin dashboard


static String getOrCreateDucoGroupId() {
  // Persisted across boots so the Duino dashboard keeps the same grouped miner identity.
  if (ducoGroupId.length() > 0) return ducoGroupId;

  Preferences prefs;
  prefs.begin("nukaminer", false);
  ducoGroupId = prefs.getString("duco_gid", "");
  if (ducoGroupId.length() == 0) {
    // Match Official PC Miner behavior: small numeric group id (0-2811).
    uint32_t gid = (uint32_t)esp_random() % 2812U;
    ducoGroupId = String(gid);
    prefs.putString("duco_gid", ducoGroupId);
  }
  prefs.end();
  return ducoGroupId;
}

static String regenerateDucoGroupId() {
  // Force a new group id and persist it (used by the Config page button).
  Preferences prefs;
  prefs.begin("nukaminer", false);
  uint32_t gid = (uint32_t)esp_random() % 2812U;
  ducoGroupId = String(gid);
  prefs.putString("duco_gid", ducoGroupId);
  prefs.end();
  return ducoGroupId;
}

static bool fetchPool(String &host, int &port) {
  // Fetch JSON from https://server.duinocoin.com/getPool
  WiFiClientSecure secure;
  secure.setInsecure(); // simplest for embedded; you can pin cert if desired
  HTTPClient http;
  if (!http.begin(secure, "https://server.duinocoin.com/getPool")) {
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  // Fields vary slightly; accept common ones
  host = String((const char*)(doc["ip"] | doc["host"] | doc["name"] | ""));
  port = (int)(doc["port"] | 2813);

  // Some responses include "ip": "xx" and "name": "xx"
  if (host.length() == 0 && doc["name"]) host = String((const char*)doc["name"]);
  if (host.length() == 0) return false;
  return true;
}

static String g_cachedPoolHost;
static int g_cachedPoolPort = 0;
static uint32_t g_cachedPoolAtMs = 0;

static void invalidatePoolCache() {
  g_cachedPoolHost = "";
  g_cachedPoolPort = 0;
  g_cachedPoolAtMs = 0;
}

static bool fetchPoolCached(String &host, int &port) {
  const uint32_t ttlMs = (cfg.pool_cache_s > 0) ? (cfg.pool_cache_s * 1000UL) : 0UL;

  if (ttlMs > 0 && g_cachedPoolHost.length() > 0) {
    if ((uint32_t)(millis() - g_cachedPoolAtMs) < ttlMs) {
      host = g_cachedPoolHost;
      port = g_cachedPoolPort;
      return true;
    }
  }

  if (!fetchPool(host, port)) return false;

  if (ttlMs > 0) {
    g_cachedPoolHost = host;
    g_cachedPoolPort = port;
    g_cachedPoolAtMs = millis();
  }
  return true;
}


// -----------------------------
// Pool manager (single task)
// -----------------------------
// Fetching / resolving the Duino-Coin pool involves TLS+HTTP and can cause
// cache/memory contention if both miner cores do it. We do it once on CPU0 and
// share the result with miners.
static TaskHandle_t poolTask = nullptr;
static SemaphoreHandle_t poolMutex = nullptr;
static String g_poolHost;
static int    g_poolPort = 0;
static volatile uint32_t g_poolUpdatedMs = 0;
static volatile bool poolInvalidateReq = false;

static bool getSharedPool(String &host, int &port) {
  if (!poolMutex) return false;
  if (xSemaphoreTake(poolMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  host = g_poolHost;
  port = g_poolPort;
  xSemaphoreGive(poolMutex);
  return host.length() > 0 && port > 0;
}

static void setSharedPool(const String &host, int port) {
  if (!poolMutex) return;
  if (xSemaphoreTake(poolMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  g_poolHost = host;
  g_poolPort = port;
  g_poolUpdatedMs = millis();
  xSemaphoreGive(poolMutex);
}

static void poolTaskFn(void *arg) {
  (void)arg;
  String host; int port = 0;

  // Create mutex lazily in case start order changes
  if (!poolMutex) poolMutex = xSemaphoreCreateMutex();

  while (true) {
    if (!minerRun) { vTaskDelay(pdMS_TO_TICKS(250)); continue; }

    if (sdBusy) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    if (!WiFi.isConnected()) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }

    if (poolInvalidateReq) {
      invalidatePoolCache();
      poolInvalidateReq = false;
    }

    // Refresh pool periodically; if caching enabled, fetchPoolCached will return quickly.
    if (fetchPoolCached(host, port)) {
      setSharedPool(host, port);
      node_id = host + ":" + String(port);
      // Refresh every 60s, but respond quickly if caching TTL is shorter.
      vTaskDelay(pdMS_TO_TICKS(60000));
    } else {
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
}





static void minerTaskFn(void *arg) {
  MiningJob *job = (MiningJob*)arg;
  if (!job || !job->config) {
    vTaskDelete(nullptr);
    return;
  }
  MiningConfig *mconf = job->config;

  uint8_t failCount = 0;
  String host; int port = 0;

  while (minerRun) {
    // In AP/Portal mode we pause mining entirely. This keeps the web UI and
    // BOOT interactions responsive while the user is configuring WiFi.
    if (portalRunning || WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // Pause mining during large SD transfers (uploads/downloads/flashing) to keep WiFi responsive.
    if (sdBusy) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    if (!WiFi.isConnected()) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }

    // Pool resolution is handled by poolTaskFn() on CPU0.
    if (!getSharedPool(host, port)) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

    mconf->host = host;
    mconf->port = port;

    // MiningJob::mine() performs connect->job->hash->submit.
    // If it fails repeatedly while WiFi is still up, request a pool cache refresh.
    const bool ok = job->mine();
    if (!ok) {
      failCount++;
      if (WiFi.isConnected() && failCount >= 3) {
        poolInvalidateReq = true;
        failCount = 0;
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    } else {
      failCount = 0;
    }

    // Let the scheduler breathe (but avoid long sleeps here).
    vTaskDelay(1);
  }

  vTaskDelete(nullptr);
}




static void minerStart() {
  if (minerTask0 || minerTask1) return;
  if (!cfg.duino_enabled) return;
  if (portalRunning || WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) return;
  if (cfg.duco_user.length() == 0) return;

  // If both cores are disabled, nothing to do.
  if (!cfg.core1_enabled && !cfg.core2_enabled) return;

  // The Duino-Coin dashboard groups miners by their identifier string.
  // To make each core appear as its own miner, append a -Core# suffix.
  String idBase = cfg.rig_id;
  if (idBase.length() == 0) idBase = cfg.duco_user;

  // When using a shared group-id, Duino-Coin dashboard aggregates multiple workers into one entry
  // and shows it as a single miner with N threads.
  // To ensure aggregation, both workers must use the SAME rig identifier and the SAME group-id.
  String id0 = idBase;
  String id1 = idBase;

  const String groupId = getOrCreateDucoGroupId();


  minerRun = true;

  // Task pinning notes (ESP32-S3):
  // - WiFi + many system tasks usually run on CPU core 0.
  // - The Arduino loop typically runs on CPU core 1.
  // For best responsiveness, keep the *default/primary* miner (Core 2) on CPU core 1.
  // If Core 1 is enabled, run it on CPU core 0 and usually limit it.
  const int pinCore1 = 0;
  const int pinCore2 = 1;


  // Start pool manager on CPU0 (single resolver for both miners).
  // IMPORTANT: fetchPoolCached() can involve TLS + JSON parsing and can be stack-hungry.
  // A too-small task stack will corrupt memory and cause reboot loops (Guru Meditation).
  // Use a larger stack to stay safe on ESP32-S3.
  if (!poolMutex) poolMutex = xSemaphoreCreateMutex();
  if (!poolTask) {
    constexpr uint32_t POOL_TASK_STACK = 12288; // bytes (3x default 4KB)
    xTaskCreatePinnedToCore(poolTaskFn, "ducoPool", POOL_TASK_STACK, nullptr, 1, &poolTask, pinCore1);
  }

  // Core 1 miner task (job0)
  if (cfg.core1_enabled) {
    ducoConfig0 = new MiningConfig(cfg.duco_user, id0, cfg.miner_key, groupId);
    ducoJob0 = new MiningJob(0, ducoConfig0);
    xTaskCreatePinnedToCore(minerTaskFn, "duco0", 8192, ducoJob0, 1, &minerTask0, pinCore1);
  }

  // Core 2 miner task (job1)
  if (cfg.core2_enabled) {
    ducoConfig1 = new MiningConfig(cfg.duco_user, id1, cfg.miner_key, groupId);
    ducoJob1 = new MiningJob(1, ducoConfig1);
    // Keep miner priority at 1 so it doesn't starve the Arduino loop/task.
    // Responsiveness is protected by the serviceTaskFn running at higher priority.
    xTaskCreatePinnedToCore(minerTaskFn, "duco1", 8192, ducoJob1, 1, &minerTask1, pinCore2);
  }
}

static void minerStop() {
  minerRun = false;
  if (minerTask0 || minerTask1) {
    vTaskDelay(pdMS_TO_TICKS(50));
    minerTask0 = nullptr;
    minerTask1 = nullptr;
  }
  delete ducoJob0; ducoJob0=nullptr;
  delete ducoJob1; ducoJob1=nullptr;
  delete ducoConfig0; ducoConfig0=nullptr;
  delete ducoConfig1; ducoConfig1=nullptr;

}
static void minerSuspendForPortal() {
  if (minerSuspendedForPortal) return;
  // Suspending tasks prevents watchdog resets when switching WiFi modes while
  // a miner is mid-hash/connect on the same core as the web handler.
  if (minerTask0) vTaskSuspend(minerTask0);
  if (minerTask1) vTaskSuspend(minerTask1);
  minerSuspendedForPortal = (minerTask0 || minerTask1);
}

static void minerResumeAfterPortal() {
  if (!minerSuspendedForPortal) return;
  if (minerTask0) vTaskResume(minerTask0);
  if (minerTask1) vTaskResume(minerTask1);
  minerSuspendedForPortal = false;
}


// -----------------------------
// UI pages (TzCoinMiner-inspired)
// -----------------------------
enum Page { PAGE_LOGO, PAGE_MINING, PAGE_GRAPH, PAGE_SETUP, PAGE_IP };
static Page page = PAGE_LOGO;

static uint32_t lastInteractionMs = 0;
static uint32_t lastCarouselFlipMs = 0;

// Hashrate history for LCD graph
static const uint16_t HR_HIST_LEN = 60;
static uint32_t hrHist[HR_HIST_LEN];
static uint16_t hrHistPos = 0;
static bool hrHistFilled = false;
static uint32_t lastHrSampleMs = 0;

// Lightweight LCD UI state for the web dashboard (avoids streaming raw framebuffer)
static void webHandleLcdUiJson() {
  if (!requireAuthOrPortal()) return;

  // Respect global LCD polling toggle (same rules as /lcd.raw)
  // Light-LCD endpoint is always available; client controls its own polling cadence.
  web.sendHeader("X-LCD-Polling", "1");

  // Build a small, stable-ish ETag. We only need "good enough" to let the browser 304.
  // Include page id, sleep flag, share count, and a coarse time bucket so the UI updates.
  const uint32_t nowMs = millis();
  const uint32_t coarse = nowMs / 2000U; // 2s buckets
  uint32_t tag = ((uint32_t)page << 24) ^ ((uint32_t)displaySleeping << 23) ^
                 ((uint32_t)accepted_share_count << 1) ^ (uint32_t)coarse ^
                 ((uint32_t)hrHistPos << 8);
  char etag[24];
  snprintf(etag, sizeof(etag), "\"%08lx\"", (unsigned long)tag);

  // If-None-Match handling
  if (web.hasHeader("If-None-Match")) {
    String inm = web.header("If-None-Match");
    if (inm == etag) { web.sendHeader("ETag", etag); web.send(304); return; }
  }
  web.sendHeader("ETag", etag);
  web.sendHeader("Cache-Control", "no-store");

  StaticJsonDocument<1536> doc;
  doc["asleep"] = displaySleeping;

  const char* title = "NukaMiner";
  switch (page) {
    case PAGE_LOGO:   title = "NukaMiner"; break;
    case PAGE_MINING: title = "Mining"; break;
    case PAGE_GRAPH:  title = "Hash Graph"; break;
    case PAGE_SETUP:  title = "Setup"; break;
    case PAGE_IP:     title = "IP Info"; break;
    default: break;
  }
  doc["page"] = (int)page;
  doc["title"] = title;

  JsonArray lines = doc.createNestedArray("lines");

  // NOTE: These strings mirror what the LCD draws, but are intentionally kept small.
  if (page == PAGE_LOGO) {
    lines.add("DUINO-COIN");
    lines.add("MINER");
    lines.add("V1.0");
  } else if (page == PAGE_MINING) {
    char b1[48]; snprintf(b1, sizeof(b1), "User: %s", cfg.duco_user.c_str()); lines.add(b1);
    char b2[48]; snprintf(b2, sizeof(b2), "Rig: %s", cfg.rig_id.c_str()); lines.add(b2);

    const uint32_t totalHash = (uint32_t)hashrate + (cfg.core2_enabled ? (uint32_t)hashrate_core_two : 0U);
    char b3[48]; snprintf(b3, sizeof(b3), "Hash: %.2f kH/s", (double)totalHash / 1000.0); lines.add(b3);

    char b4[48]; snprintf(b4, sizeof(b4), "Diff: %u", (unsigned)difficulty); lines.add(b4);
    char b5[48]; snprintf(b5, sizeof(b5), "Shares: %lu/%lu", (unsigned long)accepted_share_count, (unsigned long)share_count); lines.add(b5);
  } else if (page == PAGE_IP) {
    if (WiFi.isConnected()) {
      char b1[48]; snprintf(b1, sizeof(b1), "SSID: %s", WiFi.SSID().c_str()); lines.add(b1);
      char b2[48]; snprintf(b2, sizeof(b2), "IP: %s", WiFi.localIP().toString().c_str()); lines.add(b2);
      char b3[48]; snprintf(b3, sizeof(b3), "RSSI: %d", WiFi.RSSI()); lines.add(b3);
    } else {
      lines.add("WiFi: disconnected");
    }
  } else if (page == PAGE_GRAPH) {
    lines.add("Total kH/s");
    // Provide graph points as kH/s values (already stored in H/s)
    JsonArray g = doc.createNestedArray("graph");
    const uint16_t count = hrHistFilled ? HR_HIST_LEN : hrHistPos;
    for (uint16_t i = 0; i < count; i++) {
      const uint16_t idx = hrHistFilled ? ((hrHistPos + i) % HR_HIST_LEN) : i;
      g.add((double)hrHist[idx] / 1000.0);
    }
  } else if (page == PAGE_SETUP) {
    lines.add("BOOT: next page");
    lines.add("Hold: AP mode");
    lines.add(portalRunning ? "AP Mode: ON" : "AP Mode: OFF");
  }

  String out;
  serializeJson(doc, out);
  web.send(200, "application/json", out);
}

static void displayWake() {
  displaySleeping = false;
  // Restore configured brightness without mutating the config.
  blSet(cfg.lcd_brightness, false);
  // DISPON (0x29) is common across ST77xx/ILI9xxx controllers.
  // Using the raw command avoids relying on internal TFT_eSPI command macros
  // that some IntelliSense setups may not see.
  tft.writecommand(0x29);
  lastInteractionMs = millis();
  Serial.println("[NukaMiner] Display wake");
}

static void displaySleep() {
  displaySleeping = true;
  // DISPOFF (0x28) puts the panel into sleep mode; backlight is controlled separately.
  tft.writecommand(0x28);
  // Turn the LCD off without altering the configured brightness.
  blSet(0, false);
  Serial.println("[NukaMiner] Display sleep");
}

static void drawTopBar(const char* title) {
  // Top status bar: title + WiFi indicator + temperature + core activity
  fbFillRect(0, 0, WIDTH, 14, TFT_BLACK);
  fbText(title, 4, 3, TFT_YELLOW, 1, false);

  // Core indicators + web indicator (rightmost)
  const bool c1Active = (minerTask0 != nullptr) && minerRun;
  const bool c2Active = (minerTask1 != nullptr) && minerRun;

  // User request: show "1" and "2" indicators.
  // - Green = actively mining
  // - Red = disabled
  // - Yellow = enabled but idle
  const uint16_t c1Col = cfg.core1_enabled ? (c1Active ? TFT_GREEN : TFT_YELLOW) : TFT_RED;
  const uint16_t c2Col = cfg.core2_enabled ? (c2Active ? TFT_GREEN : TFT_YELLOW) : TFT_RED;
  // Add a bit more spacing so "1 2 W" are readable at a glance.
  // Reserve the right-most area for these indicators.
  const int ind1x = WIDTH - 28;
  const int ind2x = WIDTH - 18;
  const int indWx = WIDTH - 8;
  fbText("1", ind1x, 3, c1Col, 1, false);
  fbText("2", ind2x, 3, c2Col, 1, false);

  // Web UI indicator (cyan "W") when available
  const bool webOk = cfg.web_enabled && (portalRunning || cfg.web_always_on || webSessionActive);
  if (webOk) fbText("W", indWx, 3, TFT_CYAN, 1, false);

  // WiFi indicator bars + temperature (keep clear of the 1/2/W indicators, but
  // don't push so far left that page titles overlap the temp/wifi)
  const bool wifiOk = WiFi.isConnected();
  const int rssi = wifiOk ? WiFi.RSSI() : -127;
  uint8_t bars = 0;
  if (wifiOk) {
    if (rssi > -55) bars = 3;
    else if (rssi > -67) bars = 2;
    else if (rssi > -80) bars = 1;
    else bars = 0;
  }
  // Keep WiFi/temp far enough right to avoid overlapping page titles,
  // but left enough to avoid colliding with the 1/2/W indicators.
  // Font widths on the ST7735 vary slightly depending on renderer, so be conservative.
  const int bx = WIDTH - 40; // 160px wide -> x=120; safe gap before the 1/2/W area
  const int by = 11;
  if (!wifiOk) {
    // red "X"
    fbFillRect(bx, 4, 10, 1, TFT_RED);
    fbFillRect(bx, 5, 1, 8, TFT_RED);
    fbFillRect(bx + 9, 5, 1, 8, TFT_RED);
  } else {
    for (uint8_t i = 0; i < 3; i++) {
      const uint8_t h = (i + 1) * 2;
      const uint16_t col = (i < bars) ? TFT_GREEN : TFT_BLACK;
      fbFillRect(bx + i * 3, by - h, 2, h, col);
    }
  }

  // Temperature (left of WiFi bars)
  // Note: ESP32 internal temp sensor is approximate.
  const float tc = temperatureRead();
  char tbuf[10];
  snprintf(tbuf, sizeof(tbuf), "%.0fC", (double)tc);
  const uint16_t tcol = (tc >= 70.0f) ? TFT_RED : (tc >= 55.0f ? TFT_ORANGE : TFT_GREEN);
  // Place temp just left of the WiFi bars; nudge a bit further left to avoid overlapping the bars.
  fbText(tbuf, bx - 28, 3, tcol, 1, false);
}

static void drawLogoPage() {
  fbFill(TFT_BLACK);
  drawTopBar("NukaMiner");
  fbText("DUINO-COIN", WIDTH/2, 26, TFT_WHITE, 1, true);
  fbText("MINER", WIDTH/2, 38, TFT_WHITE, 2, true);
  fbText("V1.0", WIDTH/2, 60, TFT_WHITE, 1, true);
}

static void drawMiningPage() {
  fbFill(TFT_BLACK);
  drawTopBar("Mining");

  // Total hashrate (core1 + optional core2)
  const uint32_t totalHash = (uint32_t)hashrate + (cfg.core2_enabled ? (uint32_t)hashrate_core_two : 0U);

  char line1[64];
  snprintf(line1, sizeof(line1), "User: %s", cfg.duco_user.c_str());
  fbText(line1, 4, 20, TFT_WHITE, 1, false);

  char line2[64];
  snprintf(line2, sizeof(line2), "Rig: %s", cfg.rig_id.c_str());
  fbText(line2, 4, 30, TFT_WHITE, 1, false);

  const double totalKh = ((double)totalHash) / 1000.0;

  char line3[64];
  snprintf(line3, sizeof(line3), "Hash: %.2f kH/s", totalKh);
  fbText(line3, 4, 42, TFT_WHITE, 1, false);

  char line4[64];
  snprintf(line4, sizeof(line4), "Diff: %u", difficulty);
  fbText(line4, 4, 52, TFT_WHITE, 1, false);

  char line5[64];
  snprintf(line5, sizeof(line5), "Shares: %lu/%lu", accepted_share_count, share_count);
  fbText(line5, 4, 62, TFT_WHITE, 1, false);

  // Always show both core hashrates on the Mining page (even if one core is disabled)
  // so the layout stays consistent and the user can see "0.0" for disabled cores.
  char line6[64];
  const double c1kh = cfg.core1_enabled ? (((double)((uint32_t)hashrate)) / 1000.0) : 0.0;
  const double c2kh = cfg.core2_enabled ? (((double)((uint32_t)hashrate_core_two)) / 1000.0) : 0.0;
  snprintf(line6, sizeof(line6), "C1:%.1f C2:%.1f kH/s", c1kh, c2kh);
  fbText(line6, 4, 72, TFT_WHITE, 1, false);
}

static void drawHashGraphPage() {
  fbFill(TFT_BLACK);
  drawTopBar("Hash Graph");

  // Graph area
  const int gx = 18;
  const int gy = 18;
  const int gw = WIDTH - gx - 4;
  // Leave room below the graph for the hashrate text
  const int gh = HEIGHT - gy - 14;

  fbFillRect(gx, gy, gw, gh, TFT_BLACK);
  fbRect(gx, gy, gw, gh, TFT_DARKGREY);

  // Find max for scaling
  uint32_t maxV = 1;
  uint16_t count = hrHistFilled ? HR_HIST_LEN : hrHistPos;
  for (uint16_t i = 0; i < count; i++) {
    uint32_t v = hrHist[i];
    if (v > maxV) maxV = v;
  }
  // Add a little headroom
  maxV = (uint32_t)(maxV * 1.15f);
  if (maxV < 1000) maxV = 1000;

  // NOTE: Y-axis labels intentionally omitted for a cleaner graph.
  char lbl[24];

  if (count < 2) {
    fbText("(collecting...)", gx + 6, gy + gh/2 - 4, TFT_ORANGE, 1, false);
    return;
  }

  // Draw line from oldest->newest. Start in middle of area when buffer not filled yet.
  int startX = gx;
  if (!hrHistFilled) {
    startX = gx + (gw / 2);
  }

  // Scale so that "maxV" uses 80% of the graph height, centered vertically.
  const int innerH = gh - 2;
  const int plotH = (innerH * 8) / 10;              // 80%
  const int padY  = (innerH - plotH) / 2;           // centered padding
  auto mapY = [&](uint32_t v) -> int {
    // Clamp and map into [gy+1+padY, gy+1+padY+plotH-1]
    if (v > maxV) v = maxV;
    const int y0 = gy + 1 + padY;
    const int y1 = y0 + plotH - 1;
    const int y  = y1 - (int)((v * (uint32_t)(plotH - 1)) / maxV);
    if (y < y0) return y0;
    if (y > y1) return y1;
    return y;
  };

  int prevX = startX;
  int prevY = mapY(hrHist[0]);

  for (uint16_t i = 1; i < count; i++) {
    int x;
    if (hrHistFilled) {
      x = gx + (int)((i * (uint32_t)(gw - 2)) / (count - 1));
    } else {
      // When not filled, plot to the right from center.
      x = startX + (int)((i * (uint32_t)(gw/2 - 2)) / (count - 1));
      if (x > gx + gw - 2) x = gx + gw - 2;
    }
    uint32_t v = hrHist[i];
    int y = mapY(v);
    fbLine(prevX, prevY, x, y, TFT_CYAN);
    prevX = x; prevY = y;
  }

  // Current hashrate label (below the graph)
  const uint32_t totalHash = (uint32_t)hashrate + (cfg.core2_enabled ? (uint32_t)hashrate_core_two : 0U);
  snprintf(lbl, sizeof(lbl), "%.2f kH/s", ((double)totalHash)/1000.0);
  fbText(lbl, gx, gy + gh + 2, TFT_WHITE, 1, false);
}

static void drawSetupPage() {
  fbFill(TFT_BLACK);
  drawTopBar("Setup");

  // Keep lines short so words don't get clipped mid-word.
  fbText("Short: Page", 4, 20, TFT_WHITE, 1, false);
  fbText("Long: Setup", 4, 32, TFT_WHITE, 1, false);
  fbText("Pass: nukaminer", 4, 44, TFT_WHITE, 1, false);
}

static void drawIpPage() {
  fbFill(TFT_BLACK);
  drawTopBar("IP Info");

  if (!WiFi.isConnected()) {
    fbText("WiFi not connected", 4, 24, TFT_RED, 1, false);
    fbText("Hold BOOT for setup", 4, 36, TFT_WHITE, 1, false);
    return;
  }

  IPAddress ip = WiFi.localIP();
  IPAddress gw = WiFi.gatewayIP();
  IPAddress sn = WiFi.subnetMask();
  int32_t rssi = WiFi.RSSI();

  char buf[64];
  snprintf(buf, sizeof(buf), "IP: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  fbText(buf, 4, 18, TFT_WHITE, 1, false);

  snprintf(buf, sizeof(buf), "GW: %u.%u.%u.%u", gw[0], gw[1], gw[2], gw[3]);
  fbText(buf, 4, 28, TFT_WHITE, 1, false);

  snprintf(buf, sizeof(buf), "SN: %u.%u.%u.%u", sn[0], sn[1], sn[2], sn[3]);
  fbText(buf, 4, 38, TFT_WHITE, 1, false);

  snprintf(buf, sizeof(buf), "RSSI: %ld dBm", (long)rssi);
  fbText(buf, 4, 50, TFT_WHITE, 1, false);

  uint8_t mac[6];
  WiFi.macAddress(mac);
  // MAC can be longer than the screen, so split into two lines
  //fbText("MAC:", 4, 60, TFT_YELLOW, 1, false);
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  fbText(buf, 4, 62, TFT_WHITE, 1, false);
}

// -----------------------------
// Button handling (short/long)
// -----------------------------
// The BOOT button is active-low. We debounce because the S3 BOOT button
// can bounce enough to register multiple "short presses" on one click.
//
// Make BOOT interrupt-driven so presses are not missed when miners are running
// hot. The ISR only captures edges; debounce + short/long detection is handled
// in a lightweight service routine.
static volatile bool     btnIsrPending = false;
static volatile bool     btnIsrRawPressed = false;
static volatile uint32_t btnIsrChangeMs = 0;

static void IRAM_ATTR bootBtnIsr() {
  // Use tick count inside ISR (millis() is not ISR-safe in all contexts).
  const uint32_t nowMs = (uint32_t)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);
  // Active-low
  btnIsrRawPressed = (digitalRead(PIN_BUTTON) == LOW);
  btnIsrChangeMs = nowMs;
  btnIsrPending = true;
}

static bool btnStable = false;        // debounced pressed state
static bool btnLastRaw = false;
static uint32_t btnLastChangeMs = 0;
static uint32_t btnDownMs = 0;
static uint32_t lastBtnActionMs = 0;
// Single-click (page) + long-press (AP mode) only.

static void handleButton() {
  // Keep debounce fairly small so fast double-clicks register.
  // BOOT can be a bit bouncy, but 30-40ms is usually enough.
  const uint32_t debounceMs = 35;
  // Consume edge updates from ISR (if any). This keeps button responsive even
  // when the main loop is delayed by mining activity.
  if (btnIsrPending) {
    noInterrupts();
    const bool raw = btnIsrRawPressed;
    const uint32_t ms = btnIsrChangeMs;
    btnIsrPending = false;
    interrupts();
    btnLastRaw = raw;
    btnLastChangeMs = ms;
  }

  bool rawPressed = btnLastRaw;
  uint32_t now = millis();

  // If no ISR edge was received (e.g., interrupts temporarily masked), fall back
  // to a cheap poll to re-sync.
  const bool polled = (digitalRead(PIN_BUTTON) == LOW);
  if (polled != btnLastRaw) {
    btnLastRaw = polled;
    btnLastChangeMs = now;
    rawPressed = polled;
  }

  // Update stable state only after it has remained unchanged for debounceMs
  if ((now - btnLastChangeMs) >= debounceMs && rawPressed != btnStable) {
    btnStable = rawPressed;

    if (btnStable) {
      // stable press down
      btnDownMs = now;
      lastInteractionMs = now;

  // If the Web UI isn't set to "always on", a physical BOOT press enables it
  // temporarily (idle timeout) so the user can reach /status and /config.
  if (cfg.web_enabled && !cfg.web_always_on && !portalRunning) {
    webSessionActive = true;
    webSessionTouch(now);
  }
      if (displaySleeping) displayWake();
      return;
    }

    // stable release
    uint32_t held = now - btnDownMs;

    // Guard against bounce/double-triggers while still allowing fast double-clicks.
    if (now - lastBtnActionMs < 25) {
      return;
    }
    lastBtnActionMs = now;
    lastInteractionMs = now;

  // If the Web UI isn't set to "always on", a physical BOOT press enables it
  // temporarily (idle timeout) so the user can reach /status and /config.
  if (cfg.web_enabled && !cfg.web_always_on && !portalRunning) {
    webSessionActive = true;
    webSessionTouch(now);
  }

    if (displaySleeping) {
      displayWake();
      return;
    }

    if (held >= 900) {
      // long press -> portal (user-invoked, keep running)
      portalStart(false);
      page = PAGE_SETUP;
    } else {
      // short press -> next page
      lastCarouselFlipMs = now;
      if (portalRunning) {
        page = PAGE_SETUP;
      } else {
        switch (page) {
          case PAGE_LOGO:   page = PAGE_MINING; break;
          case PAGE_MINING: page = PAGE_GRAPH; break;
          case PAGE_GRAPH:  page = PAGE_IP; break;
          case PAGE_IP:     page = PAGE_LOGO; break;
          case PAGE_SETUP:  page = PAGE_MINING; break;
        }
      }
    }
  }
}

// Web-triggered BOOT button "short press" (same behavior as quick physical press).
static void bootButtonShortPress() {
  uint32_t now = millis();

  // Guard against double triggers (matches physical debounce guard).
  if (now - lastBtnActionMs < 25) return;
  lastBtnActionMs = now;
  lastInteractionMs = now;

  // If the Web UI isn't set to "always on", a physical BOOT press enables it
  // temporarily (idle timeout) so the user can reach /status and /config.
  if (cfg.web_enabled && !cfg.web_always_on && !portalRunning) {
    webSessionActive = true;
    webSessionTouch(now);
  }

  if (displaySleeping) {
    displayWake();
    return;
  }

  lastCarouselFlipMs = now;

  if (portalRunning) {
    page = PAGE_SETUP;
    return;
  }

  switch (page) {
    case PAGE_LOGO:   page = PAGE_MINING; break;
    case PAGE_MINING: page = PAGE_GRAPH; break;
    case PAGE_GRAPH:  page = PAGE_IP; break;
    case PAGE_IP:     page = PAGE_LOGO; break;
    case PAGE_SETUP:  page = PAGE_MINING; break;
  }
}

// -----------------------------
// Service task implementation
// -----------------------------
static void serviceTaskFn(void *arg) {
  (void)arg;
  for (;;) {
    handleButton();
    scheduledRebootCheck();
    portalLoop();

    if (webBegun && cfg.web_enabled) {
      web.handleClient();
    }

    // 1 tick keeps CPU0 responsive without eating measurable hashrate.
    vTaskDelay(1);
  }
}

// -----------------------------
// WiFi
// -----------------------------
static void wifiConnect() {
  if (!wifiHasAnyConfig()) {
    Serial.println("[NukaMiner] No saved WiFi configuration");
    return;
  }

  // STA first (normal mode)
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  // Decide what to connect to:
  //  1) Prefer the highest-priority *visible* saved profile (tie-break: RSSI).
  //  2) If priorities tie, prefer the last successfully connected SSID (wifi_last).
  //  3) If we can't scan (or no matches), fall back to wifi_last, then highest-priority saved profile.
  WifiProfile* last = nullptr;
  int lastRssi = -9999;
  if (wifiLastSsid.length()) {
    last = wifiProfileBySsid(wifiLastSsid);
  }

  WifiProfile* bestVisible = nullptr;
  int bestPrio = -32768;
  int bestRssi = -9999;

  // Quick scan to pick the best *visible* profile by priority, then RSSI.
  // Keep this short so boot stays responsive.
  if (!wifiProfiles.empty()) {
    WiFi.scanDelete();
    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
    if (n > 0) {
      for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        int r = WiFi.RSSI(i);

        if (last && s == last->ssid) {
          lastRssi = r;
        }

        WifiProfile* p = wifiProfileBySsid(s);
        if (!p) continue;

        int pr = (int)p->prio;
        if (!bestVisible || pr > bestPrio || (pr == bestPrio && r > bestRssi)) {
          bestVisible = p;
          bestPrio = pr;
          bestRssi = r;
        }
      }
    }
    WiFi.scanDelete();
  }

  // Choose between bestVisible and last (stickiness only when priorities tie).
  WifiProfile* chosen = nullptr;
  if (bestVisible) {
    if (last && lastRssi > -9990 && (int)last->prio == bestPrio) {
      chosen = last; // same priority -> keep last good SSID
    } else {
      chosen = bestVisible; // priority wins
    }
  } else if (last) {
    chosen = last;
  }

  // Fallback: highest priority profile.
  if (!chosen && !wifiProfiles.empty()) {
    wifiProfilesSort();
    chosen = &wifiProfiles[0];
  }

  // Legacy fallback.
  if (!chosen) {
    Serial.printf("[NukaMiner] WiFi begin (legacy) SSID='%s'\n", cfg.wifi_ssid.c_str());
    WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());
    return;
  }

  // Mirror into cfg fields for UI/backups.
  cfg.wifi_ssid = chosen->ssid;
  cfg.wifi_pass = chosen->pass;

  Serial.printf("[NukaMiner] WiFi begin SSID='%s' (prio=%d)\n", chosen->ssid.c_str(), (int)chosen->prio);
  WiFi.begin(chosen->ssid.c_str(), chosen->pass.c_str());
}

static void maybeStartPortalIfNeeded() {
  // If no saved WiFi, start AP portal immediately.
  if (!wifiHasAnyConfig()) {
    Serial.println("[NukaMiner] Starting portal (no WiFi configured)");
    portalStart(true);
    return;
  }

  // Give STA a fair chance. DHCP + association can take >15s on some networks.
  const uint32_t connectWindowMs = 45000;
  uint32_t start = millis();
  uint32_t lastPrint = 0;
  while (!WiFi.isConnected() && millis() - start < connectWindowMs) {
    if (millis() - lastPrint > 1000) {
      lastPrint = millis();
      wl_status_t st = WiFi.status();
      Serial.printf("[NukaMiner] WiFi status=%d\n", (int)st);
    }
    handleButton();
    portalLoop();
    delay(50);
  }

  if (WiFi.isConnected()) {
    Serial.printf("[NukaMiner] WiFi connected: %s\n", WiFi.localIP().toString().c_str());
    return;
  }

  // Could not connect. Start portal as AP+STA so user can fix settings,
  // but keep trying to connect in the background.
  Serial.println("[NukaMiner] WiFi not connected - starting portal (AP+STA fallback)");
  WiFi.mode(WIFI_AP_STA);
  if (cfg.wifi_ssid.length()) WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());
  portalStart(true);
}

static void portalStop() {
  if (!portalRunning) return;
  dns.stop();
  // Keep web server running; it will now serve normal UI on STA.
  // Turn off AP to reduce interference once STA is up.
  WiFi.softAPdisconnect(true);

  // Restore the user's Web UI setting if we force-enabled it for portal recovery.
  if (portalForcedWeb) {
    cfg.web_enabled = webEnabledBeforePortal;
    portalForcedWeb = false;
  }

  portalRunning = false;
  portalAuto = false;
  // Resume miner tasks that were suspended for AP/Portal mode.
  minerResumeAfterPortal();
  Serial.println("[NukaMiner] Portal stopped");
}

// -----------------------------
// Arduino
// -----------------------------
void setup() {
  // Capture reset reason early (for Status page)
  g_resetReason = esp_reset_reason();

  pinMode(PIN_BUTTON, INPUT_PULLUP);

  // Interrupt-driven BOOT button capture so presses are not missed while mining.
  // Use CHANGE so we see both press and release for long-press detection.
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), bootBtnIsr, CHANGE);

  // Initialize debounce state to current button level
  btnLastRaw = (digitalRead(PIN_BUTTON) == LOW);
  btnStable = btnLastRaw;
  btnLastChangeMs = millis();

  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("[NukaMiner] Boot");
  Serial.printf("[NukaMiner] Free heap at boot: %u\n", (unsigned)ESP.getFreeHeap());

  Serial.println("[NukaMiner] Display init...");
  Serial.println("[NukaMiner] Framebuffer alloc...");

  fbFront = (uint16_t*)malloc(WIDTH * HEIGHT * sizeof(uint16_t));
  fbBack  = (uint16_t*)malloc(WIDTH * HEIGHT * sizeof(uint16_t));
  if (!fbFront || !fbBack) {
    Serial.println("[NukaMiner] FATAL: framebuffer alloc failed");
    while(true) { delay(1000); }
  }
  // Draw into back buffer; serve front buffer to web.
  fb = fbBack;
  // Initialize both buffers to black so the first web frame is valid.
  for (int i = 0; i < WIDTH * HEIGHT; i++) { fbFront[i] = 0; fbBack[i] = 0; }

  // Start backlight at a safe default until config is loaded (don't mutate config)
  blSet(50, false);
  delay(10);
  Serial.println("[NukaMiner] Calling tft.init()...");
  tft.init();
  Serial.println("[NukaMiner] tft.init ok");
  // IMPORTANT: Our full-screen framebuffer is stored as native-endian RGB565.
  // TFT_eSPI's fast pushPixels() path on ESP32-S3 streams the underlying bytes.
  // Enable swapBytes so colors render correctly (otherwise red/blue/yellow/cyan get permuted).
  tft.setSwapBytes(true);
  tft.setRotation(1); // landscape 160x80 (updated after config load)
  tft.fillScreen(TFT_BLACK);


  Serial.printf("[NukaMiner] Framebuffer OK (%u bytes). Free heap: %u\n",
                (unsigned)(WIDTH * HEIGHT * sizeof(uint16_t)), (unsigned)ESP.getFreeHeap());

  Serial.println("[NukaMiner] Load config...");

  loadConfig();

  // Load WiFi profiles (and migrate legacy single-SSID settings if needed)
  wifiProfilesLoad();

  // Apply hashrate limiter immediately (mining code reads this global)
  NM_hash_limit_pct = cfg.hash_limit_pct; // alias
  NM_hash_limit_pct_job0 = cfg.hash_limit_pct;
  NM_hash_limit_pct_job1 = cfg.core2_enabled ? cfg.core2_hash_limit_pct : 100;

  // Apply user rotation and brightness now that config is loaded
  tft.setRotation(cfg.lcd_rot180 ? 3 : 1);
  blSet(cfg.lcd_brightness, false);

  // Init RGB LED after config is loaded
  ledInit();
  ledService();
  registerWebHandlers();

  // Start the high-priority service task on CPU0 to keep Web UI and BOOT
  // responsive under heavy mining load.
  if (!serviceTask) {
    xTaskCreatePinnedToCore(serviceTaskFn, "svc", 4096, nullptr, 3, &serviceTask, 0);
  }

  // SD restore only when explicitly requested (avoid SD errors on boards without SD)

  Serial.println("[NukaMiner] WiFi connect...");

  wifiConnect();
  maybeStartPortalIfNeeded();

  // Start Web UI only after network stack is up (prevents LWIP mbox assert on ESP32-S3)
  if (cfg.web_enabled && WiFi.isConnected() && !portalRunning) {
    if (!webBegun) { web.begin(); webBegun = true; }
  }

  // Start miner after WiFi is up (will wait for connection)
  minerStart();

  // Pick a sensible initial page.
  if (!wifiHasAnyConfig()) {
    page = PAGE_SETUP;
  } else {
    page = cfg.duino_enabled ? PAGE_MINING : PAGE_IP;
  }

  Serial.println("[NukaMiner] Setup complete");

  lastInteractionMs = millis();
}

void loop() {
  // BOOT handling, portalLoop and web.handleClient() are serviced by the
  // dedicated CPU0 service task for responsiveness during mining.

  // Auto-cycle LCD pages (only when not in AP/portal)
  if (!portalRunning && !deviceControlMode && cfg.carousel_enabled && cfg.carousel_seconds > 0 && !displaySleeping) {
    uint32_t now = millis();
    uint32_t period = (uint32_t)cfg.carousel_seconds * 1000UL;
    if (now - lastCarouselFlipMs >= period) {
      lastCarouselFlipMs = now;
      switch (page) {
        case PAGE_LOGO:   page = PAGE_MINING; break;
        case PAGE_MINING: page = PAGE_GRAPH; break;
        case PAGE_GRAPH:  page = PAGE_IP; break;
        // include the Duino-Coin splash/info page in the normal rotation
        case PAGE_IP:     page = PAGE_LOGO; break;
        case PAGE_SETUP:  page = PAGE_MINING; break;
      }
    }
  }
  // portalLoop/web handled in serviceTaskFn

  // If we started the portal as a fallback (AP+STA) and STA later connects,
  // shut down AP and switch to normal operation.
  if (portalRunning && portalAuto && WiFi.isConnected()) {
    portalStop();
    // Start Web UI now that STA is up
    if (cfg.web_enabled && !webBegun) { web.begin(); webBegun = true; }
    // Miner task may already be running; if not, start it.
    if (!minerIsRunning()) minerStart();

    // If we were showing setup instructions because portal was active,
    // switch to a normal info page after the device is connected.
    if (page == PAGE_SETUP) {
      page = cfg.duino_enabled ? PAGE_MINING : PAGE_IP;
    }
  }

  // -----------------------------
  // WiFi reconnect watchdog
  // If WiFi drops while mining, attempt to reconnect. After 5 failed attempts
  // (spaced out) reboot to recover from DNS/SSL stack issues.
  // -----------------------------
  if (!portalRunning && wifiHasAnyConfig()) {
    if (WiFi.status() == WL_CONNECTED) {
      wifiReconnectFails = 0;
      // Remember last successful SSID to speed up future boots.
      String cur = WiFi.SSID();
      if (cur.length() && cur != wifiLastSsid) {
        wifiLastSsid = cur;
        wifiProfilesSave();
      }
    } else {
      uint32_t now = millis();
      if (now - lastWifiCheckMs > 1000) {
        lastWifiCheckMs = now;
        if (now - lastWifiAttemptMs > 5000) {
          lastWifiAttemptMs = now;
          wifiReconnectFails++;
          NM_log(String("[NukaMiner] WiFi disconnected, reconnect attempt ") + wifiReconnectFails);

          // Aggressive reconnect to refresh DHCP/DNS
          WiFi.disconnect(true, true);
          delay(50);
          WiFi.mode(WIFI_STA);
          WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());

          if (cfg.duino_enabled && minerIsRunning() && wifiReconnectFails >= 5) {
            NM_log("[NukaMiner] WiFi reconnect failed 5 times while mining - rebooting");
            delay(200);
            ESP.restart();
          }
        }
      }
    }
  }

  // Sample hashrate for LCD graph (once per second)
  {
    uint32_t now = millis();
    if (now - lastHrSampleMs >= 1000) {
      lastHrSampleMs = now;
      const uint32_t totalHash = (uint32_t)hashrate + (cfg.core2_enabled ? (uint32_t)hashrate_core_two : 0U);
      if (hrHistPos < HR_HIST_LEN) {
        hrHist[hrHistPos++] = totalHash;
        if (hrHistPos == HR_HIST_LEN) hrHistFilled = true;
      } else {
        // shift left and append newest
        memmove(&hrHist[0], &hrHist[1], sizeof(uint32_t) * (HR_HIST_LEN - 1));
        hrHist[HR_HIST_LEN - 1] = totalHash;
        hrHistFilled = true;
        hrHistPos = HR_HIST_LEN;
      }
    }
  }

  // Carousel mode: auto-cycle pages when not in AP/portal
  if (!portalRunning && cfg.carousel_enabled && cfg.carousel_seconds >= 2 && !displaySleeping) {
    uint32_t now = millis();
    if (lastCarouselFlipMs == 0) lastCarouselFlipMs = now;
    if (now - lastCarouselFlipMs >= (uint32_t)cfg.carousel_seconds * 1000UL) {
      lastCarouselFlipMs = now;
      // advance page (skip setup page in normal mode)
      switch (page) {
        case PAGE_LOGO:   page = PAGE_MINING; break;
        case PAGE_MINING: page = PAGE_GRAPH; break;
        case PAGE_GRAPH:  page = PAGE_IP; break;
        // include the Duino-Coin splash/info page in the normal rotation
        case PAGE_IP:     page = PAGE_LOGO; break;
        case PAGE_SETUP:  page = PAGE_MINING; break;
      }
    }
  }

  // Display sleep
  if (!displaySleeping && cfg.display_sleep_s > 0) {
    if (millis() - lastInteractionMs > cfg.display_sleep_s * 1000UL) {
      displaySleep();
    }
  }

  if (!displaySleeping) {
    switch(page) {
      case PAGE_LOGO:   drawLogoPage(); break;
      case PAGE_MINING: drawMiningPage(); break;
      case PAGE_GRAPH:  drawHashGraphPage(); break;
      case PAGE_SETUP:  drawSetupPage(); break;
      case PAGE_IP:     drawIpPage(); break;
    }
    fbPush();
  }

  // Update RGB LED state
  ledService();

  delay(100);
}
