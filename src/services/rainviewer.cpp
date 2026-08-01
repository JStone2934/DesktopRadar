#include "rainviewer.h"

#include <ArduinoJson.h>

#include "config.h"
#include "http_fetch.h"
#include "zoom_ctrl.h"

static RainviewerFrame s_metaCache;
static uint32_t s_metaCacheMs = 0;
static const uint32_t kMetaCacheTtlMs = 10UL * 60UL * 1000UL;

static bool rainviewerFetchLatestOnce(RainviewerFrame* out) {
  size_t len = 0;
  uint8_t* data = httpFetch(RAINVIEWER_API, nullptr, HTTP_MAX_JSON_BYTES, &len,
                            HTTP_TIMEOUT_MS);
  if (!data) {
    return false;
  }

  JsonDocument filter;
  filter["host"] = true;
  filter["radar"]["past"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, data, len, DeserializationOption::Filter(filter));
  free(data);
  if (err) {
    Serial.printf("RainViewer JSON error: %s\n", err.c_str());
    return false;
  }

  const char* host = doc["host"] | "";
  JsonArray past = doc["radar"]["past"].as<JsonArray>();
  if (!host[0] || past.isNull() || past.size() == 0) {
    Serial.println("RainViewer: empty past frames");
    return false;
  }

  JsonObject last = past[past.size() - 1].as<JsonObject>();
  const char* path = last["path"] | "";
  const uint32_t t = last["time"] | 0;
  if (!path[0]) {
    Serial.println("RainViewer: missing path");
    return false;
  }

  out->host = host;
  out->path = path;
  out->time = t;
  return true;
}

bool rainviewerFetchLatest(RainviewerFrame* out) {
  if (!out) {
    return false;
  }

  if (s_metaCacheMs != 0 && (millis() - s_metaCacheMs) < kMetaCacheTtlMs &&
      s_metaCache.path.length() > 0 && s_metaCache.host.length() > 0) {
    *out = s_metaCache;
    Serial.printf("RainViewer cached host=%s path=%s time=%lu\n",
                  out->host.c_str(), out->path.c_str(),
                  (unsigned long)out->time);
    return true;
  }

  for (int attempt = 1; attempt <= 3; ++attempt) {
    if (rainviewerFetchLatestOnce(out)) {
      s_metaCache = *out;
      s_metaCacheMs = millis();
      Serial.printf("RainViewer host=%s path=%s time=%lu\n", out->host.c_str(),
                    out->path.c_str(), (unsigned long)out->time);
      return true;
    }
    Serial.printf("RainViewer meta fail attempt %d/3\n", attempt);
    for (int i = 0; i < 40 * attempt; ++i) {
      inputServiceDuringBlock();
      if (composeAbortRequested()) {
        return false;
      }
      delay(10);
    }
  }
  return false;
}

String rainviewerTileUrl(const RainviewerFrame& frame, int zoom, int tx, int ty) {
  // {host}{path}/256/{z}/{x}/{y}/4/1_1.png
  String url;
  url.reserve(frame.host.length() + frame.path.length() + 48);
  url += frame.host;
  url += frame.path;
  url += "/256/";
  url += zoom;
  url += '/';
  url += tx;
  url += '/';
  url += ty;
  url += "/4/1_1.png";
  return url;
}
