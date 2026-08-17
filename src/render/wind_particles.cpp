#include "wind_particles.h"

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "frame_cache.h"
#include "wind_field.h"
#include "zoom_ctrl.h"

namespace {

struct Particle {
  float x[WIND_TRAIL_POINTS];
  float y[WIND_TRAIL_POINTS];
  uint32_t ageMs;
  uint32_t lifeMs;
};

// 每个活动像素保存精确底色；透明度归零时只恢复该像素，不清整屏。
// 3072×6B≈18KB，连同新增像素队列仍远小于 115KB 全帧缓冲。
struct TrailPixel {
  uint16_t key;       // y * LCD_WIDTH + x
  uint16_t base565;   // 对应静态 RGB565 缓存中的原色
  uint8_t strength;   // 255=新轨迹，0=下一次绘制时恢复底色
  uint8_t flags;      // bit0=本帧粒子头，bit1=不进入拖影的瞬态箭翼
};

struct WidePathNode {
  float x;
  float y;
  float distance;
};

static_assert(sizeof(TrailPixel) == 6, "TrailPixel must stay compact");

static constexpr int kTrailPixelCapacity = 3072;
// 最长约 300px 的通道按 20px 间距最多需要 15 个粗箭头；每个方向掩码
// 最多 78 像素，1280 项工作区和 3072 项轨迹池仍留有足够余量。
static constexpr int kNewPixelCapacity = 1280;
static constexpr uint32_t kTrailFadeMs = 1500UL;
static constexpr int kWideQueueMinSlots = 6;
static constexpr int kWideQueueMaxSlots = 15;
static constexpr int kWideQueueMaxReduction = 3;
static constexpr float kWideQueueTargetSpacing = 20.0f;
static constexpr float kWideQueueFadeLength = 28.0f;
static constexpr float kWideQueueEndExclusion = 12.0f;
static constexpr float kWideQueueSpeedScale = 3.0f;
static constexpr float kWideQueueMinimumSpeed = 6.0f;
static constexpr uint32_t kWideSpeedTransitionMs = 2000UL;
static constexpr float kWideQueueMinRouteLength = 120.0f;
static constexpr float kWideQueueTrimStep = 10.0f;
static constexpr uint8_t kWideOverlapAlphaMin = 64;
static constexpr uint8_t kWideOverlapPixelLimit = 6;
static constexpr size_t kWideAuditBitmapBytes =
    (LCD_WIDTH * LCD_HEIGHT + 7U) / 8U;
static constexpr int kWideArrowMaskCapacity = 112;
static constexpr int kWideDirectionCount = 16;
static constexpr int kWidePathCapacity = 32;
static constexpr float kWidePathStep = 10.0f;
static constexpr int kWideSeedGrid = 5;
static constexpr int kZoomSlots = ZOOM_MAX - ZOOM_MIN + 1;
static constexpr uint16_t kWideFallbackColor = 0xE7FF;  // RGB(225,253,255)
static constexpr uint32_t kWideColorTransitionMs = 600UL;

static Particle s_particles[WIND_PARTICLE_COUNT];
static TrailPixel s_trails[kTrailPixelCapacity];
static uint16_t s_newKeys[kNewPixelCapacity];
static uint16_t s_newBase[kNewPixelCapacity];
static uint8_t s_newFlags[kNewPixelCapacity];
static uint8_t s_newStrength[kNewPixelCapacity];
static uint16_t s_spanBuf[LCD_WIDTH];
static int8_t s_headings[WIND_PARTICLE_COUNT];
static int s_trailCount = 0;
static int s_newCount = 0;
static uint32_t s_rng = 0x83a4f19dUL;
static uint32_t s_lastFrameAt = 0;
static uint32_t s_seenRevision = 0;
static int s_seenZoom = -1;
static uint32_t s_statsAt = 0;
static uint32_t s_statsFrames = 0;
static uint32_t s_lastCapacityLogAt = 0;
static uint32_t s_lastSampleFailLogAt = 0;
static uint32_t s_statsSpans = 0;
static WindParticleStyle s_style = WIND_PARTICLE_DOT;
static int s_activeParticleCount = WIND_PARTICLE_COUNT;
static int s_wideQueueSlotCount = 0;
static float s_wideQueuePhase = 0.0f;
static float s_wideQueueSpacing = 22.0f;
static float s_wideQueueSpeed = kWideQueueMinimumSpeed;
static float s_wideQueueSpeedFrom = kWideQueueMinimumSpeed;
static float s_wideQueueSpeedTarget = kWideQueueMinimumSpeed;
static uint32_t s_wideQueueSpeedTransitionAt = 0;
static float s_wideCenterSpeed = 0.0f;
static bool s_wideCenterSpeedValid = false;
static float s_wideWindowStart = 0.0f;
static float s_wideWindowLength = 0.0f;
static float s_wideWindowMinGap = 0.0f;
static bool s_wideWindowFallback = false;
static float s_wideDominantX = 1.0f;
static float s_wideDominantY = 0.0f;
static uint32_t s_wideDirectionRevision = 0;
static bool s_wideDirectionValid = false;
static WidePathNode s_widePath[kWidePathCapacity];
static int s_widePathCount = 0;
static float s_widePathLength = 0.0f;
static float s_wideSeedX[kZoomSlots];
static float s_wideSeedY[kZoomSlots];
static bool s_wideSeedValid[kZoomSlots];
static uint16_t s_wideColorCurrent = kWideFallbackColor;
static uint16_t s_wideColorFrom = kWideFallbackColor;
static uint16_t s_wideColorTarget = kWideFallbackColor;
static uint32_t s_wideColorTransitionAt = 0;
static bool s_wideColorReady = false;
static bool s_wideColorSamplePending = true;
static bool s_wideColorHasCloud = false;
static uint32_t s_wideAuditRevision = 0;
static bool s_wideAuditHidden = false;
static uint16_t s_wideAuditMaxShared = 0;
static float s_wideAuditTrim = 0.0f;
static uint32_t s_wideAuditMs = 0;
static int s_wideAuditInitialSlots = 0;
static bool s_wideAuditHeld = false;

struct WideLayoutState {
  float routeLength;
  uint32_t revision;
  uint8_t slots;
  uint8_t safeStreak;
  bool valid;
};

static WideLayoutState s_wideApplied[kZoomSlots];
static WideLayoutState s_widePending[kZoomSlots];

static constexpr uint8_t kTrailHead = 0x01;
// 只用于开放式箭翼：本帧以头部颜色绘制，下一帧恢复静态底色，
// 不把 V 形轮廓加入 1500ms 的持久拖影。
static constexpr uint8_t kTrailTransientHead = 0x02;

struct ArrowOffset {
  int8_t x;
  int8_t y;
};

// 屏幕 y 轴向下。每行依次为 E、SE、S、SW、W、NW、N、NE；
// 一个尖端加两条各 3px 的短翼，形成 7x7 范围内的开放式箭头。
static constexpr ArrowOffset kOpenArrowOffsets[8][7] = {
    {{0, 0}, {-1, -1}, {-2, -2}, {-3, -3}, {-1, 1}, {-2, 2}, {-3, 3}},
    {{0, 0}, {-1, 0}, {-2, 0}, {-3, 0}, {0, -1}, {0, -2}, {0, -3}},
    {{0, 0}, {-1, -1}, {-2, -2}, {-3, -3}, {1, -1}, {2, -2}, {3, -3}},
    {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {0, -1}, {0, -2}, {0, -3}},
    {{0, 0}, {1, -1}, {2, -2}, {3, -3}, {1, 1}, {2, 2}, {3, 3}},
    {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {0, 1}, {0, 2}, {0, 3}},
    {{0, 0}, {-1, 1}, {-2, 2}, {-3, 3}, {1, 1}, {2, 2}, {3, 3}},
    {{0, 0}, {-1, 0}, {-2, 0}, {-3, 0}, {0, 1}, {0, 2}, {0, 3}},
};
static ArrowOffset
    s_wideArrowMasks[kWideDirectionCount][kWideArrowMaskCapacity];
static uint8_t s_wideArrowMaskCounts[kWideDirectionCount];
static float s_wideDirectionVectors[kWideDirectionCount][2];
static bool s_wideArrowMasksReady = false;

static uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t alpha);
static uint16_t currentWideQueueColor(uint32_t now);
static int quantizeWideDirection(float forwardX, float forwardY);
static void buildWideArrowMasks();

static int styleParticleCount() {
  return s_style == WIND_PARTICLE_WIDE_ARROW ? 0 : WIND_PARTICLE_COUNT;
}

static uint32_t nextRand() {
  uint32_t x = s_rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s_rng = x ? x : 0x83a4f19dUL;
  return s_rng;
}

static float rand01() {
  return (float)(nextRand() & 0x00FFFFFFUL) / 16777216.0f;
}

static bool pointAllowed(float x, float y) {
  if (x < 4.0f || x > (float)(LCD_WIDTH - 5) || y < 4.0f ||
      y >= (float)(LCD_HEIGHT - OVERLAY_BAR_H - 2)) {
    return false;
  }
  const float dx = x - (float)LCD_WIDTH * 0.5f;
  const float dy = y - (float)LCD_HEIGHT * 0.5f;
  const float d2 = dx * dx + dy * dy;
  if (d2 > 112.0f * 112.0f || d2 < 12.0f * 12.0f) {
    return false;
  }
  return true;
}

