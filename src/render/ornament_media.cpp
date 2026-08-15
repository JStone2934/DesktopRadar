#include "ornament_media.h"

#include <AnimatedGIF.h>
#include <Arduino.h>
#include <LittleFS.h>
#include <new>
#include <string.h>

#include "button.h"
#include "config.h"
#include "frame_cache.h"
#include "wind_field.h"

namespace {

static constexpr uint32_t kMetaMagic = 0x534D4544UL;  // "SMED"
static constexpr uint16_t kMetaVersion = 1;
static constexpr size_t kFsReserveBytes = 96U * 1024U;
static constexpr const char* kDir = "/media";
static constexpr const char* kPartPath = "/media/upload.part";
static constexpr const char* kRgbPath = "/media/active.rgb565";
static constexpr const char* kGifPath = "/media/active.gif";
static constexpr const char* kMetaPath = "/media/active.meta";
static constexpr const char* kMetaNewPath = "/media/active.meta.new";
static constexpr const char* kBackupPath = "/media/active.old";
static constexpr const char* kBackupMetaPath = "/media/active.meta.old";

struct __attribute__((packed)) MediaMeta {
  uint32_t magic;
  uint16_t version;
  uint8_t type;
  uint8_t reserved;
  uint16_t width;
  uint16_t height;
  uint32_t fileSize;
  uint32_t crc32;
  uint16_t frameCount;
  uint32_t durationMs;
};

static LGFX* s_lcd = nullptr;
static MediaMeta s_meta{};
static bool s_valid = false;
static bool s_paused = false;
static bool s_uploading = false;
static bool s_prepared = false;
static OrnamentMediaType s_preparedType = OrnamentMediaType::None;
static size_t s_expectedBytes = 0;
static size_t s_uploadedBytes = 0;
static uint32_t s_uploadCrc = 0xffffffffUL;
static File s_uploadFile;

static AnimatedGIF* s_gif = nullptr;
static File s_gifFile;
static uint32_t s_nextFrameAt = 0;
static int s_gifX = 0;
static int s_gifY = 0;
static uint8_t s_gifFailures = 0;
static bool s_gifLoopPending = false;
static uint16_t s_line[240];

struct GifValidationState {
  int frame;
  int canvasWidth;
  int canvasHeight;
  bool unsupportedDisposal;
  bool firstOpaqueFullWidth;
  int firstRows;
};
static GifValidationState s_validation{};

static void setError(char* out, size_t capacity, const char* message) {
  if (!out || capacity == 0) return;
  snprintf(out, capacity, "%s", message ? message : "未知错误");
}

static const char* activePath(OrnamentMediaType type) {
  return type == OrnamentMediaType::Gif ? kGifPath : kRgbPath;
}

static uint32_t crcUpdate(uint32_t crc, const uint8_t* data, size_t len) {
  while (len--) {
    crc ^= *data++;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320UL & (0UL - (crc & 1UL)));
    }
  }
  return crc;
}

static bool ensureDir() {
  return LittleFS.exists(kDir) || LittleFS.mkdir(kDir);
}

static bool crcFile(const char* path, uint32_t* out) {
  if (!out) return false;
  File file = LittleFS.open(path, "r");
  if (!file) return false;
  uint8_t buf[512];
  uint32_t crc = 0xffffffffUL;
  while (file.available()) {
    const int got = file.read(buf, sizeof(buf));
    if (got <= 0) {
      file.close();
      return false;
    }
    crc = crcUpdate(crc, buf, (size_t)got);
    buttonService();
    delay(0);
  }
  file.close();
  *out = crc ^ 0xffffffffUL;
  return true;
}

