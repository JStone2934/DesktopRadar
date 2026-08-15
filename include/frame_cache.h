#pragma once

#include <stddef.h>
#include <stdint.h>

#include <FS.h>

#include "LGFX_GC9A01.hpp"
#include "config.h"
#include "mercator.h"

/** 造片时累计屏幕中心 3×3 雷达源色（8-bit×alpha）。 */
struct RadarCenterSample {
  uint32_t sumR;
  uint32_t sumG;
  uint32_t sumB;
  uint32_t sumA;
  uint8_t maxAlpha;
};

// 雷达源图按 8x8 屏幕像素降采样；每格保留 alpha 最大的源色，供动态
// 覆盖层判断当前位置是否有云。该网格不包含底图、十字或底栏颜色。
static constexpr int RADAR_COLOR_GRID_CELL = 8;
static constexpr int RADAR_COLOR_GRID_COLS =
    LCD_WIDTH / RADAR_COLOR_GRID_CELL;
static constexpr int RADAR_COLOR_GRID_ROWS =
    LCD_HEIGHT / RADAR_COLOR_GRID_CELL;
static constexpr int RADAR_COLOR_GRID_COUNT =
    RADAR_COLOR_GRID_COLS * RADAR_COLOR_GRID_ROWS;

struct RadarColorGrid {
  uint16_t color565[RADAR_COLOR_GRID_COUNT];
  uint8_t alpha[RADAR_COLOR_GRID_COUNT];
};

bool frameCacheBegin();

/** 成品 RGB565 已就绪（ready + 长度校验）。 */
bool frameCacheHas(int zoom);

/** z3–z12 中已就绪的档位数。 */
int frameCacheCountReady();

/** 可缓存的总档位数（ZOOM_MAX - ZOOM_MIN + 1，再去掉 ZOOM_SKIP）。 */
int frameCacheZoomSlots();

/** 行刷 RGB565 到 LCD（秒切，需 ready）。 */
bool frameCacheBlit(LGFX* lcd, int zoom);

/** 从成品 RGB565 恢复一个小矩形区域；用于擦除局部 UI 覆盖层。 */
bool frameCacheRestoreRect(LGFX* lcd, int zoom, int x, int y, int w, int h);

/** 临时隐藏中心十字准星；只改 LCD，不改缓存文件。 */
bool frameCacheHideCrosshair(LGFX* lcd, int zoom);

/**
 * 行刷底图：只要 .rgb565 长度正确即可（含造片中已清 ready 的旧成品）。
 */
bool frameCacheBlitUnderlay(LGFX* lcd, int zoom);

/**
 * 将预警环按 alpha(0–255) 与指定档底图混合后画到 LCD。
 * alpha=0：只恢复环带底图像素（透明）；alpha=255：实色环（不读文件）。
 */
bool frameCachePaintAlertRing(LGFX* lcd, int zoom, uint16_t color565,
                              uint8_t alpha);

/**
 * 采样环带底图像素到缓冲区（供呼吸动画缓存，避免每帧读 Flash）。
 * 返回 false 表示无成品或缓冲区不足。
 */
bool frameCacheSampleAlertRing(int zoom, uint16_t* pix, uint8_t* xs, uint8_t* ys,
                               int cap, int* outCount);

bool frameCacheRemove(int zoom);

/** 删除全部缩放档、临时瓦片和 ready 边车；摆件模式独占 LittleFS。 */
void frameCacheClearAll();

/** 清除各档临时瓦片残留，保留已 commit 成品。 */
void frameCacheScrubOrphans();

/** 同上，但跳过 keepZoom（造片中的当前档，避免刚下好的 PNG 被清掉）。keepZoom<0 等于全清。 */
void frameCacheScrubOrphansExcept(int keepZoom);

/**
 * 造片前：清空该档临时瓦片与 ready，保留旧 .rgb565 供底图 blit。
 * 新成品写入 .rgb565.new，Commit 时原子替换。
 */
bool frameCachePrepare(int zoom);

/**
 * 造片失败/中止后：若仍有完整旧 .rgb565，恢复 .ready，并删除残留 .new。
 */
bool frameCacheRestoreStale(int zoom);

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

/** 将内存中的 240×240 RGB565 写入 .rgb565.new（不覆盖旧成品）。 */
bool frameCacheWriteRgb565(int zoom, const uint16_t* frame);

/** 清空并创建 .rgb565.new，供低内存分段造片。 */
bool frameCacheBeginRgb565New(int zoom);

/**
 * 将连续若干行写入 .rgb565.new；调用前须 frameCacheBeginRgb565New。
 * frame 只需容纳 rowCount×LCD_WIDTH 个 RGB565 像素。
 */
bool frameCacheWriteRgb565Band(int zoom, int startRow, int rowCount,
                               const uint16_t* frame);

/**
 * 从正在构建的 .rgb565.new 读取连续若干行。
 * 用于让 PNG 解码器与较大的合成帧带分时复用连续堆内存。
 */
bool frameCacheReadRgb565NewBand(int zoom, int startRow, int rowCount,
                                 uint16_t* frame);

/** 读已就绪静帧到内存缓冲（需 FRAME_RGB565_BYTES）。 */
bool frameCacheLoadRgb565(int zoom, uint16_t* frame);

/**
 * 批量读取成品底图像素。keys 为按行主序升序排列的 y*LCD_WIDTH+x，
 * 同一行只读取一次，供稀疏动态覆盖层保存精确底色。
 */
bool frameCacheReadPixelsSorted(int zoom, const uint16_t* keys,
                                uint16_t* colors, size_t count);

