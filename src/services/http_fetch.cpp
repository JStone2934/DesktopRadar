#include "http_fetch.h"

#include <FS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

#include "zoom_ctrl.h"

namespace {

struct ResponsiveGetCtx {
  HTTPClient* http;
  TaskHandle_t waiter;
  volatile bool done;
  int code;
};

static void httpGetWorker(void* opaque) {
  auto* ctx = static_cast<ResponsiveGetCtx*>(opaque);
  ctx->code = ctx->http->GET();
  ctx->done = true;
  xTaskNotifyGive(ctx->waiter);
  vTaskDelete(nullptr);
}

/**
 * TLS 建连/响应头可能在 HTTPClient::GET 内阻塞数秒。放到同核低优先级
 * 任务后，等待方仍持续服务按键和LCD；ESP32-C3虽是单核，也能由调度器
 * 抢占网络等待，不让秒切依赖服务器响应时间。
 */
static int responsiveHttpGet(HTTPClient& http) {
  ResponsiveGetCtx ctx{&http, xTaskGetCurrentTaskHandle(), false, HTTPC_ERROR_CONNECTION_REFUSED};
  TaskHandle_t worker = nullptr;
  const BaseType_t created =
      xTaskCreate(httpGetWorker, "http-get", 6144, &ctx,
                  tskIDLE_PRIORITY + 1, &worker);
  if (created != pdPASS) {
    // 直接 GET 会把唯一 UI 主任务锁死到网络超时。更新可以失败，缓存切换
    // 不能失败；内存不足时快速放弃本次后台更新。
    Serial.println("HTTP GET worker alloc fail; defer request for UI priority");
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }
  while (!ctx.done) {
    inputServiceDuringBlock();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
  }
  return ctx.code;
}

static bool deadlineReached(uint32_t deadline) {
  return (int32_t)(millis() - deadline) >= 0;
}

class BoundedMemoryStream final : public Stream {
 public:
  explicit BoundedMemoryStream(size_t limit) : limit_(limit) {}
  ~BoundedMemoryStream() override { free(data_); }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    inputServiceDuringBlock();
    if (composeAbortRequested()) {
      aborted_ = true;
      return 0;
    }
    if (!data || size > limit_ - length_) {
      overflow_ = true;
      return 0;
    }
    const size_t needed = length_ + size;
    if (needed > capacity_) {
      size_t next = capacity_ ? capacity_ * 2U : 4096U;
      if (next < needed) {
        next = needed;
      }
      if (next > limit_) {
        next = limit_;
      }
      uint8_t* grown = static_cast<uint8_t*>(realloc(data_, next + 1U));
      if (!grown) {
        allocFailed_ = true;
        return 0;
      }
      data_ = grown;
      capacity_ = next;
    }
    memcpy(data_ + length_, data, size);
    length_ += size;
    return size;
  }

  bool failed() const { return overflow_ || allocFailed_ || aborted_; }
  bool overflowed() const { return overflow_; }
  bool aborted() const { return aborted_; }
  size_t length() const { return length_; }
  uint8_t* release() {
    if (!data_) {
      return nullptr;
    }
    data_[length_] = 0;
    uint8_t* result = data_;
    data_ = nullptr;
    capacity_ = 0;
    length_ = 0;
    return result;
  }

 private:
  uint8_t* data_ = nullptr;
  size_t length_ = 0;
  size_t capacity_ = 0;
  size_t limit_ = 0;
  bool overflow_ = false;
  bool allocFailed_ = false;
  bool aborted_ = false;
};

class BoundedFileStream final : public Stream {
 public:
  BoundedFileStream(fs::File& file, size_t limit)
      : file_(file), limit_(limit) {}

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { file_.flush(); }

  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    inputServiceDuringBlock();
    if (composeAbortRequested()) {
      aborted_ = true;
      return 0;
    }
    if (!data || size > limit_ - length_) {
      overflow_ = true;
      return 0;
    }
    const size_t written = file_.write(data, size);
    length_ += written;
    if (written != size) {
      writeFailed_ = true;
    }
    return written;
  }

  bool failed() const { return overflow_ || writeFailed_ || aborted_; }
  bool overflowed() const { return overflow_; }
  bool aborted() const { return aborted_; }
  size_t length() const { return length_; }

 private:
  fs::File& file_;
  size_t limit_ = 0;
  size_t length_ = 0;
  bool overflow_ = false;
  bool writeFailed_ = false;
  bool aborted_ = false;
};