static bool readMetaAt(const char* metaPath, const char* mediaPath,
                       MediaMeta* out) {
  if (!out) return false;
  File file = LittleFS.open(metaPath, "r");
  if (!file || file.size() != sizeof(MediaMeta) ||
      file.read(reinterpret_cast<uint8_t*>(out), sizeof(MediaMeta)) !=
          sizeof(MediaMeta)) {
    if (file) file.close();
    return false;
  }
  file.close();
  if (out->magic != kMetaMagic || out->version != kMetaVersion ||
      (out->type != static_cast<uint8_t>(OrnamentMediaType::Rgb565) &&
       out->type != static_cast<uint8_t>(OrnamentMediaType::Gif))) {
    return false;
  }
  const auto type = static_cast<OrnamentMediaType>(out->type);
  if ((type == OrnamentMediaType::Rgb565 &&
       (out->fileSize != FRAME_RGB565_BYTES || out->width != LCD_WIDTH ||
        out->height != LCD_HEIGHT)) ||
      (type == OrnamentMediaType::Gif &&
       (out->fileSize == 0 || out->fileSize > ORNAMENT_GIF_MAX_BYTES ||
        out->width == 0 || out->height == 0 || out->width > LCD_WIDTH ||
        out->height > LCD_HEIGHT || out->frameCount == 0 ||
        out->frameCount > ORNAMENT_GIF_MAX_FRAMES))) {
    return false;
  }
  File media = LittleFS.open(mediaPath, "r");
  if (!media || media.size() != out->fileSize) {
    if (media) media.close();
    return false;
  }
  media.close();
  uint32_t crc = 0;
  return crcFile(mediaPath, &crc) && crc == out->crc32;
}

static bool readMeta(MediaMeta* out) {
  if (!out) return false;
  MediaMeta candidate{};
  File metaFile = LittleFS.open(kMetaPath, "r");
  if (!metaFile || metaFile.size() != sizeof(MediaMeta) ||
      metaFile.read(reinterpret_cast<uint8_t*>(&candidate),
                    sizeof(candidate)) != sizeof(candidate)) {
    if (metaFile) metaFile.close();
    return false;
  }
  metaFile.close();
  if (candidate.type != static_cast<uint8_t>(OrnamentMediaType::Rgb565) &&
      candidate.type != static_cast<uint8_t>(OrnamentMediaType::Gif)) {
    return false;
  }
  return readMetaAt(kMetaPath,
                    activePath(static_cast<OrnamentMediaType>(candidate.type)),
                    out);
}

static bool recoverInterruptedCommit(MediaMeta* out) {
  if (!out) return false;
  MediaMeta backup{};
  if (!readMetaAt(kBackupMetaPath, kBackupPath, &backup)) return false;
  const auto type = static_cast<OrnamentMediaType>(backup.type);
  LittleFS.remove(kRgbPath);
  LittleFS.remove(kGifPath);
  LittleFS.remove(kMetaPath);
  const char* restoredPath = activePath(type);
  if (!LittleFS.rename(kBackupPath, restoredPath)) {
    return false;
  }
  if (!LittleFS.rename(kBackupMetaPath, kMetaPath)) {
    LittleFS.rename(restoredPath, kBackupPath);
    return false;
  }
  *out = backup;
  return true;
}

static bool writeMeta(const MediaMeta& meta) {
  LittleFS.remove(kMetaNewPath);
  File file = LittleFS.open(kMetaNewPath, "w");
  if (!file ||
      file.write(reinterpret_cast<const uint8_t*>(&meta), sizeof(meta)) !=
          sizeof(meta)) {
    if (file) file.close();
    LittleFS.remove(kMetaNewPath);
    return false;
  }
  file.flush();
  file.close();
  LittleFS.remove(kMetaPath);
  if (!LittleFS.rename(kMetaNewPath, kMetaPath)) {
    LittleFS.remove(kMetaNewPath);
    return false;
  }
  return true;
}

static void showMessage(const char* top, const char* bottom) {
  if (!s_lcd) return;
  s_lcd->fillScreen(TFT_BLACK);
  s_lcd->setTextDatum(MC_DATUM);
  s_lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  s_lcd->setFont(&fonts::Font4);
  s_lcd->drawString(top ? top : "Storm Eye", 120, bottom ? 106 : 120);
  if (bottom) {
    s_lcd->setFont(&fonts::Font2);
    s_lcd->drawString(bottom, 120, 139);
  }
}

static void* gifOpen(const char* filename, int32_t* size) {
  s_gifFile = LittleFS.open(filename, "r");
  if (!s_gifFile) return nullptr;
  *size = (int32_t)s_gifFile.size();
  return &s_gifFile;
}

static void gifClose(void* handle) {
  File* file = static_cast<File*>(handle);
  if (file) file->close();
}