/**
 * 将 .rgb565.new 替换为正式 .rgb565，不写 ready。
 * 用于无雷达时仅上屏、供进度环底图。
 */
bool frameCachePromoteNewNoReady(int zoom);

/**
 * 从 256×256 RGB565 raw（及可选 alpha）贴入内存帧。
 * alphaPath 非空时按 0–255 alpha 与底图混合（消除雷达黑边）；否则 alphaKey 仅跳过 0。
 * centerOut 非空且带 alpha 时，累计落在屏幕中心 3×3 的雷达源色×alpha。
 */
bool frameCacheStampRawToBuffer(uint16_t* frame, const char* rawPath,
                                const char* alphaPath, int pasteX, int pasteY,
                                int scale, bool alphaKey,
                                RadarCenterSample* centerOut = nullptr,
                                RadarColorGrid* colorGridOut = nullptr);

/**
 * frameCacheStampRawToBuffer 的分段版本；frame 的第 0 行对应屏幕 bandY。
 */
bool frameCacheStampRawToBand(uint16_t* frame, int bandY, int bandHeight,
                              const char* rawPath, const char* alphaPath,
                              int pasteX, int pasteY, int scale, bool alphaKey,
                              RadarCenterSample* centerOut = nullptr,
                              RadarColorGrid* colorGridOut = nullptr);

/**
 * 将 256×256 RGB565 瓦片写入成品文件（慢，仅兼容保留）。
 */
bool frameCacheStampTile(int zoom, const uint16_t* tile256, int pasteX,
                         int pasteY, int scale, bool alphaKey);

/** 静帧对应的雷达 Unix 时间（commit 时写入，供动画积累去重）。 */
bool frameCacheWriteRadarTime(int zoom, uint32_t timeSec);
bool frameCacheReadRadarTime(int zoom, uint32_t* timeSec);
void frameCacheRemoveRadarTime(int zoom);

/** 中心天气采样：与 .rgb565 同级，commit 清临时目录后仍保留。 */
bool frameCacheWriteAlert(int zoom, bool hasCloud, uint16_t color565);
bool frameCacheReadAlert(int zoom, bool* hasCloud, uint16_t* color565);
void frameCacheRemoveAlert(int zoom);

/** 原子保存纯雷达颜色采样网格；radarTime 用于拒绝与成品不匹配的旧网格。 */
bool frameCacheWriteRadarColorGrid(int zoom, const RadarColorGrid* grid,
                                   uint32_t radarTime);

/** 读取屏幕位置所属 8x8 单元的纯雷达颜色；无云时 hasCloud=false。 */
bool frameCacheSampleRadarColor(int zoom, int x, int y, bool* hasCloud,
                                uint16_t* color565);
void frameCacheRemoveRadarColorGrid(int zoom);

/** 由 RadarCenterSample 得到 hasCloud + RGB565；无有效样本返回 false。 */
bool radarCenterSampleFinalize(const RadarCenterSample* s, bool* hasCloud,
                               uint16_t* color565);

void radarCenterSampleReset(RadarCenterSample* s);

/** 在内存帧上画十字准星（含中心红点）。 */
void frameCacheDrawCrosshairBuf(uint16_t* frame, uint16_t color);

/** 在内存帧分段上画十字准星；frame 第 0 行对应屏幕 bandY。 */
void frameCacheDrawCrosshairBand(uint16_t* frame, int bandY, int bandHeight,
                                 uint16_t color);

/** 在成品文件上画十字准星（含中心红点）。 */
bool frameCacheDrawCrosshair(int zoom, uint16_t color);

/**
 * 在内存帧底部烘焙信息条：北京时间 "WWW HH:MM"（如 WED 14:35）。
 * frameTs==0 时显示 --- --:--。
 */
void frameCacheDrawOverlayBuf(uint16_t* frame, uint32_t frameTs);

/** 在内存帧分段上烘焙信息条；frame 第 0 行对应屏幕 bandY。 */
void frameCacheDrawOverlayBand(uint16_t* frame, int bandY, int bandHeight,
                               uint32_t frameTs);

/** 校验 .rgb565.new 后替换正式文件、写 ready，并删除临时 PNG/meta。 */
bool frameCacheCommit(int zoom);

/** 打开临时瓦片供下载写入。 */
bool frameCacheOpenTileWrite(int zoom, bool isRadar, int tx, int ty,
                             fs::File* out);

// ---- 新鲜度（本轮刷新是否已重建）----
// ready 表示成品存在（可秒切，可能是过时数据）；fresh 表示本轮已重建（新雷达）。
// 刷新开始时把全部标为过时（fresh=false），逐档重建 commit 时置 fresh=true。
// 预取队列只入队 !fresh 的档；进度条按 fresh 计数。

/** 标记某档是否为本轮新鲜成品。 */
void frameCacheMarkFresh(int zoom, bool fresh);
bool frameCacheIsFresh(int zoom);

/** 全部标记为 fresh（true）或全部过时（false）。 */
void frameCacheMarkAllFresh(bool fresh);
/** 除 keepZoom 外全部标记过时（刷新入口用，保留旧帧可秒切）。 */
void frameCacheMarkAllStaleExcept(int keepZoom);

/** 当前 fresh 档数。 */
int frameCacheCountFresh();

/** 设置受保护档（屏上正在显示）；保留接口供缓存状态跟踪。 */
void frameCacheSetProtectedZoom(int zoom);

/**
 * 造片前检查临时工作区。不会删除任何已完成缓存；空间不足时返回 false，
 * 由调用方延后刷新，从而保证过时的远档仍可秒切。
 */
bool frameCacheEnsureBakeSpace(size_t needBytes, int keepZoom);
