#include "wifi_sta.h"

#include <WiFi.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

#if __has_include("esp_eap_client.h")
#include "esp_eap_client.h"
#define RADAR_HAS_WPA2_ENT 2
#elif __has_include("esp_wpa2.h")
#include "esp_wpa2.h"
#define RADAR_HAS_WPA2_ENT 1
#else
#define RADAR_HAS_WPA2_ENT 0
#endif

static volatile bool s_eventGotIp = false;
static volatile bool s_eventStaConnected = false;
static volatile bool s_eventStaDisconnected = false;
static volatile uint8_t s_lastDisconnectReason = 0;
static volatile int8_t s_lastDisconnectRssi = 0;
static volatile uint32_t s_disconnectSequence = 0;
static bool s_eventRegistered = false;

struct ApChoice {
  bool found;
  uint8_t bssid[6];
  uint8_t channel;
  int32_t rssi;
  wifi_auth_mode_t auth;
};

enum class PeapAirKind : uint8_t {
  AuthResponse = 1,
  AssocResponse,
  ReassocResponse,
  Deauth,
  Disassoc,
  Eap,
  EapolStart,
  EapolKey,
};

struct PeapAirEvent {
  uint32_t atMs;
  int8_t rssi;
  PeapAirKind kind;
  uint8_t a;
  uint8_t b;
  uint16_t value;
};

struct PeapAirSummary {
  uint16_t authOk;
  uint16_t authFail;
  uint16_t assocOk;
  uint16_t assocFail;
  uint16_t eapIdentity;
  uint16_t eapPeap;
  uint16_t eapSuccess;
  uint16_t eapFailure;
  uint16_t keyM1;
  uint16_t keyM3;
  uint16_t deauth;
  uint16_t disassoc;
  uint16_t dropped;
};

static constexpr uint8_t kPeapAirRingSize = 32;
static PeapAirEvent s_peapAirRing[kPeapAirRingSize]{};
static volatile uint8_t s_peapAirWrite = 0;
static volatile uint8_t s_peapAirRead = 0;
static volatile uint16_t s_peapAirDropped = 0;
static volatile bool s_peapAirActive = false;
static uint8_t s_peapAirMac[6]{};
static uint32_t s_peapAirStartedAt = 0;