static bool wideArrowPixelAllowed(float x, float y) {
  if (x < 4.0f || x > (float)(LCD_WIDTH - 5) || y < 4.0f ||
      y >= (float)(LCD_HEIGHT - OVERLAY_BAR_H - 2)) {
    return false;
  }
  const float dx = x - (float)LCD_WIDTH * 0.5f;
  const float dy = y - (float)LCD_HEIGHT * 0.5f;
  // 粗箭头允许穿过中心十字，只保留圆屏外沿和底栏裁切。
  return dx * dx + dy * dy <= 112.0f * 112.0f;
}

static void spawnParticle(Particle* p) {
  if (!p) {
    return;
  }
  float x = LCD_WIDTH * 0.5f;
  float y = LCD_HEIGHT * 0.5f;
  for (int attempt = 0; attempt < 80; ++attempt) {
    x = 5.0f + rand01() * (float)(LCD_WIDTH - 10);
    y = 5.0f + rand01() * (float)(LCD_HEIGHT - OVERLAY_BAR_H - 10);
    if (pointAllowed(x, y)) {
      break;
    }
  }
  for (int i = 0; i < WIND_TRAIL_POINTS; ++i) {
    p->x[i] = x;
    p->y[i] = y;
  }
  p->lifeMs = 3500UL + (nextRand() % 2501UL);
  p->ageMs = nextRand() % p->lifeMs;
}

static void resetAll(int zoom, uint32_t revision) {
  s_rng = 0x83a4f19dUL ^ ((uint32_t)(zoom + 31) * 2654435761UL) ^
          windFieldModelTime();
  for (int i = 0; i < WIND_PARTICLE_COUNT; ++i) {
    spawnParticle(&s_particles[i]);
    s_headings[i] = -1;
  }
  // 缩放切换前已经完整 blit 新底图，旧轨迹只需丢弃，不能再恢复旧底色。
  s_trailCount = 0;
  s_newCount = 0;
  s_seenZoom = zoom;
  s_seenRevision = revision;
  s_lastFrameAt = 0;
  s_statsAt = 0;
  s_statsFrames = 0;
  s_statsSpans = 0;
  s_activeParticleCount = styleParticleCount();
  s_wideQueueSlotCount = 0;
  s_wideQueuePhase = 0.0f;
  s_wideQueueSpacing = 22.0f;
  s_wideQueueSpeed = kWideQueueMinimumSpeed;
  s_wideQueueSpeedFrom = kWideQueueMinimumSpeed;
  s_wideQueueSpeedTarget = kWideQueueMinimumSpeed;
  s_wideQueueSpeedTransitionAt = 0;
  s_wideCenterSpeed = 0.0f;
  s_wideCenterSpeedValid = false;
  s_wideWindowStart = 0.0f;
  s_wideWindowLength = 0.0f;
  s_wideWindowMinGap = 0.0f;
  s_wideWindowFallback = false;
  s_wideDominantX = 1.0f;
  s_wideDominantY = 0.0f;
  s_wideDirectionRevision = 0;
  s_wideDirectionValid = false;
  s_widePathCount = 0;
  s_widePathLength = 0.0f;
  s_wideColorCurrent = kWideFallbackColor;
  s_wideColorFrom = kWideFallbackColor;
  s_wideColorTarget = kWideFallbackColor;
  s_wideColorTransitionAt = 0;
  s_wideColorReady = false;
  s_wideColorSamplePending = true;
  s_wideColorHasCloud = false;
  s_wideAuditRevision = revision;
  s_wideAuditHidden = false;
  s_wideAuditMaxShared = 0;
  s_wideAuditTrim = 0.0f;
  s_wideAuditMs = 0;
  s_wideAuditInitialSlots = 0;
  s_wideAuditHeld = false;
  Serial.printf("wind particles reset z%d count=%d fieldRev=%lu style=%u\n",
                zoom, s_activeParticleCount, (unsigned long)revision,
                (unsigned)s_style);
}

static int8_t quantizeHeading(float east, float south) {
  const float ax = fabsf(east);
  const float ay = fabsf(south);
  // tan(67.5deg)：把连续方向稳定量化到 8 个适合 5x5 像素箭头的方向。
  constexpr float kAxisRatio = 2.41421356f;
  if (ax > ay * kAxisRatio) {
    return east >= 0.0f ? 0 : 4;
  }
  if (ay > ax * kAxisRatio) {
    return south >= 0.0f ? 2 : 6;
  }
  if (east >= 0.0f) {
    return south >= 0.0f ? 1 : 7;
  }
  return south >= 0.0f ? 3 : 5;
}

static float smoothTransition(float from, float target, uint32_t startedAt,
                              uint32_t durationMs, uint32_t now) {
  if (startedAt == 0 || durationMs == 0) {
    return target;
  }
  const uint32_t elapsed = now - startedAt;
  if (elapsed >= durationMs) {
    return target;
  }
  const float t = (float)elapsed / (float)durationMs;
  const float smooth = t * t * (3.0f - 2.0f * t);
  return from + (target - from) * smooth;
}

static float currentWideQueueSpeed(uint32_t now) {
  s_wideQueueSpeed =
      smoothTransition(s_wideQueueSpeedFrom, s_wideQueueSpeedTarget,
                       s_wideQueueSpeedTransitionAt,
                       kWideSpeedTransitionMs, now);
  return s_wideQueueSpeed;
}

static void updateWideQueueCenterSpeed(uint32_t revision) {
  float east = 0.0f;
  float south = 0.0f;
  if (!windFieldSample((float)LCD_WIDTH * 0.5f,
                       (float)LCD_HEIGHT * 0.5f, &east, &south)) {
    Serial.printf("wind wide center unavailable retain=%d rev=%lu\n",
                  (int)s_wideCenterSpeedValid, (unsigned long)revision);
    return;
  }
  const float centerSpeed = sqrtf(east * east + south * south);
  if (!isfinite(centerSpeed)) {
    Serial.printf("wind wide center invalid rev=%lu\n",
                  (unsigned long)revision);
    return;
  }

  const uint32_t now = millis();
  const bool hadValidSpeed = s_wideCenterSpeedValid;
  const float currentSpeed = currentWideQueueSpeed(now);
  s_wideCenterSpeed = centerSpeed;
  s_wideCenterSpeedValid = true;
  const float speedTarget =
      max(kWideQueueMinimumSpeed, centerSpeed * kWideQueueSpeedScale);
  if (!hadValidSpeed || fabsf(speedTarget - s_wideQueueSpeedTarget) > 0.001f) {
    s_wideQueueSpeedFrom = currentSpeed;
    s_wideQueueSpeedTarget = speedTarget;
    s_wideQueueSpeedTransitionAt = now;
  }
  Serial.printf(
      "wind wide center=%.2fm/s vector=(%.2f,%.2f) target=%.1fpx/s rev=%lu\n",
      centerSpeed, east, south, speedTarget, (unsigned long)revision);
}

static bool updateWideQueueDirection(uint32_t revision) {
  if (s_wideDirectionRevision == revision) {
    return s_wideDirectionValid;
  }
  s_wideDirectionRevision = revision;
  s_wideDirectionValid = false;
  s_widePathCount = 0;
  s_widePathLength = 0.0f;
  s_wideAuditRevision = revision;
  s_wideAuditHidden = false;
  updateWideQueueCenterSpeed(revision);

  // 画面内 5×5 采样，以风速作为权重累计到八方向。选择权重最大的方向，
  // 比只看中心点更能代表整幅雷达图的主导风向，也不会被局部乱流带偏。
  float score[8] = {};
  float speedSum[8] = {};
  float eastSum[8] = {};
  float southSum[8] = {};
  int sampleCount[8] = {};
  for (int gy = 0; gy < 5; ++gy) {
    const float y = 32.0f + (float)gy * 38.0f;
    for (int gx = 0; gx < 5; ++gx) {
      const float x = 32.0f + (float)gx * 44.0f;
      float east = 0.0f;
      float south = 0.0f;
      if (!windFieldSample(x, y, &east, &south)) {
        continue;
      }
      const float speed = sqrtf(east * east + south * south);
      if (speed < 0.08f) {
        continue;
      }
      const int8_t heading = quantizeHeading(east, south);
      score[heading] += speed;
      speedSum[heading] += speed;
      eastSum[heading] += east;
      southSum[heading] += south;
      ++sampleCount[heading];
    }
  }

  int best = -1;
  for (int heading = 0; heading < 8; ++heading) {
    if (best < 0 || score[heading] > score[best]) {
      best = heading;
    }
  }
  if (best < 0 || sampleCount[best] == 0 || score[best] <= 0.0f) {
    Serial.printf("wind wide queue no dominant direction rev=%lu\n",
                  (unsigned long)revision);
    return false;
  }

  const float meanSpeed = speedSum[best] / (float)sampleCount[best];
  const float dominantMagnitude =
      sqrtf(eastSum[best] * eastSum[best] +
            southSum[best] * southSum[best]);
  if (dominantMagnitude < 0.001f) {
    return false;
  }
  s_wideDominantX = eastSum[best] / dominantMagnitude;
  s_wideDominantY = southSum[best] / dominantMagnitude;
  s_wideDirectionValid = true;
  Serial.printf(
      "wind wide dominant bin=%d vector=(%.2f,%.2f) score=%.1f mean=%.1fm/s rev=%lu\n",
      best, s_wideDominantX, s_wideDominantY, score[best], meanSpeed,
      (unsigned long)revision);
  return true;
}

