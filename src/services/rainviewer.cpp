#include "rainviewer.h"

#include <ArduinoJson.h>

#include "config.h"
#include "http_fetch.h"

bool rainviewerFetchLatest(RainviewerFrame* out) {
  if (!out) {
    return false;
  }

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
  Serial.printf("RainViewer host=%s path=%s time=%lu\n", host, path,
                (unsigned long)t);
  return true;
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