static uint16_t readLe16(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint16_t readBe16(const uint8_t* p) {
  return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static void peapAirPush(PeapAirKind kind, int8_t rssi, uint8_t a = 0,
                        uint8_t b = 0, uint16_t value = 0) {
  const uint8_t write = s_peapAirWrite;
  const uint8_t next = (uint8_t)((write + 1U) % kPeapAirRingSize);
  if (next == s_peapAirRead) {
    s_peapAirDropped = (uint16_t)(s_peapAirDropped + 1U);
    return;
  }
  PeapAirEvent& event = s_peapAirRing[write];
  event.atMs = millis();
  event.rssi = rssi;
  event.kind = kind;
  event.a = a;
  event.b = b;
  event.value = value;
  s_peapAirWrite = next;
}

// 预编译 Arduino core 关闭了 WPA_DEBUG_PRINT。这里仅解析发给本机的
// 802.11 管理帧和 EAPOL 帧头，记录阶段/状态，不复制身份或认证载荷。
static void peapPromiscuousRx(void* raw, wifi_promiscuous_pkt_type_t type) {
  if (!s_peapAirActive || !raw ||
      (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)) {
    return;
  }
  const wifi_promiscuous_pkt_t* packet =
      static_cast<const wifi_promiscuous_pkt_t*>(raw);
  const uint8_t* frame = packet->payload;
  const size_t length = packet->rx_ctrl.sig_len;
  if (!frame || length < 24 || memcmp(frame + 4, s_peapAirMac, 6) != 0) {
    return;
  }

  const uint16_t fc = readLe16(frame);
  const uint8_t frameType = (uint8_t)((fc >> 2) & 0x03U);
  const uint8_t subtype = (uint8_t)((fc >> 4) & 0x0fU);
  const int8_t rssi = packet->rx_ctrl.rssi;

  if (frameType == 0) {
    if (subtype == 11 && length >= 30) {
      peapAirPush(PeapAirKind::AuthResponse, rssi, frame[26], frame[27],
                  readLe16(frame + 28));
    } else if ((subtype == 1 || subtype == 3) && length >= 30) {
      peapAirPush(subtype == 1 ? PeapAirKind::AssocResponse
                               : PeapAirKind::ReassocResponse,
                  rssi, 0, 0, readLe16(frame + 26));
    } else if (subtype == 12 && length >= 26) {
      peapAirPush(PeapAirKind::Deauth, rssi, 0, 0, readLe16(frame + 24));
    } else if (subtype == 10 && length >= 26) {
      peapAirPush(PeapAirKind::Disassoc, rssi, 0, 0,
                  readLe16(frame + 24));
    }
    return;
  }
  if (frameType != 2 || (fc & (1U << 14)) != 0) {
    return;
  }

  // LLC/SNAP + EtherType 0x888e。扫描小段头部可兼容 QoS/Addr4 变体。
  static constexpr uint8_t kEapolSnap[] = {0xaa, 0xaa, 0x03, 0x00,
                                           0x00, 0x00, 0x88, 0x8e};
  const size_t scanEnd = length < 96 ? length : 96;
  for (size_t offset = 24; offset + sizeof(kEapolSnap) + 4 <= scanEnd;
       ++offset) {
    if (memcmp(frame + offset, kEapolSnap, sizeof(kEapolSnap)) != 0) {
      continue;
    }
    const size_t eapol = offset + sizeof(kEapolSnap);
    const uint8_t eapolType = frame[eapol + 1];
    const uint16_t eapolLength = readBe16(frame + eapol + 2);
    if (eapol + 4U + eapolLength > length) {
      return;
    }
    if (eapolType == 0 && eapolLength >= 4) {
      const uint8_t code = frame[eapol + 4];
      const uint8_t identifier = frame[eapol + 5];
      const uint8_t eapType =
          ((code == 1 || code == 2) && eapolLength >= 5)
              ? frame[eapol + 8]
              : 0;
      peapAirPush(PeapAirKind::Eap, rssi, code, eapType, identifier);
    } else if (eapolType == 1) {
      peapAirPush(PeapAirKind::EapolStart, rssi);
    } else if (eapolType == 3 && eapolLength >= 3) {
      peapAirPush(PeapAirKind::EapolKey, rssi, frame[eapol + 4], 0,
                  readBe16(frame + eapol + 5));
    }
    return;
  }
}

static const char* eapCodeName(uint8_t code) {
  switch (code) {
    case 1:
      return "request";
    case 2:
      return "response";
    case 3:
      return "success";
    case 4:
      return "failure";
    default:
      return "unknown";
  }
}

static const char* eapTypeName(uint8_t type) {
  switch (type) {
    case 1:
      return "identity";
    case 13:
      return "TLS";
    case 21:
      return "TTLS";
    case 25:
      return "PEAP";
    case 26:
      return "MSCHAPv2";
    default:
      return "other";
  }
}

static void peapAirDrain(PeapAirSummary* summary) {
  if (!summary) {
    return;
  }
  while (s_peapAirRead != s_peapAirWrite) {
    const uint8_t read = s_peapAirRead;
    const PeapAirEvent event = s_peapAirRing[read];
    s_peapAirRead = (uint8_t)((read + 1U) % kPeapAirRingSize);
    switch (event.kind) {
      case PeapAirKind::AuthResponse:
        event.value == 0 ? ++summary->authOk : ++summary->authFail;
        Serial.printf("PEAP air +%lums auth-rsp seq=%u status=%u rssi=%d\n",
                      (unsigned long)(event.atMs - s_peapAirStartedAt),
                      (unsigned)event.a,
                      (unsigned)event.value, (int)event.rssi);
        break;
      case PeapAirKind::AssocResponse:
      case PeapAirKind::ReassocResponse:
        event.value == 0 ? ++summary->assocOk : ++summary->assocFail;
        Serial.printf("PEAP air +%lums %s status=%u rssi=%d\n",
                      (unsigned long)(event.atMs - s_peapAirStartedAt),
                      event.kind == PeapAirKind::AssocResponse ? "assoc-rsp"
                                                               : "reassoc-rsp",
                      (unsigned)event.value, (int)event.rssi);
        break;
      case PeapAirKind::Deauth:
        ++summary->deauth;
        Serial.printf("PEAP air +%lums deauth reason=%u rssi=%d\n",
                      (unsigned long)(event.atMs - s_peapAirStartedAt),
                      (unsigned)event.value,
                      (int)event.rssi);
        break;
      case PeapAirKind::Disassoc:
        ++summary->disassoc;
        Serial.printf("PEAP air +%lums disassoc reason=%u rssi=%d\n",
                      (unsigned long)(event.atMs - s_peapAirStartedAt),
                      (unsigned)event.value,
                      (int)event.rssi);
        break;
      case PeapAirKind::Eap:
        if (event.a == 3) {
          ++summary->eapSuccess;
        } else if (event.a == 4) {
          ++summary->eapFailure;
        } else if (event.b == 1) {
          ++summary->eapIdentity;
        } else if (event.b == 25) {
          ++summary->eapPeap;
        }
        Serial.printf(
            "PEAP air +%lums EAP %s id=%u type=%s(%u) rssi=%d\n",
            (unsigned long)(event.atMs - s_peapAirStartedAt),
            eapCodeName(event.a),
            (unsigned)event.value, eapTypeName(event.b), (unsigned)event.b,
            (int)event.rssi);
        break;
      case PeapAirKind::EapolStart:
        Serial.printf("PEAP air +%lums EAPOL-start rssi=%d\n",
                      (unsigned long)(event.atMs - s_peapAirStartedAt),
                      (int)event.rssi);
        break;
      case PeapAirKind::EapolKey: {
        const bool install = (event.value & (1U << 6)) != 0;
        const bool ack = (event.value & (1U << 7)) != 0;
        const bool mic = (event.value & (1U << 8)) != 0;
        const char* message =
            ack && !mic ? "M1" : (ack && mic && install ? "M3" : "other");
        if (strcmp(message, "M1") == 0) {
          ++summary->keyM1;
        } else if (strcmp(message, "M3") == 0) {
          ++summary->keyM3;
        }
        Serial.printf(
            "PEAP air +%lums EAPOL-key %s info=0x%04x desc=%u rssi=%d\n",
            (unsigned long)(event.atMs - s_peapAirStartedAt), message,
            (unsigned)event.value,
            (unsigned)event.a, (int)event.rssi);
        break;
      }
    }
  }
  summary->dropped += s_peapAirDropped;
  s_peapAirDropped = 0;
}

static bool peapAirStart() {
  s_peapAirWrite = 0;
  s_peapAirRead = 0;
  s_peapAirDropped = 0;
  s_peapAirStartedAt = millis();
  memset(s_peapAirRing, 0, sizeof(s_peapAirRing));
  if (esp_wifi_get_mac(WIFI_IF_STA, s_peapAirMac) != ESP_OK) {
    Serial.println("PEAP air trace: cannot read STA MAC");
    return false;
  }
  wifi_promiscuous_filter_t filter{};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  if (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK ||
      esp_wifi_set_promiscuous_rx_cb(peapPromiscuousRx) != ESP_OK ||
      esp_wifi_set_promiscuous(true) != ESP_OK) {
    Serial.println("PEAP air trace: enable failed");
    return false;
  }
  s_peapAirActive = true;
  Serial.printf("PEAP air trace enabled for %02x:%02x:%02x:%02x:%02x:%02x\n",
                s_peapAirMac[0], s_peapAirMac[1], s_peapAirMac[2],
                s_peapAirMac[3], s_peapAirMac[4], s_peapAirMac[5]);
  return true;
}

static void peapAirStop(PeapAirSummary* summary) {
  s_peapAirActive = false;
  esp_wifi_set_promiscuous(false);
  peapAirDrain(summary);
}

static void wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      s_eventStaConnected = true;
      s_eventStaDisconnected = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      s_eventGotIp = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      s_eventStaConnected = false;
      s_eventGotIp = false;
      s_eventStaDisconnected = true;
      s_lastDisconnectReason = info.wifi_sta_disconnected.reason;
      s_lastDisconnectRssi = info.wifi_sta_disconnected.rssi;
      s_disconnectSequence = s_disconnectSequence + 1U;
      Serial.printf(
          "WiFi EVENT disconnect #%lu reason=%u (%s) rssi=%d status=%d "
          "heap=%u\n",
          (unsigned long)s_disconnectSequence,
          (unsigned)s_lastDisconnectReason,
          WiFi.disconnectReasonName(
              (wifi_err_reason_t)s_lastDisconnectReason),
          (int)s_lastDisconnectRssi, (int)WiFi.status(),
          (unsigned)ESP.getFreeHeap());
      break;
    default:
      break;
  }
}

