#include "http_fetch.h"

#include <FS.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <stdlib.h>
#include <string.h>

#include "zoom_ctrl.h"

static bool httpSetup(HTTPClient& http, WiFiClientSecure& client, const char* url,
                      const char* referer, uint32_t timeoutMs) {
  client.setInsecure();
  client.setTimeout(timeoutMs / 1000);
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(timeoutMs);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    Serial.printf("http begin failed: %s\n", url);
    return false;
  }
  if (referer && referer[0]) {
    http.addHeader("Referer", referer);
  }
  http.addHeader("User-Agent", "ESP32Radar/1.0");
  http.addHeader("Accept-Encoding", "identity");
  return true;
}

uint8_t* httpFetch(const char* url, const char* referer, size_t maxBytes,
                   size_t* outLen, uint32_t timeoutMs) {
  if (outLen) {
    *outLen = 0;
  }
  if (!url || !outLen || maxBytes == 0) {
    return nullptr;
  }

  WiFiClientSecure client;
  HTTPClient http;
  if (!httpSetup(http, client, url, referer, timeoutMs)) {
    return nullptr;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP %d\n", code);
    http.end();
    client.stop();
    return nullptr;
  }

  const int contentLen = http.getSize();
  if (contentLen > (int)maxBytes) {
    http.end();
    client.stop();
    return nullptr;
  }

  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    http.end();
    client.stop();
    return nullptr;
  }

  size_t cap = (contentLen > 0) ? (size_t)contentLen : 4096;
  if (cap > maxBytes) {
    cap = maxBytes;
  }
  uint8_t* buf = (uint8_t*)malloc(cap + 1);
  if (!buf) {
    Serial.printf("http malloc fail need=%u max=%u\n", (unsigned)(cap + 1),
                  ESP.getMaxAllocHeap());
    http.end();
    client.stop();
    return nullptr;
  }

  size_t total = 0;
  const uint32_t deadline = millis() + timeoutMs;
  uint8_t tmp[512];
  while (millis() <= deadline) {
    inputServiceDuringBlock();
    if (composeAbortRequested()) {
      free(buf);
      http.end();
      client.stop();
      return nullptr;
    }
    size_t avail = stream->available();
    if (!avail) {
      if (contentLen > 0 && total >= (size_t)contentLen) {
        break;
      }
      if (!http.connected()) {
        delay(10);
        if (!stream->available()) {
          break;
        }
        continue;
      }
      delay(2);
      continue;
    }
    if (avail > sizeof(tmp)) {
      avail = sizeof(tmp);
    }
    const int n = stream->readBytes(tmp, avail);
    if (n <= 0) {
      break;
    }
    if (total + (size_t)n > maxBytes) {
      free(buf);
      http.end();
      client.stop();
      return nullptr;
    }
    if (total + (size_t)n > cap) {
      size_t ncap = cap * 2;
      if (ncap < total + (size_t)n) {
        ncap = total + (size_t)n;
      }
      if (ncap > maxBytes) {
        ncap = maxBytes;
      }
      uint8_t* nbuf = (uint8_t*)realloc(buf, ncap + 1);
      if (!nbuf) {
        free(buf);
        http.end();
        client.stop();
        return nullptr;
      }
      buf = nbuf;
      cap = ncap;
    }
    memcpy(buf + total, tmp, (size_t)n);
    total += (size_t)n;
    if (contentLen > 0 && total >= (size_t)contentLen) {
      break;
    }
  }

  http.end();
  client.stop();
  if (contentLen > 0 && total < (size_t)contentLen) {
    free(buf);
    return nullptr;
  }
  if (total < 8) {
    free(buf);
    return nullptr;
  }
  buf[total] = 0;
  *outLen = total;
  return buf;
}

bool httpFetchToFile(const char* url, const char* referer, fs::File& out,
                     size_t maxBytes, size_t* outLen, uint32_t timeoutMs) {
  if (outLen) {
    *outLen = 0;
  }
  if (!url || !out) {
    return false;
  }

  WiFiClientSecure client;
  HTTPClient http;
  if (!httpSetup(http, client, url, referer, timeoutMs)) {
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP %d\n", code);
    http.end();
    client.stop();
    return false;
  }

  const int contentLen = http.getSize();
  if (contentLen > (int)maxBytes) {
    Serial.printf("HTTP too large %d\n", contentLen);
    http.end();
    client.stop();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  if (!stream) {
    http.end();
    client.stop();
    return false;
  }

  size_t total = 0;
  const uint32_t deadline = millis() + timeoutMs;
  uint32_t lastDataMs = millis();
  uint8_t tmp[512];
  while (millis() <= deadline) {
    inputServiceDuringBlock();
    if (composeAbortRequested()) {
      http.end();
      client.stop();
      return false;
    }
    size_t avail = stream->available();
    if (!avail) {
      if (contentLen > 0 && total >= (size_t)contentLen) {
        break;
      }
      // 无 Content-Length 时：断开且静默一段时间才结束，避免截断 PNG
      const bool quiet = (millis() - lastDataMs) > 300;
      if (!http.connected() && quiet) {
        break;
      }
      if (quiet && contentLen <= 0 && total > 8) {
        // 仍连接但长时间无数据：视为结束
        if (!http.connected() || (millis() - lastDataMs) > 800) {
          break;
        }
      }
      delay(5);
      continue;
    }
    if (avail > sizeof(tmp)) {
      avail = sizeof(tmp);
    }
    const int n = stream->readBytes(tmp, avail);
    if (n <= 0) {
      if ((millis() - lastDataMs) > 300) {
        break;
      }
      delay(5);
      continue;
    }
    lastDataMs = millis();
    if (total + (size_t)n > maxBytes) {
      Serial.printf("HTTP exceed max %u\n", (unsigned)maxBytes);
      http.end();
      client.stop();
      return false;
    }
    if (out.write(tmp, (size_t)n) != (size_t)n) {
      http.end();
      client.stop();
      return false;
    }
    total += (size_t)n;
    if (contentLen > 0 && total >= (size_t)contentLen) {
      break;
    }
  }

  http.end();
  client.stop();
  out.flush();

  if (contentLen > 0 && total < (size_t)contentLen) {
    Serial.printf("HTTP incomplete %u/%d\n", (unsigned)total, contentLen);
    return false;
  }
  if (total < 8) {
    return false;
  }
  if (outLen) {
    *outLen = total;
  }
  return true;
}