static int32_t gifRead(GIFFILE* source, uint8_t* dest, int32_t len) {
  File* file = static_cast<File*>(source->fHandle);
  if (!file || len <= 0) return 0;
  int32_t available = source->iSize - source->iPos;
  // AnimatedGIF 官方 FS 示例保留最后一个字节，否则 Arduino File 在读到
  // 绝对 EOF 后部分实现无法再次 seek，循环播放/验证会失败。
  if (available < len) len = available - 1;
  if (len <= 0) return 0;
  const int32_t got = (int32_t)file->read(dest, (size_t)len);
  source->iPos = (int32_t)file->position();
  return got;
}

static int32_t gifSeek(GIFFILE* source, int32_t position) {
  File* file = static_cast<File*>(source->fHandle);
  if (!file || position < 0 || !file->seek((uint32_t)position)) return -1;
  source->iPos = (int32_t)file->position();
  return source->iPos;
}

static void validationDraw(GIFDRAW* draw) {
  if (!draw) return;
  buttonService();
  if (draw->ucDisposalMethod == 3) {
    s_validation.unsupportedDisposal = true;
  }
  if (s_validation.frame == 0) {
    if (draw->iX != 0 || draw->iY != 0 ||
        draw->iWidth != s_validation.canvasWidth ||
        draw->ucHasTransparency) {
      s_validation.firstOpaqueFullWidth = false;
    }
    if (draw->y >= 0 && draw->y < s_validation.canvasHeight) {
      ++s_validation.firstRows;
    }
  }
}

static bool validateGif(const char* path, MediaMeta* meta, char* error,
                        size_t errorCapacity) {
  AnimatedGIF* decoder = new (std::nothrow) AnimatedGIF();
  if (!decoder) {
    setError(error, errorCapacity, "GIF 解码内存不足");
    return false;
  }
  decoder->begin(GIF_PALETTE_RGB565_LE);
  if (!decoder->open(path, gifOpen, gifClose, gifRead, gifSeek,
                     validationDraw)) {
    delete decoder;
    setError(error, errorCapacity, "无法打开 GIF");
    return false;
  }
  const int width = decoder->getCanvasWidth();
  const int height = decoder->getCanvasHeight();
  GIFINFO info{};
  if (width < 1 || height < 1 || width > LCD_WIDTH || height > LCD_HEIGHT ||
      !decoder->getInfo(&info) || info.iFrameCount < 1 ||
      info.iFrameCount > ORNAMENT_GIF_MAX_FRAMES) {
    decoder->close();
    delete decoder;
    setError(error, errorCapacity, "GIF 尺寸或帧数超出限制");
    return false;
  }

  decoder->reset();
  s_validation = {0, width, height, false, true, 0};
  int decoded = 0;
  while (decoded < info.iFrameCount) {
    int delayMs = 0;
    const int rc = decoder->playFrame(false, &delayMs, nullptr);
    if (rc < 0 || decoder->getLastError() != GIF_SUCCESS) break;
    ++decoded;
    s_validation.frame = decoded;
    delay(0);
    if (rc == 0) break;
  }
  const int lastError = decoder->getLastError();
  decoder->close();
  delete decoder;
  if (decoded != info.iFrameCount ||
      (lastError != GIF_SUCCESS && lastError != GIF_FILE_NOT_OPEN) ||
      s_validation.unsupportedDisposal ||
      !s_validation.firstOpaqueFullWidth ||
      s_validation.firstRows < height) {
    setError(error, errorCapacity,
             s_validation.unsupportedDisposal
                 ? "GIF 使用了不支持的恢复上一帧方式"
                 : "GIF 首帧必须完整且不透明");
    return false;
  }
  meta->width = (uint16_t)width;
  meta->height = (uint16_t)height;
  meta->frameCount = (uint16_t)info.iFrameCount;
  meta->durationMs = info.iDuration > 0 ? (uint32_t)info.iDuration : 0;
  return true;
}

static bool drawRgb() {
  if (!s_lcd) return false;
  File file = LittleFS.open(kRgbPath, "r");
  if (!file || file.size() != FRAME_RGB565_BYTES) {
    if (file) file.close();
    return false;
  }
  const bool oldSwap = s_lcd->getSwapBytes();
  s_lcd->setSwapBytes(true);
  for (int y = 0; y < LCD_HEIGHT; ++y) {
    if (file.read(reinterpret_cast<uint8_t*>(s_line), FRAME_ROW_BYTES) !=
        FRAME_ROW_BYTES) {
      file.close();
      s_lcd->setSwapBytes(oldSwap);
      return false;
    }
    s_lcd->pushImage(0, y, LCD_WIDTH, 1, s_line);
    buttonService();
  }
  file.close();
  s_lcd->setSwapBytes(oldSwap);
  return true;
}