static void ensureWifiEvents() {
  if (!s_eventRegistered) {
    WiFi.onEvent(wifiEvent);
    s_eventRegistered = true;
  }
}

static void resetWifiEvents() {
  s_eventGotIp = false;
  s_eventStaConnected = false;
  s_eventStaDisconnected = false;
  s_lastDisconnectReason = 0;
  s_lastDisconnectRssi = 0;
}

static void logEspErr(const char* label, esp_err_t err) {
  if (err != ESP_OK) {
    Serial.printf("%s failed: %s (0x%x)\n", label, esp_err_to_name(err),
                  (unsigned)err);
  }
}

static void wifiDisableEnterprise() {
#if RADAR_HAS_WPA2_ENT == 1
  esp_wifi_sta_wpa2_ent_disable();
  esp_wifi_sta_wpa2_ent_clear_identity();
  esp_wifi_sta_wpa2_ent_clear_username();
  esp_wifi_sta_wpa2_ent_clear_password();
  esp_wifi_sta_wpa2_ent_clear_new_password();
  esp_wifi_sta_wpa2_ent_clear_ca_cert();
  esp_wifi_sta_wpa2_ent_clear_cert_key();
#elif RADAR_HAS_WPA2_ENT == 2
  esp_wifi_sta_enterprise_disable();
  esp_eap_client_clear_identity();
  esp_eap_client_clear_username();
  esp_eap_client_clear_password();
  esp_eap_client_clear_new_password();
  esp_eap_client_clear_ca_cert();
  esp_eap_client_clear_certificate_and_key();
#endif
}

