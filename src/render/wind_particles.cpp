#include "wind_particles.h"

#include <Arduino.h>
#include <math.h>
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

static Particle s_particles[WIND_PARTICLE_COUNT];
static uint32_t s_rng = 0x83a4f19dUL;
static uint32_t s_lastFrameAt = 0;
static uint32_t s_seenRevision = 0;
static int s_seenZoom = -1;
static uint32_t s_statsAt = 0;
static uint32_t s_statsFrames = 0;

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
  // 初始年龄打散，避免所有粒子同时重生。
  p->lifeMs = 3500UL + (nextRand() % 2501UL);
  p->ageMs = nextRand() % p->lifeMs;
}

static void resetAll(int zoom, uint32_t revision) {
  s_rng = 0x83a4f19dUL ^ ((uint32_t)(zoom + 31) * 2654435761UL) ^
          windFieldModelTime();
  for (int i = 0; i < WIND_PARTICLE_COUNT; ++i) {
    spawnParticle(&s_particles[i]);
  }
  s_seenZoom = zoom;
  s_seenRevision = revision;
  s_lastFrameAt = 0;
  s_statsAt = millis();
  s_statsFrames = 0;
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

}  // namespace

void windParticlesBegin() { windParticlesReset(); }

void windParticlesReset() {
  memset(s_particles, 0, sizeof(s_particles));
  s_seenZoom = -1;
  s_seenRevision = 0;
  s_lastFrameAt = 0;
  s_statsFrames = 0;
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
  if (s_seenZoom != displayedZoom || s_seenRevision != revision) {
    resetAll(displayedZoom, revision);
  }

  uint32_t dtMs = s_lastFrameAt == 0 ? interval : (now - s_lastFrameAt);
  if (dtMs > 350UL) {
    dtMs = 350UL;
  }
  s_lastFrameAt = now;
  const float dtSec = (float)dtMs * 0.001f;

  for (int i = 0; i < WIND_PARTICLE_COUNT; ++i) {
    updateParticle(&s_particles[i], dtSec, dtMs);
  }

  const uint32_t drawStart = millis();
  if (!frameCacheBlit(lcd, displayedZoom)) {
    return false;
  }

  const uint16_t trailColors[WIND_TRAIL_POINTS - 1] = {
      lcd->color565(38, 112, 142), lcd->color565(61, 161, 194),
      lcd->color565(151, 233, 244)};
  const uint16_t headColor = lcd->color565(220, 252, 255);

  lcd->startWrite();
  for (int i = 0; i < WIND_PARTICLE_COUNT; ++i) {
    const Particle& p = s_particles[i];
    for (int j = WIND_TRAIL_POINTS - 1; j > 0; --j) {
      if (!pointAllowed(p.x[j], p.y[j]) ||
          !pointAllowed(p.x[j - 1], p.y[j - 1])) {
        continue;
      }
      const int colorIndex = WIND_TRAIL_POINTS - 1 - j;
      lcd->drawLine((int)lroundf(p.x[j]), (int)lroundf(p.y[j]),
                    (int)lroundf(p.x[j - 1]), (int)lroundf(p.y[j - 1]),
                    trailColors[colorIndex]);
    }
    if (pointAllowed(p.x[0], p.y[0])) {
      lcd->drawPixel((int)lroundf(p.x[0]), (int)lroundf(p.y[0]), headColor);
    }
  }
  lcd->endWrite();

  ++s_statsFrames;
  if (s_statsAt == 0) {
    s_statsAt = now;
  } else if ((now - s_statsAt) >= 5000UL) {
    const float fps = (float)s_statsFrames * 1000.0f / (float)(now - s_statsAt);
    Serial.printf("wind anim fps=%.1f draw=%lums busy=%d\n", fps,
                  (unsigned long)(millis() - drawStart), (int)busy);
    s_statsAt = now;
    s_statsFrames = 0;
  }
  return true;
}