static bool normalizeVector(float* x, float* y) {
  const float magnitude = sqrtf(*x * *x + *y * *y);
  if (magnitude < 0.001f) {
    return false;
  }
  *x /= magnitude;
  *y /= magnitude;
  return true;
}

static bool widePathPointAllowed(float x, float y) {
  if (x < 8.0f || x > (float)(LCD_WIDTH - 9) || y < 8.0f ||
      y >= (float)(LCD_HEIGHT - OVERLAY_BAR_H - 8)) {
    return false;
  }
  const float dx = x - (float)LCD_WIDTH * 0.5f;
  const float dy = y - (float)LCD_HEIGHT * 0.5f;
  return dx * dx + dy * dy <= 106.0f * 106.0f;
}

static void guidedWideFlow(float x, float y, float previousX,
                           float previousY, float* outX, float* outY) {
  float localX = s_wideDominantX;
  float localY = s_wideDominantY;
  float east = 0.0f;
  float south = 0.0f;
  if (windFieldSample(x, y, &east, &south) &&
      normalizeVector(&east, &south)) {
    localX = east;
    localY = south;
  }

  // 局部方向负责弯曲，主导方向负责防止回头成环；再与上一段平滑，
  // 让 7×7 网格只表现宽缓的大方向转弯。
  float desiredX = localX * 0.72f + s_wideDominantX * 0.28f;
  float desiredY = localY * 0.72f + s_wideDominantY * 0.28f;
  if (!normalizeVector(&desiredX, &desiredY)) {
    desiredX = s_wideDominantX;
    desiredY = s_wideDominantY;
  }
  float smoothX = previousX * 0.48f + desiredX * 0.52f;
  float smoothY = previousY * 0.48f + desiredY * 0.52f;
  if (!normalizeVector(&smoothX, &smoothY)) {
    smoothX = s_wideDominantX;
    smoothY = s_wideDominantY;
  }
  *outX = smoothX;
  *outY = smoothY;
}

static bool traceWidePath(float anchorX, float anchorY, WidePathNode* out,
                          int* outCount, float* outLength,
                          float* outScore) {
  if (!out || !outCount || !outLength || !outScore ||
      !widePathPointAllowed(anchorX, anchorY)) {
    return false;
  }
  constexpr int kSideCapacity = (kWidePathCapacity - 1) / 2;
  float upstreamX[kSideCapacity];
  float upstreamY[kSideCapacity];
  float downstreamX[kSideCapacity];
  float downstreamY[kSideCapacity];
  int upstreamCount = 0;
  int downstreamCount = 0;

  auto extend = [&](float sign, float* xs, float* ys, int* count) {
    float x = anchorX;
    float y = anchorY;
    float flowX = s_wideDominantX;
    float flowY = s_wideDominantY;
    for (int i = 0; i < kSideCapacity; ++i) {
      float firstX = 0.0f;
      float firstY = 0.0f;
      guidedWideFlow(x, y, flowX, flowY, &firstX, &firstY);
      const float midX = x + sign * firstX * (kWidePathStep * 0.5f);
      const float midY = y + sign * firstY * (kWidePathStep * 0.5f);
      float nextFlowX = 0.0f;
      float nextFlowY = 0.0f;
      guidedWideFlow(midX, midY, firstX, firstY, &nextFlowX, &nextFlowY);
      const float nextX = x + sign * nextFlowX * kWidePathStep;
      const float nextY = y + sign * nextFlowY * kWidePathStep;
      if (!widePathPointAllowed(nextX, nextY)) {
        break;
      }
      xs[*count] = nextX;
      ys[*count] = nextY;
      ++(*count);
      x = nextX;
      y = nextY;
      flowX = nextFlowX;
      flowY = nextFlowY;
    }
  };

  extend(-1.0f, upstreamX, upstreamY, &upstreamCount);
  extend(1.0f, downstreamX, downstreamY, &downstreamCount);

  int count = 0;
  float length = 0.0f;
  auto append = [&](float x, float y) {
    if (count >= kWidePathCapacity) {
      return;
    }
    if (count > 0) {
      const float dx = x - out[count - 1].x;
      const float dy = y - out[count - 1].y;
      length += sqrtf(dx * dx + dy * dy);
    }
    out[count++] = WidePathNode{x, y, length};
  };
  for (int i = upstreamCount - 1; i >= 0; --i) {
    append(upstreamX[i], upstreamY[i]);
  }
  append(anchorX, anchorY);
  for (int i = 0; i < downstreamCount; ++i) {
    append(downstreamX[i], downstreamY[i]);
  }

  if (count < 2 || length < 120.0f) {
    return false;
  }

  float speedSum = 0.0f;
  float coherenceSum = 0.0f;
  int fieldSamples = 0;
  for (int i = 0; i < count; ++i) {
    float east = 0.0f;
    float south = 0.0f;
    if (!windFieldSample(out[i].x, out[i].y, &east, &south)) {
      continue;
    }
    const float speed = sqrtf(east * east + south * south);
    if (speed < 0.08f) {
      continue;
    }
    speedSum += speed;
    east /= speed;
    south /= speed;
    const float alignment =
        east * s_wideDominantX + south * s_wideDominantY;
    coherenceSum += max(0.0f, alignment);
    ++fieldSamples;
  }
  const float meanSpeed =
      fieldSamples > 0 ? speedSum / (float)fieldSamples : 0.0f;
  const float coherence =
      fieldSamples > 0 ? coherenceSum / (float)fieldSamples : 0.0f;
  // 长而连贯的流线优先；平均风速用于在长度接近时选择主风带。
  *outCount = count;
  *outLength = length;
  *outScore = length * (0.65f + 0.35f * coherence) +
              min(meanSpeed, 20.0f) * 4.0f;
  return true;
}

static bool sampleWidePath(float distance, float* x, float* y,
                           float* tangentX, float* tangentY) {
  if (s_widePathCount < 2 || !x || !y || !tangentX || !tangentY) {
    return false;
  }
  if (distance < 0.0f) {
    distance = 0.0f;
  } else if (distance > s_widePathLength) {
    distance = s_widePathLength;
  }
  int right = 1;
  while (right < s_widePathCount &&
         s_widePath[right].distance < distance) {
    ++right;
  }
  if (right >= s_widePathCount) {
    right = s_widePathCount - 1;
  }
  const int left = right - 1;
  const float segmentLength =
      s_widePath[right].distance - s_widePath[left].distance;
  const float t = segmentLength > 0.001f
                      ? (distance - s_widePath[left].distance) / segmentLength
                      : 0.0f;
  *x = s_widePath[left].x + (s_widePath[right].x - s_widePath[left].x) * t;
  *y = s_widePath[left].y + (s_widePath[right].y - s_widePath[left].y) * t;

  const int tangentLeft = max(0, left - 1);
  const int tangentRight = min(s_widePathCount - 1, right + 1);
  *tangentX = s_widePath[tangentRight].x - s_widePath[tangentLeft].x;
  *tangentY = s_widePath[tangentRight].y - s_widePath[tangentLeft].y;
  return normalizeVector(tangentX, tangentY);
}

static uint8_t wideLayoutAlpha(float distance, float routeLength,
                               float spacing) {
  const float distanceFromEnd = min(distance, routeLength - distance);
  if (distanceFromEnd <= 0.0f) {
    return 0;
  }
  const float fadeLength = min(kWideQueueFadeLength, spacing);
  if (distanceFromEnd >= fadeLength) {
    return 255;
  }
  const float t = distanceFromEnd / fadeLength;
  return (uint8_t)lroundf(t * t * (3.0f - 2.0f * t) * 255.0f);
}

enum WideAuditResult : uint8_t {
  WIDE_AUDIT_SAFE = 0,
  WIDE_AUDIT_OVERLAP,
  WIDE_AUDIT_ABORTED,
};