void wifiDisconnectClean() {
  ensureWifiEvents();
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  delay(100);
  wifiDisableEnterprise();
  resetWifiEvents();
}

static void stopStaRadioClean(const char* reason) {
  ensureWifiEvents();
  WiFi.setAutoReconnect(false);

  if (WiFi.getMode() & WIFI_MODE_STA) {
    // 先终止仍在进行的连接，再清掉 PEAP supplicant 状态。Arduino 2.0.17
    // 的首次失败会暗中 WiFi.begin() 一次，因此只做轻量 disconnect 不够。
    esp_wifi_disconnect();
    delay(150);
    wifiDisableEnterprise();
  }

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    if (!WiFi.mode(WIFI_OFF)) {
      Serial.println("WiFi: failed to stop radio");
    }
  }

  const uint32_t started = millis();
  while (WiFi.getMode() != WIFI_MODE_NULL && millis() - started < 2000UL) {
    delay(25);
  }
  delay(250);
  resetWifiEvents();
  Serial.printf("WiFi radio clean stop (%s) mode=%u\n",
                reason ? reason : "retry", (unsigned)WiFi.getMode());
}

static bool prepareStaRadio() {
  ensureWifiEvents();
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);

  // 每一轮真实尝试都从完全停止的 STA/supplicant 开始，避免上一轮的
  // 隐式重连、扫描或 PEAP 会话继续占用控制块。
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    stopStaRadioClean("prepare");
  } else {
    resetWifiEvents();
  }

  bool staOk = false;
  for (uint8_t i = 0; i < 3; ++i) {
    if (WiFi.mode(WIFI_STA)) {
      staOk = true;
      break;
    }
    Serial.printf("WiFi: STA mode retry %u\n", (unsigned)(i + 1));
    delay(300);
  }
  if (!staOk) {
    Serial.println("WiFi: failed to enter STA mode after retries");
    return false;
  }

  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  // SuperMini 的板载天线/供电在持续 TLS 上行时用最高档可能触发
  // MISSING_ACKS；15 dBm 足以覆盖当前约 -60 dBm 的 AP，并用于验证
  // 是否是高功率发射造成的链路失稳。
  WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setHostname("esp32-radar");
  logEspErr("esp_wifi_set_storage", esp_wifi_set_storage(WIFI_STORAGE_RAM));
  delay(100);
  return true;
}

static const char* authName(wifi_auth_mode_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN:
      return "OPEN";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA/WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "WPA2_ENT";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2/WPA3_PSK";
    case WIFI_AUTH_WPA3_ENT_192:
      return "WPA3_ENT";
    default:
      return "UNKNOWN";
  }
}

static bool authMatchesMode(wifi_auth_mode_t auth, AppWifiMode mode) {
  if (mode == APP_WIFI_OPEN) {
    return auth == WIFI_AUTH_OPEN;
  }
  if (mode == APP_WIFI_PEAP) {
    return auth == WIFI_AUTH_WPA2_ENTERPRISE || auth == WIFI_AUTH_WPA3_ENT_192;
  }
  return auth != WIFI_AUTH_OPEN && auth != WIFI_AUTH_WPA2_ENTERPRISE &&
         auth != WIFI_AUTH_WPA3_ENT_192;
}

