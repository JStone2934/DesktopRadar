#pragma once

#include <Arduino.h>
#include <FS.h>
#include <stddef.h>
#include <stdint.h>

/** HTTPS GET 到堆缓冲（调用方 free）。 */
uint8_t* httpFetch(const char* url, const char* referer, size_t maxBytes,
                   size_t* outLen, uint32_t timeoutMs);

/** HTTPS GET 流式写入 File（小块读取）。 */
bool httpFetchToFile(const char* url, const char* referer, fs::File& out,
                     size_t maxBytes, size_t* outLen, uint32_t timeoutMs);