// 用实际旋转点阵模拟一个完整相位。位图只记录本相位已经占用的屏幕
// 像素；同一个箭头与前面所有箭头累计共享 6px 才视为实质重叠。
static WideAuditResult auditWideLayout(float routeLength, int slots,
                                       uint8_t* bitmap,
                                       uint16_t* maxShared) {
  if (!bitmap || !maxShared || slots < kWideQueueMinSlots ||
      routeLength < kWideQueueMinRouteLength) {
    return WIDE_AUDIT_OVERLAP;
  }
  const float spacing = routeLength / (float)slots;
  const int phaseSteps = max(1, (int)ceilf(spacing));
  *maxShared = 0;
  for (int phaseStep = 0; phaseStep < phaseSteps; ++phaseStep) {
    inputServiceDuringBlock();
    if (composeAbortRequested()) {
      return WIDE_AUDIT_ABORTED;
    }
    memset(bitmap, 0, kWideAuditBitmapBytes);
    const float phase = spacing * (float)phaseStep / (float)phaseSteps;
    for (int slot = 0; slot < slots; ++slot) {
      const float distance = phase + (float)slot * spacing;
      if (distance > routeLength ||
          wideLayoutAlpha(distance, routeLength, spacing) <
              kWideOverlapAlphaMin) {
        continue;
      }
      float x = 0.0f;
      float y = 0.0f;
      float tangentX = 0.0f;
      float tangentY = 0.0f;
      if (!sampleWidePath(distance, &x, &y, &tangentX, &tangentY)) {
        continue;
      }
      const int heading = quantizeWideDirection(tangentX, tangentY);
      const int centerX = (int)lroundf(x);
      const int centerY = (int)lroundf(y);
      uint16_t shared = 0;
      for (int i = 0; i < s_wideArrowMaskCounts[heading]; ++i) {
        const int px = centerX + s_wideArrowMasks[heading][i].x;
        const int py = centerY + s_wideArrowMasks[heading][i].y;
        if (!wideArrowPixelAllowed((float)px, (float)py)) {
          continue;
        }
        const size_t key = (size_t)py * LCD_WIDTH + (size_t)px;
        const uint8_t bit = (uint8_t)(1U << (key & 7U));
        uint8_t& cell = bitmap[key >> 3U];
        if (cell & bit) {
          ++shared;
        } else {
          cell |= bit;
        }
      }
      *maxShared = max(*maxShared, shared);
      if (shared >= kWideOverlapPixelLimit) {
        // 一个相位已经足以否决此布局；继续检查不会改变安全结论，只会
        // 延迟首次显示和按键抢占。
        return WIDE_AUDIT_OVERLAP;
      }
    }
  }
  return WIDE_AUDIT_SAFE;
}

static bool sameWideLayout(const WideLayoutState& state, int slots,
                           float routeLength) {
  return state.valid && state.slots == slots &&
         fabsf(state.routeLength - routeLength) < 0.6f;
}

static bool configureWideQueueWindow(int zoomSlot) {
  if (s_widePathLength < kWideQueueMinRouteLength || zoomSlot < 0 ||
      zoomSlot >= kZoomSlots) {
    return false;
  }
  buildWideArrowMasks();
  const uint32_t auditStartedAt = millis();
  uint8_t* bitmap = (uint8_t*)malloc(kWideAuditBitmapBytes);
  if (!bitmap) {
    s_wideAuditHidden = true;
    s_wideAuditMs = millis() - auditStartedAt;
    Serial.printf("wind corridor hidden z%d reason=bitmap bytes=%u\n",
                  s_seenZoom, (unsigned)kWideAuditBitmapBytes);
    return false;
  }

  const float endExclusion = min(
      kWideQueueEndExclusion,
      max(0.0f, s_widePathLength - kWideQueueMinRouteLength));
  const float fullRouteLength = s_widePathLength - endExclusion;
  s_wideAuditInitialSlots = max(
      kWideQueueMinSlots,
      min(kWideQueueMaxSlots,
          (int)lroundf(fullRouteLength / kWideQueueTargetSpacing)));

  float selectedLength = 0.0f;
  int selectedSlots = 0;
  uint16_t selectedShared = 0;
  uint16_t maximumShared = 0;
  bool aborted = false;
  float routeLength = fullRouteLength;
  while (true) {
    const int initialSlots = max(
        kWideQueueMinSlots,
        min(kWideQueueMaxSlots,
            (int)lroundf(routeLength / kWideQueueTargetSpacing)));
    const int minimumSlots =
        max(kWideQueueMinSlots, initialSlots - kWideQueueMaxReduction);
    for (int slots = initialSlots; slots >= minimumSlots; --slots) {
      uint16_t shared = 0;
      const WideAuditResult result =
          auditWideLayout(routeLength, slots, bitmap, &shared);
      maximumShared = max(maximumShared, shared);
      if (result == WIDE_AUDIT_ABORTED) {
        aborted = true;
        break;
      }
      if (result == WIDE_AUDIT_SAFE) {
        selectedLength = routeLength;
        selectedSlots = slots;
        selectedShared = shared;
        break;
      }
    }
    if (aborted || selectedSlots > 0) {
      break;
    }
    if (routeLength <= kWideQueueMinRouteLength + 0.01f) {
      break;
    }
    routeLength =
        max(kWideQueueMinRouteLength, routeLength - kWideQueueTrimStep);
  }

  if (aborted) {
    free(bitmap);
    return false;
  }
  if (selectedSlots == 0) {
    free(bitmap);
    s_wideAuditHidden = true;
    s_wideAuditMaxShared = maximumShared;
    s_wideAuditTrim = fullRouteLength - kWideQueueMinRouteLength;
    s_wideAuditMs = millis() - auditStartedAt;
    Serial.printf(
        "wind corridor hidden z%d reason=overlap initial=%d maxShared=%u trim=%.0f audit=%lums\n",
        s_seenZoom, s_wideAuditInitialSlots, (unsigned)maximumShared,
        s_wideAuditTrim, (unsigned long)s_wideAuditMs);
    return false;
  }

  // 安全方案若要求更少箭头或更短路径会立即生效；反向恢复则先保留当前
  // 仍安全的较保守布局，连续两个不同风场 revision 均安全才扩展。
  WideLayoutState& applied = s_wideApplied[zoomSlot];
  WideLayoutState& pending = s_widePending[zoomSlot];
  s_wideAuditHeld = false;
  if (applied.valid &&
      (selectedSlots > applied.slots ||
       selectedLength > applied.routeLength + 0.5f)) {
    const int conservativeSlots = min(selectedSlots, (int)applied.slots);
    const float conservativeLength =
        min(selectedLength, applied.routeLength);
    uint16_t conservativeShared = 0;
    const WideAuditResult conservativeResult = auditWideLayout(
        conservativeLength, conservativeSlots, bitmap, &conservativeShared);
    if (conservativeResult == WIDE_AUDIT_ABORTED) {
      free(bitmap);
      return false;
    }
    if (conservativeResult == WIDE_AUDIT_SAFE) {
      if (sameWideLayout(pending, selectedSlots, selectedLength) &&
          pending.revision != s_wideDirectionRevision) {
        ++pending.safeStreak;
        pending.revision = s_wideDirectionRevision;
      } else {
        pending = WideLayoutState{selectedLength, s_wideDirectionRevision,
                                  (uint8_t)selectedSlots, 1, true};
      }
      if (pending.safeStreak < 2) {
        selectedSlots = conservativeSlots;
        selectedLength = conservativeLength;
        selectedShared = conservativeShared;
        s_wideAuditHeld = true;
      } else {
        pending.valid = false;
      }
    }
  } else {
    pending.valid = false;
  }
  applied = WideLayoutState{selectedLength, s_wideDirectionRevision,
                            (uint8_t)selectedSlots, 0, true};
  free(bitmap);

  const float spacing = selectedLength / (float)selectedSlots;
  float minGap = 10000.0f;
  for (float distance = 0.0f;
       distance <= selectedLength - spacing + 0.01f;
       distance += max(1.0f, spacing * 0.25f)) {
    float x0 = 0.0f, y0 = 0.0f, tx0 = 0.0f, ty0 = 0.0f;
    float x1 = 0.0f, y1 = 0.0f, tx1 = 0.0f, ty1 = 0.0f;
    if (!sampleWidePath(distance, &x0, &y0, &tx0, &ty0) ||
        !sampleWidePath(distance + spacing, &x1, &y1, &tx1, &ty1)) {
      return false;
    }
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    minGap = min(minGap, sqrtf(dx * dx + dy * dy));
  }

  const float oldSpacing = s_wideQueueSpacing;
  const float phaseRatio = oldSpacing > 0.001f
                               ? fmodf(s_wideQueuePhase, oldSpacing) /
                                     oldSpacing
                               : 0.0f;
  s_wideQueueSpacing = spacing;
  s_wideQueueSlotCount = selectedSlots;
  s_wideWindowStart = 0.0f;
  s_wideWindowLength = selectedLength;
  s_wideQueuePhase = phaseRatio * spacing;
  s_wideWindowMinGap = minGap;
  s_wideWindowFallback = selectedSlots < s_wideAuditInitialSlots ||
                         selectedLength < fullRouteLength - 0.5f;
  s_wideAuditMaxShared = selectedShared;
  s_wideAuditTrim = fullRouteLength - selectedLength;
  s_wideAuditMs = millis() - auditStartedAt;
  return true;
}