static ApChoice scanBestAp(const char* ssid, AppWifiMode mode,
                           uint8_t skipUsable = 0) {
  ApChoice best{};
  best.rssi = -127;
  ApChoice ranked[4]{};
  int rankedCount = 0;

  Serial.printf("WiFi scan target: ssid=%s mode=%u skip=%u\n", ssid,
                (unsigned)mode, (unsigned)skipUsable);
  const int found = WiFi.scanNetworks(false, true);
  Serial.printf("WiFi scan found %d networks\n", found);
  for (int i = 0; i < found; ++i) {
    if (WiFi.SSID(i) != ssid) {
      continue;
    }
    uint8_t* bssid = WiFi.BSSID(i);
    const int32_t rssi = WiFi.RSSI(i);
    const int32_t channel = WiFi.channel(i);
    const wifi_auth_mode_t auth = WiFi.encryptionType(i);
    const bool usable = authMatchesMode(auth, mode);
    Serial.printf("  candidate %s ch=%d rssi=%d auth=%s usable=%d\n",
                  WiFi.BSSIDstr(i).c_str(), (int)channel, (int)rssi,
                  authName(auth), (int)usable);
    if (!usable || !bssid) {
      continue;
    }

    ApChoice candidate{};
    candidate.found = true;
    memcpy(candidate.bssid, bssid, sizeof(candidate.bssid));
    candidate.channel = (uint8_t)channel;
    candidate.rssi = rssi;
    candidate.auth = auth;

    int pos = rankedCount;
    if (pos >= (int)(sizeof(ranked) / sizeof(ranked[0]))) {
      pos = (int)(sizeof(ranked) / sizeof(ranked[0])) - 1;
      if (rssi <= ranked[pos].rssi) {
        continue;
      }
    } else {
      ++rankedCount;
    }
    while (pos > 0 && rssi > ranked[pos - 1].rssi) {
      ranked[pos] = ranked[pos - 1];
      --pos;
    }
    ranked[pos] = candidate;
  }
  WiFi.scanDelete();

  if (rankedCount > 0) {
    if (skipUsable >= (uint8_t)rankedCount) {
      skipUsable = 0;
    }
    best = ranked[skipUsable];
  }

  if (best.found) {
    Serial.printf(
        "WiFi selected AP %02x:%02x:%02x:%02x:%02x:%02x ch=%u rssi=%d "
        "auth=%s rank=%u/%d\n",
        best.bssid[0], best.bssid[1], best.bssid[2], best.bssid[3],
        best.bssid[4], best.bssid[5], (unsigned)best.channel, (int)best.rssi,
        authName(best.auth), (unsigned)skipUsable, rankedCount);
  } else {
    Serial.println("WiFi target AP not found");
  }
  return best;
}

static bool setStaConfig(const char* ssid, const char* pass,
                         wifi_auth_mode_t threshold, const ApChoice* ap = nullptr,
                         uint8_t driverRetryCount = 2) {
  wifi_config_t conf;
  memset(&conf, 0, sizeof(conf));
  const size_t ssidLen = strlen(ssid);
  if (ssidLen == 0 || ssidLen > sizeof(conf.sta.ssid)) {
    Serial.printf("WiFi: invalid SSID length %u\n", (unsigned)ssidLen);
    return false;
  }
  memcpy(conf.sta.ssid, ssid, ssidLen);
  if (pass && pass[0]) {
    const size_t passLen = strlen(pass);
    if (passLen > sizeof(conf.sta.password)) {
      Serial.printf("WiFi: invalid password length %u\n", (unsigned)passLen);
      return false;
    }
    memcpy(conf.sta.password, pass, passLen);
  }
  conf.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  conf.sta.bssid_set = false;
  conf.sta.channel = 0;
  if (ap && ap->found) {
    conf.sta.bssid_set = true;
    memcpy(conf.sta.bssid, ap->bssid, sizeof(conf.sta.bssid));
    conf.sta.channel = ap->channel;
    conf.sta.scan_method = WIFI_FAST_SCAN;
  }
  conf.sta.threshold.authmode = threshold;
  conf.sta.threshold.rssi = -95;
  conf.sta.pmf_cfg.capable = true;
  conf.sta.pmf_cfg.required = false;
  conf.sta.failure_retry_cnt = driverRetryCount;

  Serial.printf(
      "WiFi STA config: ssid=%s locked=%d ch=%u driverRetries=%u\n", ssid,
      (int)conf.sta.bssid_set, (unsigned)conf.sta.channel,
      (unsigned)conf.sta.failure_retry_cnt);

  const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &conf);
  logEspErr("esp_wifi_set_config", err);
  return err == ESP_OK;
}

