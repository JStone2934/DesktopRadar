#include "wind_particles.h"

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "frame_cache.h"
#include "wind_field.h"

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
  uint8_t flags;      // bit0=本帧粒子头；占用原结构体对齐填充
};

static_assert(sizeof(TrailPixel) == 6, "TrailPixel must stay compact");

static constexpr int kTrailPixelCapacity = 3072;
static constexpr int kNewPixelCapacity = 640;
static constexpr uint32_t kTrailFadeMs = 1500UL;

static Particle s_particles[WIND_PARTICLE_COUNT];
static TrailPixel s_trails[kTrailPixelCapacity];
static uint16_t s_newKeys[kNewPixelCapacity];
static uint16_t s_newBase[kNewPixelCapacity];
static uint16_t s_spanBuf[LCD_WIDTH];
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

static constexpr uint8_t kTrailHead = 0x01;

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
  Serial.printf("wind particles reset z%d count=%d fieldRev=%lu\n", zoom,
                WIND_PARTICLE_COUNT, (unsigned long)revision);
}

static void updateParticle(Particle* p, float dtSec, uint32_t dtMs) {
  if (!p) {
    return;
  }
  p->ageMs += dtMs;
  if (p->ageMs >= p->lifeMs || !pointAllowed(p->x[0], p->y[0])) {
    spawnParticle(p);
    return;
  }

  float east = 0.0f;
  float south = 0.0f;
  if (!windFieldSample(p->x[0], p->y[0], &east, &south)) {
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
  }

  for (int i = WIND_TRAIL_POINTS - 1; i > 0; --i) {
    p->x[i] = p->x[i - 1];
    p->y[i] = p->y[i - 1];
  }
  p->x[0] += vx * dtSec;
  p->y[0] += vy * dtSec;
  if (!pointAllowed(p->x[0], p->y[0])) {
    spawnParticle(p);
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

static void queueTrailPixel(int x, int y) {
  if (!pointAllowed((float)x, (float)y)) {
    return;
  }
  const uint16_t key = (uint16_t)(y * LCD_WIDTH + x);
  const int existing = findTrail(key);
  if (existing >= 0) {
    s_trails[existing].strength = 255;
    return;
  }
  const int pos = newKeyLowerBound(key);
  if (pos < s_newCount && s_newKeys[pos] == key) {
    return;
  }
  if (s_newCount < kNewPixelCapacity) {
    memmove(&s_newKeys[pos + 1], &s_newKeys[pos],
            (size_t)(s_newCount - pos) * sizeof(s_newKeys[0]));
    s_newKeys[pos] = key;
    ++s_newCount;
  }
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
    if (trail.strength != 0) {
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
  const int accepted = min(s_newCount, available);
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
      s_trails[out--] =
          TrailPixel{s_newKeys[fresh], s_newBase[fresh], 255, 0};
      --fresh;
    }
  }
  while (fresh >= 0) {
    s_trails[out--] = TrailPixel{s_newKeys[fresh], s_newBase[fresh], 255, 0};
    --fresh;
  }
  s_trailCount += accepted;
  return accepted;
}

static void markParticleHeads() {
  for (int i = 0; i < s_trailCount; ++i) {
    s_trails[i].flags = 0;
  }
  for (int i = 0; i < WIND_PARTICLE_COUNT; ++i) {
    const int x = (int)lroundf(s_particles[i].x[0]);
    const int y = (int)lroundf(s_particles[i].y[0]);
    if (!pointAllowed((float)x, (float)y)) {
      continue;
    }
    const int found = findTrail((uint16_t)(y * LCD_WIDTH + x));
    if (found >= 0) {
      s_trails[found].flags |= kTrailHead;
    }
  }
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

}  // namespace

void windParticlesBegin() { windParticlesReset(); }

void windParticlesReset() {
  memset(s_particles, 0, sizeof(s_particles));
  s_trailCount = 0;
  s_newCount = 0;
  s_seenZoom = -1;
  s_seenRevision = 0;
  s_lastFrameAt = 0;
  s_statsFrames = 0;
  s_statsSpans = 0;
}

void windParticlesNotifyBaseRedrawn() {
  // 调用方刚完成整屏 blit/push；无需逐点恢复，直接忘掉旧底色。
  s_trailCount = 0;
  s_newCount = 0;
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

  decayTrails(dtMs);
  s_newCount = 0;
  for (int i = 0; i < WIND_PARTICLE_COUNT; ++i) {
    updateParticle(&s_particles[i], dtSec, dtMs);
    queueNewestSegment(s_particles[i]);
  }

  const uint32_t drawStart = millis();
  // 先让本帧经过的旧像素复活，再恢复真正过期的像素，避免交叉轨迹互擦。
  const int restoreSpans = restoreAndRemoveExpired(lcd);
  sampleAndInsertNew(displayedZoom);
  markParticleHeads();
  const int drawSpans = drawTrailsAndHeads(lcd);

  ++s_statsFrames;
  s_statsSpans += (uint32_t)(restoreSpans + drawSpans);
  if (s_statsAt == 0) {
    s_statsAt = now;
  } else if ((now - s_statsAt) >= 5000UL) {
    const float fps = (float)s_statsFrames * 1000.0f / (float)(now - s_statsAt);
    const unsigned avgSpans =
        s_statsFrames > 0 ? (unsigned)(s_statsSpans / s_statsFrames) : 0;
    Serial.printf(
        "wind anim fps=%.1f draw=%lums busy=%d trails=%d new=%d spans=%u\n",
        fps, (unsigned long)(millis() - drawStart), (int)busy, s_trailCount,
        s_newCount, avgSpans);
    s_statsAt = now;
    s_statsFrames = 0;
    s_statsSpans = 0;
  }
  return true;
}