static bool buildWidePath(uint32_t revision) {
  // updateWideQueueDirection 在新 revision 时只更新主导向量；候选通道只
  // 在缩放/风场变化时评分一次，之后所有动画帧复用。
  if (s_wideDirectionRevision != revision &&
      !updateWideQueueDirection(revision)) {
    return false;
  }
  if (s_wideAuditHidden && s_wideAuditRevision == revision) {
    return false;
  }
  if (s_widePathCount >= 2) {
    return true;
  }
  if (s_wideDirectionRevision != revision || !s_wideDirectionValid ||
      s_seenZoom < ZOOM_MIN || s_seenZoom > ZOOM_MAX) {
    return false;
  }

  WidePathNode candidate[kWidePathCapacity];
  int candidateCount = 0;
  float candidateLength = 0.0f;
  float candidateScore = 0.0f;
  float bestX = (float)LCD_WIDTH * 0.5f;
  float bestY = (float)LCD_HEIGHT * 0.5f;
  float bestScore = -1.0f;
  const int seedSlot = s_seenZoom - ZOOM_MIN;
  float previousScore = -1.0f;
  bool previousUsable = false;

  if (s_wideSeedValid[seedSlot]) {
    previousUsable = traceWidePath(
        s_wideSeedX[seedSlot], s_wideSeedY[seedSlot], candidate,
        &candidateCount, &candidateLength, &previousScore);
    if (previousUsable) {
      bestX = s_wideSeedX[seedSlot];
      bestY = s_wideSeedY[seedSlot];
      bestScore = previousScore;
    }
  }

  for (int gy = 0; gy < kWideSeedGrid; ++gy) {
    const float y = 36.0f + (float)gy * 37.0f;
    for (int gx = 0; gx < kWideSeedGrid; ++gx) {
      const float x = 44.0f + (float)gx * 38.0f;
      if (!traceWidePath(x, y, candidate, &candidateCount,
                         &candidateLength, &candidateScore)) {
        continue;
      }
      float rankedScore = candidateScore;
      if (previousUsable) {
        const float dx = x - s_wideSeedX[seedSlot];
        const float dy = y - s_wideSeedY[seedSlot];
        // 相近通道优先，抑制相邻网格之间无意义的来回跳动。
        rankedScore -= sqrtf(dx * dx + dy * dy) * 0.18f;
      }
      if (rankedScore > bestScore) {
        bestScore = rankedScore;
        bestX = x;
        bestY = y;
      }
    }
  }

  bool switched = !previousUsable;
  if (previousUsable && (bestX != s_wideSeedX[seedSlot] ||
                         bestY != s_wideSeedY[seedSlot])) {
    // 新通道至少强 15% 才允许换道；否则保留上一轮的稳定位置。
    if (bestScore <= previousScore * 1.15f) {
      bestX = s_wideSeedX[seedSlot];
      bestY = s_wideSeedY[seedSlot];
      bestScore = previousScore;
    } else {
      switched = true;
    }
  }

  if (!traceWidePath(bestX, bestY, s_widePath, &s_widePathCount,
                     &s_widePathLength, &candidateScore)) {
    s_widePathCount = 0;
    s_widePathLength = 0.0f;
    return false;
  }
  s_wideSeedX[seedSlot] = bestX;
  s_wideSeedY[seedSlot] = bestY;
  s_wideSeedValid[seedSlot] = true;
  if (!configureWideQueueWindow(seedSlot)) {
    // 没有安全布局时保留已构建路径但隐藏本 revision，避免每帧重复做
    // 昂贵审计；按键中止则允许后续重试。
    if (!s_wideAuditHidden) {
      s_widePathCount = 0;
      s_widePathLength = 0.0f;
    }
    return false;
  }
  s_wideColorSamplePending = true;
  Serial.printf(
      "wind corridor z%d seed=(%.0f,%.0f) switched=%d score=%.1f nodes=%d length=%.1f route=%.1f..%.1f initial=%d slots=%d spacing=%.1f minGap=%.1f shared=%u trim=%.0f held=%d audit=%lums\n",
      s_seenZoom, bestX, bestY, (int)switched, candidateScore,
      s_widePathCount, s_widePathLength, s_wideWindowStart,
      s_wideWindowStart + s_wideWindowLength, s_wideAuditInitialSlots,
      s_wideQueueSlotCount, s_wideQueueSpacing, s_wideWindowMinGap,
      (unsigned)s_wideAuditMaxShared, s_wideAuditTrim,
      (int)s_wideAuditHeld, (unsigned long)s_wideAuditMs);
  return true;
}

static void wideDirectionUnitVector(int heading, float* forwardX,
                                    float* forwardY) {
  const float angle = (float)heading * 6.28318530718f /
                      (float)kWideDirectionCount;
  *forwardX = cosf(angle);
  *forwardY = sinf(angle);
}

static int quantizeWideDirection(float forwardX, float forwardY) {
  int best = 0;
  float bestDot = -2.0f;
  for (int heading = 0; heading < kWideDirectionCount; ++heading) {
    const float dot = forwardX * s_wideDirectionVectors[heading][0] +
                      forwardY * s_wideDirectionVectors[heading][1];
    if (dot > bestDot) {
      bestDot = dot;
      best = heading;
    }
  }
  return best;
}

static void buildWideArrowMasks() {
  if (s_wideArrowMasksReady) {
    return;
  }
  int minCount = kWideArrowMaskCapacity;
  int maxCount = 0;
  for (int heading = 0; heading < kWideDirectionCount; ++heading) {
    float forwardX = 0.0f;
    float forwardY = 0.0f;
    wideDirectionUnitVector(heading, &forwardX, &forwardY);
    s_wideDirectionVectors[heading][0] = forwardX;
    s_wideDirectionVectors[heading][1] = forwardY;
    int count = 0;
    // 反向扫描目标点阵：把每个目标像素中心变换回箭头局部坐标，再判断
    // 是否落在 6px 实心折角内。这样斜向旋转也不会产生正向映射的空洞。
    for (int y = -14; y <= 14; ++y) {
      for (int x = -14; x <= 14; ++x) {
        const float along = (float)x * forwardX + (float)y * forwardY;
        const float lateral = -(float)x * forwardY + (float)y * forwardX;
        const float absLateral = fabsf(lateral);
        const float front = -absLateral;
        if (absLateral > 6.49f || along < front - 5.49f ||
            along > front + 0.49f) {
          continue;
        }
        if (count < kWideArrowMaskCapacity) {
          s_wideArrowMasks[heading][count++] =
              ArrowOffset{(int8_t)x, (int8_t)y};
        }
      }
    }
    s_wideArrowMaskCounts[heading] = (uint8_t)count;
    minCount = min(minCount, count);
    maxCount = max(maxCount, count);
  }
  s_wideArrowMasksReady = true;
  Serial.printf("wind wide solid masks directions=%d pixels=%d..%d\n",
                kWideDirectionCount, minCount, maxCount);
}

static void updateParticle(Particle* p, float dtSec, uint32_t dtMs,
                           int8_t* heading) {
  if (!p || !heading) {
    return;
  }
  p->ageMs += dtMs;
  if (p->ageMs >= p->lifeMs || !pointAllowed(p->x[0], p->y[0])) {
    spawnParticle(p);
    *heading = -1;
    return;
  }

  float east = 0.0f;
  float south = 0.0f;
  if (!windFieldSample(p->x[0], p->y[0], &east, &south)) {
    *heading = -1;
    return;
  }
  const float mag = sqrtf(east * east + south * south);
  float pxPerSec = 6.0f + mag * 1.25f;
  if (pxPerSec > 24.0f) {
    pxPerSec = 24.0f;
  }
  float vx = 0.0f;
  float vy = 0.0f;
  if (mag >= 0.08f) {
    vx = east / mag * pxPerSec;
    vy = south / mag * pxPerSec;
    *heading = quantizeHeading(east, south);
  } else {
    *heading = -1;
  }

  for (int i = WIND_TRAIL_POINTS - 1; i > 0; --i) {
    p->x[i] = p->x[i - 1];
    p->y[i] = p->y[i - 1];
  }
  p->x[0] += vx * dtSec;
  p->y[0] += vy * dtSec;
  if (!pointAllowed(p->x[0], p->y[0])) {
    spawnParticle(p);
    *heading = -1;
  }
}