static bool waitForIp(uint32_t timeoutMs, const char* label,
                      PeapAirSummary* peapSummary = nullptr) {
  const uint32_t start = millis();
  uint32_t printedDisconnectSequence = 0;
  uint32_t nextDot = 0;

  while (millis() - start <= timeoutMs) {
    peapAirDrain(peapSummary);
    if (WiFi.status() == WL_CONNECTED && s_eventGotIp) {
      Serial.println();
      Serial.printf(
          "WiFi OK (%s), IP=%s GW=%s DNS=%s RSSI=%d BSSID=%s ch=%d\n",
          label, WiFi.localIP().toString().c_str(),
          WiFi.gatewayIP().toString().c_str(),
          WiFi.dnsIP().toString().c_str(), WiFi.RSSI(),
          WiFi.BSSIDstr().c_str(), (int)WiFi.channel());
      return true;
    }

    const uint8_t reason = s_lastDisconnectReason;
    const uint32_t disconnectSequence = s_disconnectSequence;
    if (s_eventStaDisconnected && reason != 0 &&
        disconnectSequence != printedDisconnectSequence) {
      printedDisconnectSequence = disconnectSequence;
      Serial.printf(
          "\nWiFi %s disconnected #%lu: reason=%u (%s) rssi=%d\n", label,
          (unsigned long)disconnectSequence, (unsigned)reason,
                    WiFi.disconnectReasonName((wifi_err_reason_t)reason),
                    (int)s_lastDisconnectRssi);
    }

    if (millis() >= nextDot) {
      Serial.print('.');
      nextDot = millis() + 500;
    }
    delay(50);
  }

  peapAirDrain(peapSummary);
  Serial.println();
  Serial.printf("WiFi %s connect timeout status=%d lastReason=%u (%s)\n", label,
                (int)WiFi.status(), (unsigned)s_lastDisconnectReason,
                s_lastDisconnectReason
                    ? WiFi.disconnectReasonName(
                          (wifi_err_reason_t)s_lastDisconnectReason)
                    : "none");
  return false;
}

static bool connectWithAttempts(const char* label, uint8_t attempts,
                                uint32_t timeoutMs, uint32_t retryBaseMs,
                                bool (*beginAttempt)(void*, uint8_t),
                                void* ctx) {
  if (!prepareStaRadio()) {
    return false;
  }

  for (uint8_t attempt = 1; attempt <= attempts; ++attempt) {
    Serial.printf("WiFi %s attempt %u/%u\n", label, (unsigned)attempt,
                  (unsigned)attempts);
    if (!beginAttempt(ctx, attempt)) {
      Serial.printf("WiFi %s attempt %u setup failed\n", label,
                    (unsigned)attempt);
    } else if (waitForIp(timeoutMs, label)) {
      return true;
    }

    if (attempt >= attempts) {
      break;
    }

    // 轻量重试：保留 STA 驱动和 PEAP 凭据，只断开当前关联并清理事件。
    // 下一次 esp_wifi_connect() 会按 SSID 重新做全信道扫描和自动选 AP。
    WiFi.disconnect(false, false);
    delay(retryBaseMs + attempt * retryBaseMs);
    resetWifiEvents();
    Serial.printf("WiFi %s retry without radio re-init\n", label);
  }

  stopStaRadioClean("all attempts failed");
  return false;
}

struct PskContext {
  const AppConfig* cfg;
};

static bool beginPskAttempt(void* raw, uint8_t attempt) {
  const PskContext* ctx = static_cast<const PskContext*>(raw);
  const AppConfig* cfg = ctx->cfg;
  WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
  if (attempt == 1) {
    (void)scanBestAp(cfg->ssid, APP_WIFI_PSK);
  }
  if (!setStaConfig(cfg->ssid, cfg->pass, WIFI_AUTH_WPA_PSK)) {
    return false;
  }
  const esp_err_t err = esp_wifi_connect();
  logEspErr("esp_wifi_connect", err);
  return err == ESP_OK;
}