static bool readExactBody(HTTPClient& http, WiFiClient& source, Stream& sink,
                          size_t count, size_t* total, uint32_t deadline) {
  uint8_t buffer[512];
  while (count > 0) {
    inputServiceDuringBlock();
    if (composeAbortRequested() || deadlineReached(deadline)) {
      return false;
    }
    size_t available = source.available();
    if (available == 0) {
      if (!http.connected()) {
        return false;
      }
      delay(2);
      continue;
    }
    if (available > count) {
      available = count;
    }
    if (available > sizeof(buffer)) {
      available = sizeof(buffer);
    }
    const int read = source.readBytes(buffer, available);
    if (read <= 0 || sink.write(buffer, (size_t)read) != (size_t)read) {
      return false;
    }
    count -= (size_t)read;
    *total += (size_t)read;
  }
  return true;
}

static bool readLine(HTTPClient& http, WiFiClient& source, char* line,
                     size_t capacity, uint32_t deadline) {
  if (!line || capacity < 2) {
    return false;
  }
  size_t length = 0;
  while (!deadlineReached(deadline)) {
    inputServiceDuringBlock();
    if (composeAbortRequested()) {
      return false;
    }
    if (!source.available()) {
      if (!http.connected()) {
        return false;
      }
      delay(2);
      continue;
    }
    const int value = source.read();
    if (value < 0) {
      continue;
    }
    if (value == '\n') {
      line[length] = '\0';
      return true;
    }
    if (value != '\r') {
      if (length + 1 >= capacity) {
        return false;
      }
      line[length++] = (char)value;
    }
  }
  return false;
}

static int readHttpBody(HTTPClient& http, Stream& sink, int contentLength,
                        uint32_t timeoutMs) {
  WiFiClient* source = http.getStreamPtr();
  if (!source) {
    return -1;
  }
  const uint32_t deadline = millis() + timeoutMs;
  size_t total = 0;
  const String transfer = http.header("Transfer-Encoding");
  if (transfer.equalsIgnoreCase("chunked")) {
    char line[64];
    while (readLine(http, *source, line, sizeof(line), deadline)) {
      char* end = nullptr;
      const unsigned long chunk = strtoul(line, &end, 16);
      if (end == line || chunk > SIZE_MAX - total) {
        return -1;
      }
      if (chunk == 0) {
        // Consume optional trailer headers through the final blank line.
        do {
          if (!readLine(http, *source, line, sizeof(line), deadline)) {
            return -1;
          }
        } while (line[0] != '\0');
        return (int)total;
      }
      if (!readExactBody(http, *source, sink, (size_t)chunk, &total,
                         deadline)) {
        return -1;
      }
      uint8_t crlf[2];
      size_t ignored = 0;
      class CrLfSink final : public Stream {
       public:
        explicit CrLfSink(uint8_t* out) : out_(out) {}
        int available() override { return 0; }
        int read() override { return -1; }
        int peek() override { return -1; }
        void flush() override {}
        size_t write(uint8_t value) override { return write(&value, 1); }
        size_t write(const uint8_t* data, size_t size) override {
          if (length_ + size > 2) return 0;
          memcpy(out_ + length_, data, size);
          length_ += size;
          return size;
        }
       private:
        uint8_t* out_;
        size_t length_ = 0;
      } crlfSink(crlf);
      if (!readExactBody(http, *source, crlfSink, 2, &ignored, deadline) ||
          crlf[0] != '\r' || crlf[1] != '\n') {
        return -1;
      }
    }
    return -1;
  }

  if (contentLength >= 0) {
    return readExactBody(http, *source, sink, (size_t)contentLength, &total,
                         deadline)
               ? (int)total
               : -1;
  }

  // Identity body without Content-Length: read until close; allow a short
  // quiet tail because some servers close after the final bytes asynchronously.
  uint32_t lastData = millis();
  uint8_t buffer[512];
  while (!deadlineReached(deadline)) {
    inputServiceDuringBlock();
    if (composeAbortRequested()) {
      return -1;
    }
    size_t available = source->available();
    if (available == 0) {
      if (!http.connected() && millis() - lastData > 50UL) {
        return (int)total;
      }
      delay(2);
      continue;
    }
    if (available > sizeof(buffer)) {
      available = sizeof(buffer);
    }
    const int read = source->readBytes(buffer, available);
    if (read <= 0 || sink.write(buffer, (size_t)read) != (size_t)read) {
      return -1;
    }
    total += (size_t)read;
    lastData = millis();
  }
  return -1;
}

}  // namespace