static int trailLowerBound(uint16_t key) {
  int lo = 0;
  int hi = s_trailCount;
  while (lo < hi) {
    const int mid = lo + (hi - lo) / 2;
    if (s_trails[mid].key < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

static int findTrail(uint16_t key) {
  const int i = trailLowerBound(key);
  return i < s_trailCount && s_trails[i].key == key ? i : -1;
}

static int newKeyLowerBound(uint16_t key) {
  int lo = 0;
  int hi = s_newCount;
  while (lo < hi) {
    const int mid = lo + (hi - lo) / 2;
    if (s_newKeys[mid] < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

static bool insertNewKey(uint16_t key, uint8_t flags,
                         uint8_t strength = 255) {
  const int pos = newKeyLowerBound(key);
  if (pos < s_newCount && s_newKeys[pos] == key) {
    s_newFlags[pos] |= flags;
    s_newStrength[pos] = max(s_newStrength[pos], strength);
    return true;
  }
  if (s_newCount >= kNewPixelCapacity) {
    return false;
  }
  memmove(&s_newKeys[pos + 1], &s_newKeys[pos],
          (size_t)(s_newCount - pos) * sizeof(s_newKeys[0]));
  memmove(&s_newFlags[pos + 1], &s_newFlags[pos],
          (size_t)(s_newCount - pos) * sizeof(s_newFlags[0]));
  memmove(&s_newStrength[pos + 1], &s_newStrength[pos],
          (size_t)(s_newCount - pos) * sizeof(s_newStrength[0]));
  s_newKeys[pos] = key;
  s_newFlags[pos] = flags;
  s_newStrength[pos] = strength;
  ++s_newCount;
  return true;
}

static void queueTrailPixel(int x, int y) {
  if (!pointAllowed((float)x, (float)y)) {
    return;
  }
  const uint16_t key = (uint16_t)(y * LCD_WIDTH + x);
  const int existing = findTrail(key);
  if (existing >= 0) {
    s_trails[existing].strength = 255;
    // 真正的运动轨迹经过旧箭翼时，将该像素升级为持久拖影。
    s_trails[existing].flags &= (uint8_t)~kTrailTransientHead;
    return;
  }
  const int pos = newKeyLowerBound(key);
  if (pos < s_newCount && s_newKeys[pos] == key) {
    s_newFlags[pos] &= (uint8_t)~kTrailTransientHead;
    s_newStrength[pos] = 255;
    return;
  }
  insertNewKey(key, 0);
}

static void queueHeadPixel(int x, int y) {
  if (!pointAllowed((float)x, (float)y)) {
    return;
  }
  const uint16_t key = (uint16_t)(y * LCD_WIDTH + x);
  const int existing = findTrail(key);
  if (existing >= 0) {
    s_trails[existing].flags |= kTrailHead;
    s_trails[existing].strength = 255;
    return;
  }
  const int pos = newKeyLowerBound(key);
  if (pos < s_newCount && s_newKeys[pos] == key) {
    s_newFlags[pos] |= kTrailHead;
    s_newStrength[pos] = 255;
    return;
  }
  // 纯箭翼没有强度，不参与渐隐；HEAD 标记保证它在本帧恢复阶段不被移除。
  insertNewKey(key, kTrailHead | kTrailTransientHead);
}

static void queueNewestSegment(const Particle& p) {
  int x0 = (int)lroundf(p.x[1]);
  int y0 = (int)lroundf(p.y[1]);
  const int x1 = (int)lroundf(p.x[0]);
  const int y1 = (int)lroundf(p.y[0]);
  const int dx = abs(x1 - x0);
  const int sx = x0 < x1 ? 1 : -1;
  const int dy = -abs(y1 - y0);
  const int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;

  // 正常最大位移不到 9px；上限防止异常坐标造成长循环。
  for (int guard = 0; guard < 32; ++guard) {
    queueTrailPixel(x0, y0);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

static void clearPreviousHeads() {
  for (int i = 0; i < s_trailCount; ++i) {
    s_trails[i].flags &= (uint8_t)~kTrailHead;
  }
}

static void queueParticleHead(const Particle& p, int8_t heading) {
  const int x = (int)lroundf(p.x[0]);
  const int y = (int)lroundf(p.y[0]);
  if (s_style != WIND_PARTICLE_OPEN_ARROW || heading < 0 || heading >= 8) {
    queueHeadPixel(x, y);
    return;
  }
  for (const ArrowOffset& offset : kOpenArrowOffsets[heading]) {
    queueHeadPixel(x + offset.x, y + offset.y);
  }
}

static void appendWideHeadKey(int x, int y, uint8_t alpha) {
  if (!wideArrowPixelAllowed((float)x, (float)y) ||
      s_newCount >= kNewPixelCapacity) {
    return;
  }
  s_newKeys[s_newCount] = (uint16_t)(y * LCD_WIDTH + x);
  s_newStrength[s_newCount] = alpha;
  ++s_newCount;
}

static void queueWideArrowAt(float screenX, float screenY, int heading,
                             uint8_t alpha) {
  const int x = (int)lroundf(screenX);
  const int y = (int)lroundf(screenY);
  if (heading < 0 || heading >= kWideDirectionCount || alpha == 0) {
    return;
  }

  buildWideArrowMasks();
  const int count = s_wideArrowMaskCounts[heading];
  for (int i = 0; i < count; ++i) {
    const ArrowOffset offset = s_wideArrowMasks[heading][i];
    appendWideHeadKey(x + offset.x, y + offset.y, alpha);
  }
}

static uint8_t wideWindowAlpha(float localDistance) {
  return wideLayoutAlpha(localDistance, s_wideWindowLength,
                         s_wideQueueSpacing);
}

static uint16_t brightenCloudColor(uint16_t color) {
  int r = (int)(((color >> 11) & 0x1F) * 255U / 31U);
  int g = (int)(((color >> 5) & 0x3F) * 255U / 63U);
  int b = (int)((color & 0x1F) * 255U / 31U);
  const int peak = max(r, max(g, b));
  // 雷达低级别色有时较暗；只抬高亮度、不改变色相，避免同色箭头
  // 完全融入云图。高亮黄/红等原色保持不变。
  constexpr int kMinimumPeak = 184;
  if (peak > 0 && peak < kMinimumPeak) {
    r = min(255, r * kMinimumPeak / peak);
    g = min(255, g * kMinimumPeak / peak);
    b = min(255, b * kMinimumPeak / peak);
  }
  return (uint16_t)(((uint16_t)(r >> 3) << 11) |
                    ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
}

static uint16_t currentWideQueueColor(uint32_t now) {
  if (!s_wideColorReady || s_wideColorCurrent == s_wideColorTarget) {
    return s_wideColorCurrent;
  }
  const uint32_t elapsed = now - s_wideColorTransitionAt;
  if (elapsed >= kWideColorTransitionMs) {
    s_wideColorCurrent = s_wideColorTarget;
    s_wideColorFrom = s_wideColorTarget;
    return s_wideColorCurrent;
  }
  const uint8_t alpha =
      (uint8_t)((elapsed * 255UL) / kWideColorTransitionMs);
  s_wideColorCurrent = blend565(s_wideColorTarget, s_wideColorFrom, alpha);
  return s_wideColorCurrent;
}

static void refreshWideQueueColor() {
  if (!s_wideColorSamplePending || s_widePathCount < 2 ||
      s_seenZoom < ZOOM_MIN || s_seenZoom > ZOOM_MAX) {
    return;
  }
  s_wideColorSamplePending = false;

  // 在入口淡入段中点取样：这里是用户实际看见第一个箭头的位置，且比
  // 圆屏裁切边缘更稳定。整列只采用这一个颜色。
  const float sampleDistance =
      s_wideWindowStart +
      min(kWideQueueFadeLength * 0.5f, s_wideWindowLength);
  float x = s_widePath[0].x;
  float y = s_widePath[0].y;
  float tangentX = 0.0f;
  float tangentY = 0.0f;
  (void)sampleWidePath(sampleDistance, &x, &y, &tangentX, &tangentY);

  bool hasCloud = false;
  uint16_t radarColor = 0;
  const bool sampled = frameCacheSampleRadarColor(
      s_seenZoom, (int)lroundf(x), (int)lroundf(y), &hasCloud, &radarColor);
  const uint16_t selected =
      sampled && hasCloud && radarColor != 0 ? brightenCloudColor(radarColor)
                                             : kWideFallbackColor;
  const uint32_t now = millis();
  if (!s_wideColorReady) {
    s_wideColorCurrent = selected;
    s_wideColorFrom = selected;
    s_wideColorTarget = selected;
    s_wideColorReady = true;
  } else if (selected != s_wideColorTarget) {
    s_wideColorFrom = currentWideQueueColor(now);
    s_wideColorTarget = selected;
    s_wideColorTransitionAt = now;
  }
  s_wideColorHasCloud = sampled && hasCloud && radarColor != 0;
  Serial.printf(
      "wind corridor color z%d at=(%.0f,%.0f) grid=%d cloud=%d raw=%04x target=%04x\n",
      s_seenZoom, x, y, (int)sampled, (int)s_wideColorHasCloud,
      (unsigned)radarColor, (unsigned)selected);
}

static int queueWideArrowTrain(uint32_t revision, float dtSec) {
  buildWideArrowMasks();
  if (!buildWidePath(revision)) {
    return 0;
  }
  refreshWideQueueColor();

  const uint32_t now = millis();
  const float queueSpeed = currentWideQueueSpeed(now);
  if (s_wideQueueSpacing > 0.001f) {
    s_wideQueuePhase =
        fmodf(s_wideQueuePhase + queueSpeed * dtSec, s_wideQueueSpacing);
    if (s_wideQueuePhase < 0.0f) {
      s_wideQueuePhase += s_wideQueueSpacing;
    }
  }

  for (int slot = 0; slot < s_wideQueueSlotCount; ++slot) {
    // phase 只滚动一个间距。出口箭头逐渐消失的同时，入口箭头逐渐出现；
    // 中间箭头持续前进，不存在整组抵达、等待和整体重启。
    const float localDistance =
        s_wideQueuePhase + (float)slot * s_wideQueueSpacing;
    if (localDistance > s_wideWindowLength) {
      continue;
    }
    const float distance = s_wideWindowStart + localDistance;
    float x = 0.0f;
    float y = 0.0f;
    float tangentX = 0.0f;
    float tangentY = 0.0f;
    if (!sampleWidePath(distance, &x, &y, &tangentX, &tangentY)) {
      continue;
    }
    const uint8_t alpha = wideWindowAlpha(localDistance);
    queueWideArrowAt(x, y, quantizeWideDirection(tangentX, tangentY), alpha);
  }
  return s_wideQueueSlotCount;
}

static void finalizeWideHeadKeys() {
  if (s_newCount <= 0) {
    return;
  }
  const int rawCount = s_newCount;
  // 对 key/alpha 成对做原地 Shell 排序，避免为不到 900 个像素再常驻一份
  // 3.5KB 临时数组；同 key 时 alpha 升序，去重时自然取最后的最大值。
  for (int gap = rawCount / 2; gap > 0; gap /= 2) {
    for (int i = gap; i < rawCount; ++i) {
      const uint16_t key = s_newKeys[i];
      const uint8_t alpha = s_newStrength[i];
      int j = i;
      while (j >= gap &&
             (s_newKeys[j - gap] > key ||
              (s_newKeys[j - gap] == key &&
               s_newStrength[j - gap] > alpha))) {
        s_newKeys[j] = s_newKeys[j - gap];
        s_newStrength[j] = s_newStrength[j - gap];
        j -= gap;
      }
      s_newKeys[j] = key;
      s_newStrength[j] = alpha;
    }
  }

  // 旧箭头数组同样有序。重叠位置只标为本帧头部，真正的新位置原地压缩，
  // 后续只为它们读取一次底色；避免上千次有序插入和 memmove。
  int old = 0;
  int write = 0;
  int read = 0;
  while (read < rawCount) {
    const uint16_t key = s_newKeys[read];
    uint8_t alpha = s_newStrength[read];
    ++read;
    while (read < rawCount && s_newKeys[read] == key) {
      alpha = max(alpha, s_newStrength[read]);
      ++read;
    }
    while (old < s_trailCount && s_trails[old].key < key) {
      ++old;
    }
    if (old < s_trailCount && s_trails[old].key == key) {
      s_trails[old].flags |= kTrailHead;
      s_trails[old].strength = alpha;
      continue;
    }
    s_newKeys[write] = key;
    s_newFlags[write] = kTrailHead | kTrailTransientHead;
    s_newStrength[write] = alpha;
    ++write;
  }
  s_newCount = write;
}

static uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t alpha) {
  const uint32_t inv = 255U - alpha;
  const uint32_t fr = (fg >> 11) & 0x1F;
  const uint32_t fg6 = (fg >> 5) & 0x3F;
  const uint32_t fb = fg & 0x1F;
  const uint32_t br = (bg >> 11) & 0x1F;
  const uint32_t bg6 = (bg >> 5) & 0x3F;
  const uint32_t bb = bg & 0x1F;
  const uint32_t r = (fr * alpha + br * inv + 127U) / 255U;
  const uint32_t g = (fg6 * alpha + bg6 * inv + 127U) / 255U;
  const uint32_t b = (fb * alpha + bb * inv + 127U) / 255U;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void decayTrails(uint32_t dtMs) {
  uint32_t amount = (dtMs * 255UL + kTrailFadeMs - 1UL) / kTrailFadeMs;
  if (amount < 1UL) {
    amount = 1UL;
  }
  if (amount > 255UL) {
    amount = 255UL;
  }
  for (int i = 0; i < s_trailCount; ++i) {
    const uint8_t strength = s_trails[i].strength;
    s_trails[i].strength = strength > amount ? strength - amount : 0;
  }
}

static int restoreAndRemoveExpired(LGFX* lcd) {
  const bool prevSwap = lcd->getSwapBytes();
  lcd->setSwapBytes(true);
  lcd->startWrite();
  int write = 0;
  int spanX = 0;
  int spanY = 0;
  int spanLen = 0;
  int spans = 0;
  auto flushSpan = [&]() {
    if (spanLen <= 0) {
      return;
    }
    if (spanLen == 1) {
      lcd->drawPixel(spanX, spanY, s_spanBuf[0]);
    } else {
      lcd->pushImage(spanX, spanY, spanLen, 1, s_spanBuf);
    }
    ++spans;
    spanLen = 0;
  };

  // 输入按 key 排序；稳定压缩保留此顺序，同时把相邻过期像素合并恢复。
  for (int read = 0; read < s_trailCount; ++read) {
    const TrailPixel trail = s_trails[read];
    if (trail.strength != 0 || (trail.flags & kTrailHead)) {
      s_trails[write++] = trail;
      continue;
    }
    const int x = trail.key % LCD_WIDTH;
    const int y = trail.key / LCD_WIDTH;
    if (spanLen == 0 || y != spanY || x != spanX + spanLen) {
      flushSpan();
      spanX = x;
      spanY = y;
    }
    s_spanBuf[spanLen++] = trail.base565;
  }
  flushSpan();
  s_trailCount = write;
  lcd->endWrite();
  lcd->setSwapBytes(prevSwap);
  return spans;
}

static int sampleAndInsertNew(int zoom) {
  if (s_newCount <= 0) {
    return 0;
  }
  if (!frameCacheReadPixelsSorted(zoom, s_newKeys, s_newBase,
                                  (size_t)s_newCount)) {
    const uint32_t now = millis();
    if (now - s_lastSampleFailLogAt > 5000UL) {
      s_lastSampleFailLogAt = now;
      Serial.printf("wind trail base sample fail z%d new=%d\n", zoom,
                    s_newCount);
    }
    return 0;
  }

  const int available = kTrailPixelCapacity - s_trailCount;
  int persistentCount = 0;
  for (int i = 0; i < s_newCount; ++i) {
    if ((s_newFlags[i] & kTrailTransientHead) == 0) {
      ++persistentCount;
    }
  }
  const int persistentBudget = min(persistentCount, available);
  const int transientBudget = max(0, available - persistentBudget);
  int persistentUsed = 0;
  int transientUsed = 0;
  int accepted = 0;
  // 容量不足时先保留真实运动轨迹，再按 key 顺序接受剩余箭翼；压缩后仍有序。
  for (int read = 0; read < s_newCount; ++read) {
    const bool transient = s_newFlags[read] & kTrailTransientHead;
    if ((!transient && persistentUsed >= persistentBudget) ||
        (transient && transientUsed >= transientBudget)) {
      continue;
    }
    if (transient) {
      ++transientUsed;
    } else {
      ++persistentUsed;
    }
    s_newKeys[accepted] = s_newKeys[read];
    s_newBase[accepted] = s_newBase[read];
    s_newFlags[accepted] = s_newFlags[read];
    s_newStrength[accepted] = s_newStrength[read];
    ++accepted;
  }
  if (accepted < s_newCount) {
    const uint32_t now = millis();
    if (now - s_lastCapacityLogAt > 5000UL) {
      s_lastCapacityLogAt = now;
      Serial.printf("wind trail pool full active=%d dropped=%d\n",
                    s_trailCount, s_newCount - accepted);
    }
  }

  // 两个输入均有序，从尾部原地合并，已有轨迹永远优先保留。
  int old = s_trailCount - 1;
  int fresh = accepted - 1;
  int out = s_trailCount + accepted - 1;
  while (old >= 0 && fresh >= 0) {
    if (s_trails[old].key > s_newKeys[fresh]) {
      s_trails[out--] = s_trails[old--];
    } else {
      const uint8_t flags = s_newFlags[fresh];
      const uint8_t strength = s_newStrength[fresh];
      s_trails[out--] = TrailPixel{s_newKeys[fresh], s_newBase[fresh],
                                   strength, flags};
      --fresh;
    }
  }
  while (fresh >= 0) {
    const uint8_t flags = s_newFlags[fresh];
    const uint8_t strength = s_newStrength[fresh];
    s_trails[out--] =
        TrailPixel{s_newKeys[fresh], s_newBase[fresh], strength, flags};
    --fresh;
  }
  s_trailCount += accepted;
  return accepted;
}

static int drawTrailsAndHeads(LGFX* lcd) {
  const uint16_t trailColor = lcd->color565(105, 211, 232);
  const uint16_t headColor = lcd->color565(225, 253, 255);
  const bool prevSwap = lcd->getSwapBytes();
  lcd->setSwapBytes(true);
  lcd->startWrite();
  int spans = 0;
  int i = 0;
  while (i < s_trailCount) {
    const uint16_t firstKey = s_trails[i].key;
    const int x0 = firstKey % LCD_WIDTH;
    const int y = firstKey / LCD_WIDTH;
    int j = i;
    while (j < s_trailCount && s_trails[j].key / LCD_WIDTH == y &&
           s_trails[j].key == firstKey + (j - i)) {
      const TrailPixel& trail = s_trails[j];
      s_spanBuf[j - i] = (trail.flags & kTrailHead)
                            ? headColor
                            : blend565(trailColor, trail.base565,
                                       trail.strength);
      ++j;
    }
    const int len = j - i;
    if (len == 1) {
      lcd->drawPixel(x0, y, s_spanBuf[0]);
    } else {
      lcd->pushImage(x0, y, len, 1, s_spanBuf);
    }
    ++spans;
    i = j;
  }
  lcd->endWrite();
  lcd->setSwapBytes(prevSwap);
  return spans;
}

static int drawArrowFrame(LGFX* lcd) {
  const bool wideFade = s_style == WIND_PARTICLE_WIDE_ARROW;
  const uint16_t headColor =
      wideFade ? currentWideQueueColor(millis()) : kWideFallbackColor;
  const bool prevSwap = lcd->getSwapBytes();
  lcd->setSwapBytes(true);
  lcd->startWrite();
  int spans = 0;
  int i = 0;
  while (i < s_trailCount) {
    const uint16_t firstKey = s_trails[i].key;
    const int x0 = firstKey % LCD_WIDTH;
    const int y = firstKey / LCD_WIDTH;
    int j = i;
    while (j < s_trailCount && s_trails[j].key / LCD_WIDTH == y &&
           s_trails[j].key == firstKey + (j - i)) {
      const TrailPixel& pixel = s_trails[j];
      // 旧箭头和新箭头的并集一次性提交最终颜色：旧位置直接恢复底图，
      // 新位置直接画亮色，不存在“全体先消失、随后再出现”的清屏相位。
      if (pixel.flags & kTrailHead) {
        s_spanBuf[j - i] =
            wideFade
                ? blend565(headColor, pixel.base565, pixel.strength)
                : headColor;
      } else {
        s_spanBuf[j - i] = pixel.base565;
      }
      ++j;
    }
    const int len = j - i;
    if (len == 1) {
      lcd->drawPixel(x0, y, s_spanBuf[0]);
    } else {
      lcd->pushImage(x0, y, len, 1, s_spanBuf);
    }
    ++spans;
    i = j;
  }
  lcd->endWrite();
  lcd->setSwapBytes(prevSwap);

  // 旧位置已恢复，只保留本帧箭头作为下一帧的 underlay 记录。
  int write = 0;
  for (int read = 0; read < s_trailCount; ++read) {
    if (s_trails[read].flags & kTrailHead) {
      s_trails[write++] = s_trails[read];
    }
  }
  s_trailCount = write;
  return spans;
}

}  // namespace

void windParticlesBegin() { windParticlesReset(); }

void windParticlesSetStyle(WindParticleStyle style) {
  if (style == WIND_PARTICLE_OPEN_ARROW ||
      style == WIND_PARTICLE_WIDE_ARROW) {
    s_style = style;
  } else {
    s_style = WIND_PARTICLE_DOT;
  }
}

void windParticlesReset() {
  memset(s_particles, 0, sizeof(s_particles));
  memset(s_headings, -1, sizeof(s_headings));
  s_trailCount = 0;
  s_newCount = 0;
  s_seenZoom = -1;
  s_seenRevision = 0;
  s_lastFrameAt = 0;
  s_statsFrames = 0;
  s_statsSpans = 0;
  s_activeParticleCount = styleParticleCount();
  s_wideQueueSlotCount = 0;
  s_wideQueuePhase = 0.0f;
  s_wideQueueSpacing = 22.0f;
  s_wideQueueSpeed = kWideQueueMinimumSpeed;
  s_wideQueueSpeedFrom = kWideQueueMinimumSpeed;
  s_wideQueueSpeedTarget = kWideQueueMinimumSpeed;
  s_wideQueueSpeedTransitionAt = 0;
  s_wideCenterSpeed = 0.0f;
  s_wideCenterSpeedValid = false;
  s_wideWindowStart = 0.0f;
  s_wideWindowLength = 0.0f;
  s_wideWindowMinGap = 0.0f;
  s_wideWindowFallback = false;
  s_wideDominantX = 1.0f;
  s_wideDominantY = 0.0f;
  s_wideDirectionRevision = 0;
  s_wideDirectionValid = false;
  s_widePathCount = 0;
  s_widePathLength = 0.0f;
  s_wideColorCurrent = kWideFallbackColor;
  s_wideColorFrom = kWideFallbackColor;
  s_wideColorTarget = kWideFallbackColor;
  s_wideColorTransitionAt = 0;
  s_wideColorReady = false;
  s_wideColorSamplePending = true;
  s_wideColorHasCloud = false;
  s_wideAuditRevision = 0;
  s_wideAuditHidden = false;
  s_wideAuditMaxShared = 0;
  s_wideAuditTrim = 0.0f;
  s_wideAuditMs = 0;
  s_wideAuditInitialSlots = 0;
  s_wideAuditHeld = false;
  memset(s_wideApplied, 0, sizeof(s_wideApplied));
  memset(s_widePending, 0, sizeof(s_widePending));
}

void windParticlesNotifyBaseRedrawn() {
  // 调用方刚完成整屏 blit/push；无需逐点恢复，直接忘掉旧底色。
  s_trailCount = 0;
  s_newCount = 0;
  // 雷达成品可能已更新；下个风场帧只重采样一次入口颜色。
  s_wideColorSamplePending = true;
}

bool windParticlesTick(LGFX* lcd, int displayedZoom, bool busy) {
  if (!lcd || displayedZoom < ZOOM_MIN || displayedZoom > ZOOM_MAX ||
      !windFieldReadyFor(displayedZoom) || !frameCacheHas(displayedZoom)) {
    return false;
  }
  const uint32_t now = millis();
  const uint32_t interval = busy ? WIND_BUSY_FRAME_MS : WIND_FRAME_MS;
  if (s_lastFrameAt != 0 && (now - s_lastFrameAt) < interval) {
    return false;
  }

  const uint32_t revision = windFieldRevision();
  if (s_seenZoom != displayedZoom) {
    resetAll(displayedZoom, revision);
  } else if (s_seenRevision != revision) {
    // 新风场直接作用于现有粒子，旧方向轨迹自然渐隐，避免全体重生闪变。
    s_seenRevision = revision;
    Serial.printf("wind particles adopt fieldRev=%lu without reset\n",
                  (unsigned long)revision);
  }

  uint32_t dtMs = s_lastFrameAt == 0 ? interval : (now - s_lastFrameAt);
  if (dtMs > 350UL) {
    dtMs = 350UL;
  }
  s_lastFrameAt = now;
  const float dtSec = (float)dtMs * 0.001f;

  const uint32_t frameWorkStart = millis();
  clearPreviousHeads();
  const bool arrowStyle = s_style == WIND_PARTICLE_OPEN_ARROW ||
                          s_style == WIND_PARTICLE_WIDE_ARROW;
  if (!arrowStyle) {
    decayTrails(dtMs);
  }
  s_newCount = 0;
  if (s_style == WIND_PARTICLE_WIDE_ARROW) {
    // 粗箭头只表达整幅画面的主导风向，不再维护多个独立粒子。
    s_activeParticleCount = queueWideArrowTrain(revision, dtSec);
    finalizeWideHeadKeys();
  } else {
    for (int i = 0; i < s_activeParticleCount; ++i) {
      updateParticle(&s_particles[i], dtSec, dtMs, &s_headings[i]);
      if (!arrowStyle) {
        queueNewestSegment(s_particles[i]);
      }
    }
    for (int i = 0; i < s_activeParticleCount; ++i) {
      queueParticleHead(s_particles[i], s_headings[i]);
    }
  }

  const uint32_t drawStart = millis();
  int restoreSpans = 0;
  int drawSpans = 0;
  if (arrowStyle) {
    // 箭头模式没有持久拖尾。先合并当前箭头，再把旧/新位置的最终颜色
    // 一次提交，避免两个绘制阶段之间出现全体闪烁。
    sampleAndInsertNew(displayedZoom);
    drawSpans = drawArrowFrame(lcd);
  } else {
    // 亮点模式保留原有 1500ms 渐隐轨迹。
    restoreSpans = restoreAndRemoveExpired(lcd);
    sampleAndInsertNew(displayedZoom);
    drawSpans = drawTrailsAndHeads(lcd);
  }

  const uint32_t frameWorkMs = millis() - frameWorkStart;

  ++s_statsFrames;
  s_statsSpans += (uint32_t)(restoreSpans + drawSpans);
  if (s_statsAt == 0) {
    s_statsAt = now;
  } else if ((now - s_statsAt) >= 5000UL) {
    const float fps = (float)s_statsFrames * 1000.0f / (float)(now - s_statsAt);
    const unsigned avgSpans =
        s_statsFrames > 0 ? (unsigned)(s_statsSpans / s_statsFrames) : 0;
    Serial.printf(
        "wind anim fps=%.1f draw=%lums total=%lums busy=%d style=%u count=%d trails=%d new=%d spans=%u center=%.1f speed=%.1f/%.1f\n",
        fps, (unsigned long)(millis() - drawStart),
        (unsigned long)frameWorkMs, (int)busy, (unsigned)s_style,
        s_activeParticleCount, s_trailCount, s_newCount, avgSpans,
        s_wideCenterSpeed, s_wideQueueSpeed, s_wideQueueSpeedTarget);
    s_statsAt = now;
    s_statsFrames = 0;
    s_statsSpans = 0;
  }
  return true;
}