static bool wifiConnectPsk(AppConfig* cfg) {
  if (!cfg) {
    return false;
  }

  const size_t passLen = strnlen(cfg->pass, sizeof(cfg->pass));
  Serial.printf("WiFi PSK connecting to %s (passLen=%u)\n", cfg->ssid,
                (unsigned)passLen);
  if (passLen == 0) {
    Serial.println("WiFi PSK: password empty");
    return false;
  }

  PskContext ctx{cfg};
  if (connectWithAttempts("PSK", 2, WIFI_CONNECT_TIMEOUT_MS, 500,
                          beginPskAttempt, &ctx)) {
    return true;
  }

  if (strcmp(cfg->ssid, WIFI_SSID) == 0 && strcmp(cfg->pass, WIFI_PASS) != 0) {
    Serial.println("WiFi PSK: retry with config.h WIFI_PASS and repair NVS");
    char oldPass[sizeof(cfg->pass)];
    snprintf(oldPass, sizeof(oldPass), "%s", cfg->pass);
    snprintf(cfg->pass, sizeof(cfg->pass), "%s", WIFI_PASS);
    if (connectWithAttempts("PSK fallback", 1, WIFI_CONNECT_TIMEOUT_MS, 500,
                            beginPskAttempt, &ctx)) {
      if (appConfigSave(cfg)) {
        Serial.println("WiFi PSK: NVS password repaired");
      }
      return true;
    }
    snprintf(cfg->pass, sizeof(cfg->pass), "%s", oldPass);
  }
  return false;
}

struct OpenContext {
  const AppConfig* cfg;
};

static bool beginOpenAttempt(void* raw, uint8_t attempt) {
  const OpenContext* ctx = static_cast<const OpenContext*>(raw);
  WiFi.setMinSecurity(WIFI_AUTH_OPEN);
  if (attempt == 1) {
    (void)scanBestAp(ctx->cfg->ssid, APP_WIFI_OPEN);
  }
  if (!setStaConfig(ctx->cfg->ssid, nullptr, WIFI_AUTH_OPEN)) {
    return false;
  }
  const esp_err_t err = esp_wifi_connect();
  logEspErr("esp_wifi_connect", err);
  return err == ESP_OK;
}

static bool wifiConnectOpen(const AppConfig& cfg) {
  Serial.printf("WiFi OPEN connecting to %s (MAC whitelist)\n", cfg.ssid);
  OpenContext ctx{&cfg};
  return connectWithAttempts("OPEN", 2, WIFI_CONNECT_TIMEOUT_MS, 500,
                             beginOpenAttempt, &ctx);
}

struct PeapContext {
  const AppConfig* cfg;
  ApChoice fixedAp;
  bool useOuterIdentity;
  bool useBssidLock;
};

static bool setPeapCredentials(const AppConfig& cfg, bool useOuterIdentity) {
#if RADAR_HAS_WPA2_ENT == 0
  (void)cfg;
  Serial.println("WiFi PEAP: enterprise API not available in this core");
  return false;
#else
  const char* outer = cfg.outer_identity[0] ? cfg.outer_identity : cfg.identity;
  const size_t outerLen = useOuterIdentity ? strlen(outer) : 0;
  const size_t userLen = strlen(cfg.identity);
  const size_t passLen = strlen(cfg.pass);

#if RADAR_HAS_WPA2_ENT == 1
  logEspErr("wpa2 disable time check",
            esp_wifi_sta_wpa2_ent_set_disable_time_check(true));
  if (useOuterIdentity) {
    logEspErr("wpa2 set identity",
              esp_wifi_sta_wpa2_ent_set_identity(
                  reinterpret_cast<const unsigned char*>(outer), outerLen));
  } else {
    esp_wifi_sta_wpa2_ent_clear_identity();
  }
  logEspErr("wpa2 set username",
            esp_wifi_sta_wpa2_ent_set_username(
                reinterpret_cast<const unsigned char*>(cfg.identity), userLen));
  logEspErr("wpa2 set password",
            esp_wifi_sta_wpa2_ent_set_password(
                reinterpret_cast<const unsigned char*>(cfg.pass), passLen));
  const esp_err_t err = esp_wifi_sta_wpa2_ent_enable();
  logEspErr("wpa2 enable", err);
  return err == ESP_OK;
#else
  logEspErr("eap disable time check", esp_eap_client_set_disable_time_check(true));
  logEspErr("eap methods", esp_eap_client_set_eap_methods(ESP_EAP_TYPE_PEAP));
  if (useOuterIdentity) {
    logEspErr("eap set identity",
              esp_eap_client_set_identity(
                  reinterpret_cast<const uint8_t*>(outer), outerLen));
  } else {
    esp_eap_client_clear_identity();
  }
  logEspErr("eap set username",
            esp_eap_client_set_username(
                reinterpret_cast<const uint8_t*>(cfg.identity), userLen));
  logEspErr("eap set password",
            esp_eap_client_set_password(
                reinterpret_cast<const uint8_t*>(cfg.pass), passLen));
  const esp_err_t err = esp_wifi_sta_enterprise_enable();
  logEspErr("enterprise enable", err);
  return err == ESP_OK;
#endif
#endif
}