static void playerDraw(GIFDRAW* draw) {
  if (!draw || !s_lcd) return;
  buttonService();
  const int y = s_gifY + draw->iY + draw->y;
  int width = draw->iWidth;
  if (y < 0 || y >= LCD_HEIGHT || draw->iX >= LCD_WIDTH || width < 1) return;
  if (draw->iX + width > LCD_WIDTH) width = LCD_WIDTH - draw->iX;
  uint8_t* pixels = draw->pPixels;
  uint16_t* palette = draw->pPalette;
  const bool restoreBackground = draw->ucDisposalMethod == 2;
  const bool oldSwap = s_lcd->getSwapBytes();
  s_lcd->setSwapBytes(true);

  int x = 0;
  while (x < width) {
    while (x < width && draw->ucHasTransparency && !restoreBackground &&
           pixels[x] == draw->ucTransparent) {
      ++x;
    }
    const int start = x;
    while (x < width) {
      const uint8_t index = pixels[x];
      if (draw->ucHasTransparency && !restoreBackground &&
          index == draw->ucTransparent) {
        break;
      }
      const uint8_t actual =
          restoreBackground && index == draw->ucTransparent
              ? draw->ucBackground
              : index;
      s_line[x - start] = palette[actual];
      ++x;
    }
    if (x > start) {
      s_lcd->pushImage(s_gifX + draw->iX + start, y, x - start, 1, s_line);
    }
  }
  s_lcd->setSwapBytes(oldSwap);
}

static bool openGifPlayer() {
  if (!s_lcd) return false;
  if (!s_gif) {
    s_gif = new (std::nothrow) AnimatedGIF();
    if (!s_gif) return false;
    s_gif->begin(GIF_PALETTE_RGB565_LE);
  }
  if (!s_gif->open(kGifPath, gifOpen, gifClose, gifRead, gifSeek,
                   playerDraw)) {
    return false;
  }
  s_gifX = (LCD_WIDTH - s_gif->getCanvasWidth()) / 2;
  s_gifY = (LCD_HEIGHT - s_gif->getCanvasHeight()) / 2;
  s_nextFrameAt = millis();
  s_gifLoopPending = false;
  return true;
}

static void stopGif() {
  if (s_gif) {
    s_gif->close();
    delete s_gif;
    s_gif = nullptr;
  }
  if (s_gifFile) s_gifFile.close();
  s_gifLoopPending = false;
}

static bool startCurrent() {
  if (!s_valid) return false;
  s_paused = false;
  s_gifFailures = 0;
  s_gifLoopPending = false;
  if (s_meta.type == static_cast<uint8_t>(OrnamentMediaType::Rgb565)) {
    return drawRgb();
  }
  if (s_lcd) s_lcd->fillScreen(TFT_BLACK);
  return openGifPlayer();
}

}  // namespace

bool ornamentMediaBegin(LGFX* lcd) {
  if (lcd) s_lcd = lcd;
  stopGif();
  s_valid = readMeta(&s_meta);
  if (!s_valid) s_valid = recoverInterruptedCommit(&s_meta);
  if (s_valid) {
    LittleFS.remove(kBackupPath);
    LittleFS.remove(kBackupMetaPath);
  }
  if (!s_valid) {
    memset(&s_meta, 0, sizeof(s_meta));
    showMessage("Storm Eye", "Upload at 192.168.4.1");
    return false;
  }
  if (!startCurrent()) {
    s_valid = false;
    showMessage("Media error", "Open 192.168.4.1");
    return false;
  }
  return true;
}

void ornamentMediaService() {
  if (!s_valid || s_uploading || s_paused || !s_gif) return;
  const uint32_t now = millis();
  if ((int32_t)(now - s_nextFrameAt) < 0) return;
  if (s_gifLoopPending) {
    s_gif->close();
    if (!openGifPlayer() && ++s_gifFailures >= 2) {
      ornamentMediaStop();
      s_valid = false;
      showMessage("GIF error", "Open 192.168.4.1");
    }
    return;
  }
  int delayMs = 0;
  const int rc = s_gif->playFrame(false, &delayMs, nullptr);
  if (rc >= 0 && s_gif->getLastError() == GIF_SUCCESS) {
    const uint32_t wait =
        delayMs < (int)ORNAMENT_GIF_MIN_FRAME_MS
            ? ORNAMENT_GIF_MIN_FRAME_MS
            : (uint32_t)delayMs;
    s_nextFrameAt = millis() + wait;
    s_gifFailures = 0;
    s_gifLoopPending = rc == 0;
    return;
  }
  s_gif->close();
  if (!openGifPlayer()) {
    if (++s_gifFailures >= 2) {
      ornamentMediaStop();
      s_valid = false;
      showMessage("GIF error", "Open 192.168.4.1");
    }
  }
}