static bool httpSetup(HTTPClient& http, WiFiClientSecure& client, const char* url,
                      const char* referer, uint32_t timeoutMs) {
  client.setInsecure();
  // Arduino-ESP32 3.x 的 NetworkClientSecure 继承 Stream，单位是毫秒。
  // 旧写法除以 1000 后实际只有 10–15ms，较大的 PNG 常在约 65KB 处被
  // 一次正常的网络间隙截断。
  client.setTimeout(timeoutMs);
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(timeoutMs);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    Serial.printf("http begin failed: %s\n", url);
    return false;
  }
  static const char* responseHeaders[] = {"Transfer-Encoding"};
  http.collectHeaders(responseHeaders, 1);
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
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("HTTP skipped: WiFi offline (status=%d)\n",
                  (int)WiFi.status());
    return nullptr;
  }
  if (composeAbortRequested()) {
    return nullptr;
  }

  WiFiClientSecure client;
  HTTPClient http;
  if (!httpSetup(http, client, url, referer, timeoutMs)) {
    return nullptr;
  }
  if (composeAbortRequested()) {
    http.end();
    client.stop();
    return nullptr;
  }

  const int code = responsiveHttpGet(http);
  if (code != HTTP_CODE_OK) {
    char sslError[96]{};
    const int sslCode = client.lastError(sslError, sizeof(sslError));
    Serial.printf(
        "HTTP %d (%s) ssl=%d (%s) wifi=%d rssi=%d heap=%u max=%u\n",
        code, HTTPClient::errorToString(code).c_str(), sslCode,
        sslError[0] ? sslError : "none", (int)WiFi.status(), WiFi.RSSI(),
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
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

  BoundedMemoryStream sink(maxBytes);
  const int written = readHttpBody(http, sink, contentLen, timeoutMs);
  http.end();
  client.stop();
  if (written < 0 || sink.failed() || (size_t)written != sink.length()) {
    Serial.printf("HTTP body fail code=%d bytes=%u max=%u%s%s\n", written,
                  (unsigned)sink.length(), (unsigned)maxBytes,
                  sink.overflowed() ? " overflow" : "",
                  sink.aborted() ? " aborted" : "");
    return nullptr;
  }
  if (sink.length() < 8) {
    return nullptr;
  }
  *outLen = sink.length();
  return sink.release();
}

bool httpFetchToFile(const char* url, const char* referer, fs::File& out,
                     size_t maxBytes, size_t* outLen, uint32_t timeoutMs) {
  if (outLen) {
    *outLen = 0;
  }
  if (!url || !out) {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("HTTP file skipped: WiFi offline (status=%d)\n",
                  (int)WiFi.status());
    return false;
  }
  if (composeAbortRequested()) {
    return false;
  }

  WiFiClientSecure client;
  HTTPClient http;
  if (!httpSetup(http, client, url, referer, timeoutMs)) {
    return false;
  }
  if (composeAbortRequested()) {
    http.end();
    client.stop();
    return false;
  }

  const int code = responsiveHttpGet(http);
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

  BoundedFileStream sink(out, maxBytes);
  const int written = readHttpBody(http, sink, contentLen, timeoutMs);
  http.end();
  client.stop();
  out.flush();
  if (written < 0 || sink.failed() || (size_t)written != sink.length()) {
    Serial.printf("HTTP file body fail code=%d bytes=%u max=%u%s%s\n", written,
                  (unsigned)sink.length(), (unsigned)maxBytes,
                  sink.overflowed() ? " overflow" : "",
                  sink.aborted() ? " aborted" : "");
    return false;
  }
  if (sink.length() < 8) {
    return false;
  }
  if (outLen) {
    *outLen = sink.length();
  }
  return true;
}
