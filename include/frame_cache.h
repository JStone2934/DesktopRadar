#pragma once

#include <stddef.h>
#include <stdint.h>

#include <FS.h>

#include "LGFX_GC9A01.hpp"
#include "config.h"
#include "mercator.h"

bool frameCacheBegin();

/** 成品 RGB565 已就绪（ready + 长度校验）。 */
bool frameCacheHas(int zoom);

/** z3–z12 中已就绪的档位数。 */
int frameCacheCountReady();

/** 可缓存的总档位数（ZOOM_MAX - ZOOM_MIN + 1）。 */
int frameCacheZoomSlots();

/** 行刷 RGB565 到 LCD（秒切）。 */
bool frameCacheBlit(LGFX* lcd, int zoom);

bool frameCacheRemove(int zoom);

/** 清除各档临时瓦片残留，保留已 commit 成品。 */
void frameCacheScrubOrphans();

/** 同上，但跳过 keepZoom（造片中的当前档，避免刚下好的 PNG 被清掉）。keepZoom<0 等于全清。 */
void frameCacheScrubOrphansExcept(int keepZoom);

/** 造片前：清空该档临时瓦片与旧成品。 */
bool frameCachePrepare(int zoom);

bool frameCacheSaveTile(int zoom, bool isRadar, int tx, int ty,
                        const uint8_t* data, size_t len);

/** 流式下载目标路径。 */
void frameCacheTilePath(int zoom, bool isRadar, int tx, int ty, char* out,
                        size_t outLen);

bool frameCacheWriteMeta(int zoom, const Viewport& vp, bool haveRadar,
                         int overlayZoom, int scale, int rtx0, int rty0,
                         int rtx1, int rty1);

bool frameCacheReadMeta(int zoom, Viewport* vp, bool* haveRadar,
                        int* overlayZoom, int* scale, int* rtx0, int* rty0,
                        int* rtx1, int* rty1);

/** 创建 backdrop 填充的 rgb565 成品文件（尚未 ready）。 */
bool frameCacheCreateRgb565(int zoom, uint16_t backdropColor);

/** 将内存中的 240×240 RGB565 写入成品文件。 */
bool frameCacheWriteRgb565(int zoom, const uint16_t* frame);

/**
 * 从 256×256 RGB565 raw（及可选 alpha）贴入内存帧。
 * alphaPath 非空时按 0–255 alpha 与底图混合（消除雷达黑边）；否则 alphaKey 仅跳过 0。
 */
bool frameCacheStampRawToBuffer(uint16_t* frame, const char* rawPath,
                                const char* alphaPath, int pasteX, int pasteY,
                                int scale, bool alphaKey);

/**
 * 将 256×256 RGB565 瓦片写入成品文件（慢，仅兼容保留）。
 */
bool frameCacheStampTile(int zoom, const uint16_t* tile256, int pasteX,
                         int pasteY, int scale, bool alphaKey);

/** 在内存帧上画十字准星（含中心红点）。 */
void frameCacheDrawCrosshairBuf(uint16_t* frame, uint16_t color);

/** 在成品文件上画十字准星（含中心红点）。 */
bool frameCacheDrawCrosshair(int zoom, uint16_t color);

/**
 * 在内存帧底部烘焙信息条：北京时间 "WWW HH:MM"（如 WED 14:35）。
 * frameTs==0 时显示 --- --:--。
 */
void frameCacheDrawOverlayBuf(uint16_t* frame, uint32_t frameTs);

/** 校验长度后写 ready；并删除临时 PNG/meta。 */
bool frameCacheCommit(int zoom);

/** 打开临时瓦片供下载写入。 */
bool frameCacheOpenTileWrite(int zoom, bool isRadar, int tx, int ty,
                             fs::File* out);