void ornamentMediaStop() {
  stopGif();
  s_paused = false;
}

bool ornamentMediaTogglePause() {
  if (!s_valid || s_uploading || !s_gif ||
      s_meta.type != static_cast<uint8_t>(OrnamentMediaType::Gif)) {
    return false;
  }
  s_paused = !s_paused;
  if (!s_paused) s_nextFrameAt = millis();
  return s_paused;
}

void ornamentMediaGetStatus(OrnamentMediaStatus* out) {
  if (!out) return;
  out->type = static_cast<OrnamentMediaType>(s_meta.type);
  out->width = s_meta.width;
  out->height = s_meta.height;
  out->fileSize = s_meta.fileSize;
  out->frameCount = s_meta.frameCount;
  out->durationMs = s_meta.durationMs;
  out->valid = s_valid;
  out->paused = s_paused;
  out->uploading = s_uploading;
}

bool ornamentMediaPrepareUpload(OrnamentMediaType type, size_t expectedBytes,
                                char* error, size_t errorCapacity) {
  if ((type != OrnamentMediaType::Rgb565 && type != OrnamentMediaType::Gif) ||
      (type == OrnamentMediaType::Rgb565 &&
       expectedBytes != FRAME_RGB565_BYTES) ||
      (type == OrnamentMediaType::Gif &&
       (expectedBytes == 0 || expectedBytes > ORNAMENT_GIF_MAX_BYTES))) {
    setError(error, errorCapacity, "媒体类型或大小无效");
    return false;
  }
  // 配置引导允许在播放器尚未启动时直接换图。先只装载旧媒体元数据，
  // 不触碰 LCD，以便后续提交仍能用备份事务保护原文件。
  if (!s_valid) {
    s_valid = readMeta(&s_meta);
    if (!s_valid) s_valid = recoverInterruptedCommit(&s_meta);
  }
  ornamentMediaUploadAbort();
  ornamentMediaStop();
  frameCacheClearAll();
  windFieldClearCache();
  ensureDir();
  LittleFS.remove(kPartPath);

  size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (freeBytes < expectedBytes + kFsReserveBytes) {
    LittleFS.remove(kRgbPath);
    LittleFS.remove(kGifPath);
    LittleFS.remove(kMetaPath);
    s_valid = false;
    memset(&s_meta, 0, sizeof(s_meta));
    freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  }
  if (freeBytes < expectedBytes + kFsReserveBytes) {
    setError(error, errorCapacity, "LittleFS 空间不足");
    return false;
  }
  s_prepared = true;
  s_preparedType = type;
  s_expectedBytes = expectedBytes;
  return true;
}

bool ornamentMediaUploadBegin(char* error, size_t errorCapacity) {
  if (!s_prepared || !ensureDir()) {
    setError(error, errorCapacity, "请先准备上传空间");
    return false;
  }
  LittleFS.remove(kPartPath);
  s_uploadFile = LittleFS.open(kPartPath, "w");
  if (!s_uploadFile) {
    setError(error, errorCapacity, "无法创建上传文件");
    return false;
  }
  s_uploadedBytes = 0;
  s_uploadCrc = 0xffffffffUL;
  s_uploading = true;
  return true;
}

bool ornamentMediaUploadWrite(const uint8_t* data, size_t len, char* error,
                              size_t errorCapacity) {
  if (!s_uploading || !s_uploadFile || !data ||
      s_uploadedBytes + len > s_expectedBytes) {
    setError(error, errorCapacity, "上传数据超出声明大小");
    return false;
  }
  if (s_uploadFile.write(data, len) != len) {
    setError(error, errorCapacity, "写入 LittleFS 失败");
    return false;
  }
  s_uploadCrc = crcUpdate(s_uploadCrc, data, len);
  s_uploadedBytes += len;
  buttonService();
  return true;
}

