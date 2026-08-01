#pragma once

#include <stddef.h>
#include <stdint.h>

#include "LGFX_GC9A01.hpp"
#include "config.h"
#include "mercator.h"

/** 挂载 LittleFS；失败时 format 后再挂。 */
bool frameCacheBegin();

/** 该 zoom 是否有完整瓦片缓存（ready 标记）。 */
bool frameCacheHas(int zoom);

/** 从瓦片 PNG 缓存重绘到 lcd（无 HTTPS）。 */
bool frameCacheDraw(LGFX* lcd, int zoom);

/** 删除该档缓存。 */
bool frameCacheRemove(int zoom);

/** 造片前：清空并准备 /frames/zNN/ */
bool frameCachePrepare(int zoom);

/** 保存单张瓦片 PNG。 */
bool frameCacheSaveTile(int zoom, bool isRadar, int tx, int ty,
                        const uint8_t* data, size_t len);

/** 写入视口元数据。 */
bool frameCacheWriteMeta(int zoom, const Viewport& vp, bool haveRadar);

/** 写入 ready 标记，使 frameCacheHas 为真。 */
bool frameCacheCommit(int zoom);
