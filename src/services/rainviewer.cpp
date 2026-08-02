#include "rainviewer.h"

#include <ArduinoJson.h>

#include "config.h"
#include "http_fetch.h"
#include "zoom_ctrl.h"

struct PastCache {
  String host;
  RainviewerFrame frames[ANIM_MAX_FRAMES];
  int count = 0;
  uint32_t ms = 0;
};

static PastCache s_pastCache;
static const uint32_t kMetaCacheTtlMs = 60UL * 1000UL;

/** 将 past 数组过滤进 cache（旧→新，最多 ANIM_MAX_FRAMES）。 */
static bool fillPastCache(const char* host, JsonArray past, float windowHours,
                          PastCache* cache) {
  if (!host || !host[0] || !cache) {
    return false;
  }

  uint32_t newest = 0;
  for (JsonObject item : past) {
    const uint32_t t = item["time"] | 0;
    const char* path = item["path"] | "";
    if (path[0] && t > newest) {
      newest = t;
    }
  }
  if (newest == 0) {
    Serial.println("RainViewer: no valid past entries");
    return false;
  }

  const float hours = windowHours > 0.0f ? windowHours : ANIM_WINDOW_HOURS;
  const uint32_t cutoff = newest - (uint32_t)(hours * 3600.0f);

  cache->host = host;
  cache->count = 0;
  for (JsonObject item : past) {
    const char* path = item["path"] | "";
    const uint32_t t = item["time"] | 0;
    if (!path[0] || t == 0 || t < cutoff) {
      continue;
    }
    RainviewerFrame fr;
    fr.host = host;
    fr.path = path;
    fr.time = t;
    if (cache->count < ANIM_MAX_FRAMES) {
      cache->frames[cache->count++] = fr;
    } else {
      for (int j = 1; j < ANIM_MAX_FRAMES; ++j) {
        cache->frames[j - 1] = cache->frames[j];
      }
      cache->frames[ANIM_MAX_FRAMES - 1] = fr;
    }
  }
  return cache->count > 0;
}

static bool rainviewerFetchPastOnce(PastCache* cache) {
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

  return fillPastCache(host, past, ANIM_WINDOW_HOURS, cache);
}

static bool ensurePastCache() {
  if (s_pastCache.ms != 0 && (millis() - s_pastCache.ms) < kMetaCacheTtlMs &&
      s_pastCache.count > 0 && s_pastCache.host.length() > 0) {
    return true;
  }

  for (int attempt = 1; attempt <= 3; ++attempt) {
    PastCache tmp;
    if (rainviewerFetchPastOnce(&tmp)) {
      s_pastCache = tmp;
      s_pastCache.ms = millis() == 0 ? 1 : millis();
      Serial.printf("RainViewer past host=%s frames=%d newest=%lu\n",
                    s_pastCache.host.c_str(), s_pastCache.count,
                    (unsigned long)s_pastCache.frames[s_pastCache.count - 1].time);
      return true;
    }
    Serial.printf("RainViewer past fail attempt %d/3\n", attempt);
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

bool rainviewerFetchLatest(RainviewerFrame* out) {
  if (!out) {
    return false;
  }
  if (!ensurePastCache()) {
    return false;
  }
  *out = s_pastCache.frames[s_pastCache.count - 1];
  Serial.printf("RainViewer latest host=%s path=%s time=%lu\n",
                out->host.c_str(), out->path.c_str(),
                (unsigned long)out->time);
  return true;
}

bool rainviewerFetchPast(RainviewerFrame* out, int maxOut, int* outCount,
                         float windowHours) {
  if (!out || !outCount || maxOut <= 0) {
    return false;
  }
  *outCount = 0;
  if (!ensurePastCache()) {
    return false;
  }

  const uint32_t newest = s_pastCache.frames[s_pastCache.count - 1].time;
  const float hours = windowHours > 0.0f ? windowHours : ANIM_WINDOW_HOURS;
  const uint32_t cutoff = newest - (uint32_t)(hours * 3600.0f);

  int n = 0;
  RainviewerFrame tmp[ANIM_MAX_FRAMES];
  for (int i = 0; i < s_pastCache.count; ++i) {
    if (s_pastCache.frames[i].time < cutoff) {
      continue;
    }
    if (n < ANIM_MAX_FRAMES) {
      tmp[n++] = s_pastCache.frames[i];
    } else {
      for (int j = 1; j < ANIM_MAX_FRAMES; ++j) {
        tmp[j - 1] = tmp[j];
      }
      tmp[ANIM_MAX_FRAMES - 1] = s_pastCache.frames[i];
    }
  }
  if (n <= 0) {
    return false;
  }

  const int copyN = n > maxOut ? maxOut : n;
  const int start = n > maxOut ? (n - maxOut) : 0;
  for (int i = 0; i < copyN; ++i) {
    out[i] = tmp[start + i];
  }
  *outCount = copyN;
  Serial.printf("RainViewer past out=%d window=%.1fh\n", copyN, hours);
  return true;
}

String rainviewerTileUrl(const RainviewerFrame& frame, int zoom, int tx, int ty) {
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
