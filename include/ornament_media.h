#pragma once

#include <stddef.h>
#include <stdint.h>

#include "LGFX_GC9A01.hpp"

enum class OrnamentMediaType : uint8_t {
  None = 0,
  Rgb565 = 1,
  Gif = 2,
};

struct OrnamentMediaStatus {
  OrnamentMediaType type;
  uint16_t width;
  uint16_t height;
  uint32_t fileSize;
  uint16_t frameCount;
  uint32_t durationMs;
  bool valid;
  bool paused;
  bool uploading;
};

/** 装载并显示当前媒体；LittleFS 和 LCD 必须已初始化。 */
bool ornamentMediaBegin(LGFX* lcd);

/** 非阻塞动画时间片。 */
void ornamentMediaService();

/** 停止动画、关闭文件并释放 GIF 解码对象，LCD 保持最后画面。 */
void ornamentMediaStop();

/** GIF 暂停/继续；静态图返回 false。 */
bool ornamentMediaTogglePause();

void ornamentMediaGetStatus(OrnamentMediaStatus* out);

/** 清除雷达/风场缓存并为上传准备空间。 */
bool ornamentMediaPrepareUpload(OrnamentMediaType type, size_t expectedBytes,
                                char* error, size_t errorCapacity);
bool ornamentMediaUploadBegin(char* error, size_t errorCapacity);
bool ornamentMediaUploadWrite(const uint8_t* data, size_t len,
                              char* error, size_t errorCapacity);
/** 完成并提交上传；applyNow=false 时只保存，等进入摆件模式后再显示。 */
bool ornamentMediaUploadFinish(char* error, size_t errorCapacity,
                               bool applyNow = true);
void ornamentMediaUploadAbort();

/** 删除媒体及事务残留。 */
void ornamentMediaClearAll();

/** 是否存在可信、可播放的媒体。 */
bool ornamentMediaValid();