bool ornamentMediaUploadFinish(char* error, size_t errorCapacity,
                               bool applyNow) {
  if (!s_uploading || !s_uploadFile) {
    setError(error, errorCapacity, "没有进行中的上传");
    return false;
  }
  s_uploadFile.flush();
  s_uploadFile.close();
  s_uploading = false;
  if (s_uploadedBytes != s_expectedBytes) {
    LittleFS.remove(kPartPath);
    setError(error, errorCapacity, "上传长度不完整");
    return false;
  }

  MediaMeta meta{};
  meta.magic = kMetaMagic;
  meta.version = kMetaVersion;
  meta.type = static_cast<uint8_t>(s_preparedType);
  meta.fileSize = (uint32_t)s_uploadedBytes;
  meta.crc32 = s_uploadCrc ^ 0xffffffffUL;
  if (s_preparedType == OrnamentMediaType::Rgb565) {
    meta.width = LCD_WIDTH;
    meta.height = LCD_HEIGHT;
    meta.frameCount = 1;
  } else if (!validateGif(kPartPath, &meta, error, errorCapacity)) {
    LittleFS.remove(kPartPath);
    s_prepared = false;
    if (s_valid) startCurrent();
    return false;
  }

  const char* target = activePath(s_preparedType);
  const bool hadOld = s_valid;
  const auto oldType = static_cast<OrnamentMediaType>(s_meta.type);
  LittleFS.remove(kBackupPath);
  LittleFS.remove(kBackupMetaPath);
  if (hadOld) {
    if (!LittleFS.rename(activePath(oldType), kBackupPath) ||
        !LittleFS.rename(kMetaPath, kBackupMetaPath)) {
      if (LittleFS.exists(kBackupPath)) {
        LittleFS.rename(kBackupPath, activePath(oldType));
      }
      setError(error, errorCapacity, "无法保护当前媒体");
      s_prepared = false;
      startCurrent();
      return false;
    }
  }
  LittleFS.remove(kRgbPath);
  LittleFS.remove(kGifPath);
  LittleFS.remove(kMetaPath);
  if (!LittleFS.rename(kPartPath, target) || !writeMeta(meta)) {
    LittleFS.remove(kPartPath);
    LittleFS.remove(target);
    LittleFS.remove(kMetaPath);
    if (hadOld) {
      MediaMeta restored{};
      if (recoverInterruptedCommit(&restored)) {
        s_meta = restored;
        s_valid = true;
        startCurrent();
      } else {
        s_valid = false;
        memset(&s_meta, 0, sizeof(s_meta));
      }
    } else {
      s_valid = false;
      memset(&s_meta, 0, sizeof(s_meta));
    }
    setError(error, errorCapacity, "媒体提交失败");
    s_prepared = false;
    return false;
  }
  s_meta = meta;
  s_valid = true;
  s_prepared = false;
  if (applyNow && !startCurrent()) {
    LittleFS.remove(target);
    LittleFS.remove(kMetaPath);
    MediaMeta restored{};
    if (hadOld && recoverInterruptedCommit(&restored)) {
      s_meta = restored;
      s_valid = true;
      startCurrent();
    } else {
      s_valid = false;
      memset(&s_meta, 0, sizeof(s_meta));
    }
    setError(error, errorCapacity, "媒体保存成功但无法播放");
    return false;
  }
  LittleFS.remove(kBackupPath);
  LittleFS.remove(kBackupMetaPath);
  return true;
}

void ornamentMediaUploadAbort() {
  if (s_uploadFile) s_uploadFile.close();
  if (s_uploading) LittleFS.remove(kPartPath);
  s_uploading = false;
  s_prepared = false;
  s_preparedType = OrnamentMediaType::None;
  s_expectedBytes = 0;
  s_uploadedBytes = 0;
}

void ornamentMediaClearAll() {
  ornamentMediaUploadAbort();
  ornamentMediaStop();
  LittleFS.remove(kPartPath);
  LittleFS.remove(kRgbPath);
  LittleFS.remove(kGifPath);
  LittleFS.remove(kMetaPath);
  LittleFS.remove(kMetaNewPath);
  LittleFS.remove(kBackupPath);
  LittleFS.remove(kBackupMetaPath);
  LittleFS.rmdir(kDir);
  memset(&s_meta, 0, sizeof(s_meta));
  s_valid = false;
}

bool ornamentMediaValid() { return s_valid; }