static bool beginPeapAttempt(void* raw, uint8_t attempt) {
  (void)attempt;
  PeapContext* ctx = static_cast<PeapContext*>(raw);
  const AppConfig* cfg = ctx->cfg;
  const char* outer =
      cfg->outer_identity[0] ? cfg->outer_identity : cfg->identity;

  WiFi.setMinSecurity(WIFI_AUTH_OPEN);
  if (!ctx->fixedAp.found) {
    ctx->fixedAp = scanBestAp(cfg->ssid, APP_WIFI_PEAP, 0);
    if (!ctx->fixedAp.found) {
      Serial.println("WiFi PEAP A/B: strongest AP not found");
      return false;
    }
  } else if (ctx->useBssidLock) {
    const ApChoice& ap = ctx->fixedAp;
    Serial.printf(
        "WiFi PEAP A/B reuse AP %02x:%02x:%02x:%02x:%02x:%02x ch=%u\n",
        ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4],
        ap.bssid[5], (unsigned)ap.channel);
  } else {
    Serial.println("WiFi PEAP diagnostic: auto BSSID/all-channel selection");
  }

  if (!setStaConfig(cfg->ssid, nullptr, WIFI_AUTH_OPEN,
                    ctx->useBssidLock ? &ctx->fixedAp : nullptr, 0)) {
    return false;
  }
  // stopStaRadioClean() 会清掉 supplicant 全局状态；每个真实尝试必须重载。
  if (!setPeapCredentials(*cfg, ctx->useOuterIdentity)) {
    return false;
  }

  const char* variant = !ctx->useBssidLock
                            ? "C(auto-BSSID)"
                            : (ctx->useOuterIdentity ? "B(username-outer)"
                                                     : "A(no-outer)");
  Serial.printf("WiFi PEAP diagnostic configured: variant=%s ssid=%s outer=%s "
                "user=%s passLen=%u credentials=reloaded locked=%d\n",
                variant,
                cfg->ssid, ctx->useOuterIdentity ? outer : "<unset>",
                cfg->identity,
                (unsigned)strnlen(cfg->pass, sizeof(cfg->pass)),
                (int)ctx->useBssidLock);

  const esp_err_t err = esp_wifi_connect();
  logEspErr("esp_wifi_connect", err);
  return err == ESP_OK;
}

static bool wifiConnectPeap(const AppConfig& cfg) {
#if RADAR_HAS_WPA2_ENT == 0
  Serial.println("WiFi PEAP: enterprise API not available in this core");
  return false;
#else
  if (cfg.identity[0] == '\0') {
    Serial.println("WiFi PEAP: username empty");
    return false;
  }
  const size_t passLen = strnlen(cfg.pass, sizeof(cfg.pass));
  if (passLen == 0) {
    Serial.println("WiFi PEAP: password empty");
    return false;
  }

  // 3.x 最小连接路径：不预扫描、不锁 BSSID、不开混杂抓包，也不直接写
  // wifi_config_t。由 Arduino 的企业网入口一次性配置 PEAP 并选 AP。
  if (!prepareStaRadio()) {
    return false;
  }
  WiFi.setMinSecurity(WIFI_AUTH_OPEN);
  const char* outer =
      cfg.outer_identity[0] ? cfg.outer_identity : cfg.identity;
  Serial.printf(
      "WiFi PEAP minimal: ssid=%s outer=%s user=%s passLen=%u "
      "bssid=auto trace=off\n",
      cfg.ssid, outer, cfg.identity, (unsigned)passLen);
  const wl_status_t started =
      WiFi.begin(cfg.ssid, WPA2_AUTH_PEAP, outer, cfg.identity, cfg.pass);
  Serial.printf("WiFi PEAP minimal begin status=%d\n", (int)started);
  if (started == WL_CONNECT_FAILED) {
    stopStaRadioClean("PEAP minimal setup failed");
    return false;
  }
  if (waitForIp(45000UL, "PEAP-minimal")) {
    return true;
  }
  stopStaRadioClean("PEAP minimal timeout");
  return false;
#endif
}

bool wifiConnect(AppConfig* cfg) {
  if (!cfg || cfg->ssid[0] == '\0') {
    Serial.println("WiFi: empty SSID");
    return false;
  }
  if (cfg->wifi_mode == APP_WIFI_PEAP) {
    return wifiConnectPeap(*cfg);
  }
  if (cfg->wifi_mode == APP_WIFI_OPEN) {
    return wifiConnectOpen(*cfg);
  }
  return wifiConnectPsk(cfg);
}
