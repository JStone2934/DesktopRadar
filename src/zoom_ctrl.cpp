#include "zoom_ctrl.h"

#include "button.h"
#include "config.h"
#include "frame_cache.h"

static int s_zoom = MAP_ZOOM;
static int s_defaultZoom = MAP_ZOOM;
static int s_displayedZoom = -1;
static int s_prefetchQ[ZOOM_MAX - ZOOM_MIN + 1];
static int s_prefetchLen = 0;
static uint32_t s_prefetchCoolUntil[ZOOM_MAX - ZOOM_MIN + 1];
static volatile bool s_composeAbort = false;
static int s_pendingZoom = -1;
static ZoomPendingFeedbackFn s_pendingFeedback = nullptr;
static BlockingUiServiceFn s_blockingUiService = nullptr;
static int s_prefetchRadius = ZOOM_MAX - ZOOM_MIN;

static constexpr uint32_t kPrefetchFailCoolMs = 45000UL;

static inline int coolSlot(int zoom) { return zoom - ZOOM_MIN; }

static bool zoomPrefetchCooling(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return true;
  }
  return (int32_t)(millis() - s_prefetchCoolUntil[coolSlot(zoom)]) < 0;
}

void zoomPrefetchNoteFail(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return;
  }
  s_prefetchCoolUntil[coolSlot(zoom)] = millis() + kPrefetchFailCoolMs;
  Serial.printf("prefetch cool z%d for %lus\n", zoom,
                (unsigned long)(kPrefetchFailCoolMs / 1000UL));
}

void zoomPrefetchNoteOk(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return;
  }
  s_prefetchCoolUntil[coolSlot(zoom)] = 0;
}

int zoomCurrent() { return s_zoom; }

void zoomSetDefault(int zoom) {
  if (!zoomCanCompose(zoom)) {
    zoom = MAP_ZOOM;
  }
  s_defaultZoom = zoom;
}

int zoomDefault() { return s_defaultZoom; }

void zoomSetCurrent(int zoom) {
  if (zoom < ZOOM_MIN) {
    zoom = ZOOM_MIN;
  }
  if (zoom > ZOOM_MAX) {
    zoom = ZOOM_MAX;
  }
  s_zoom = zoom;
}

void zoomNoteDisplayed(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    s_displayedZoom = -1;
    return;
  }
  s_displayedZoom = zoom;
}

int zoomDisplayed() { return s_displayedZoom; }

int zoomCycleNext() {
  int z = s_zoom;
  for (int i = 0; i < (ZOOM_MAX - ZOOM_MIN + 1); ++i) {
    ++z;
    if (z > ZOOM_MAX) {
      z = ZOOM_MIN;
    }
    if (zoomCanCompose(z)) {
      s_zoom = z;
      return s_zoom;
    }
  }
  return s_zoom;
}

bool zoomCanCompose(int zoom) {
  if (zoom < ZOOM_MIN || zoom > ZOOM_MAX) {
    return false;
  }
  if (zoom == ZOOM_SKIP) {
    return false;
  }
  return true;
}

void zoomPrefetchClear() { s_prefetchLen = 0; }

static void enqueueUnique(int zoom) {
  if (!zoomCanCompose(zoom) || frameCacheIsFresh(zoom) || zoomPrefetchCooling(zoom)) {
    return;
  }
  for (int i = 0; i < s_prefetchLen; ++i) {
    if (s_prefetchQ[i] == zoom) {
      return;
    }
  }
  if (s_prefetchLen >= (int)(sizeof(s_prefetchQ) / sizeof(s_prefetchQ[0]))) {
    return;
  }
  s_prefetchQ[s_prefetchLen++] = zoom;
}

void zoomPrefetchResetAround(int centerZoom) {
  s_prefetchLen = 0;
  enqueueUnique(centerZoom);
  const int maxDelta = s_prefetchRadius;
  for (int delta = 1; delta <= maxDelta; ++delta) {
    enqueueUnique(centerZoom - delta);
    enqueueUnique(centerZoom + delta);
  }
}

void zoomSetPrefetchRadius(int radius) {
  const int maxRadius = ZOOM_MAX - ZOOM_MIN;
  if (radius < 0) {
    radius = 0;
  } else if (radius > maxRadius) {
    radius = maxRadius;
  }
  s_prefetchRadius = radius;
  zoomPrefetchClear();
}

bool zoomPrefetchPop(int* outZoom) {
  if (!outZoom || s_prefetchLen <= 0) {
    return false;
  }
  *outZoom = s_prefetchQ[0];
  for (int i = 1; i < s_prefetchLen; ++i) {
    s_prefetchQ[i - 1] = s_prefetchQ[i];
  }
  --s_prefetchLen;
  if (frameCacheIsFresh(*outZoom) || zoomPrefetchCooling(*outZoom)) {
    return zoomPrefetchPop(outZoom);
  }
  return true;
}

void composeRequestAbort() { s_composeAbort = true; }
void composeClearAbort() { s_composeAbort = false; }
bool composeAbortRequested() {
  // 合成路径直接观察物理按下电平，不必等到松手形成 ShortPress。
  if (buttonIsDown()) {
    s_composeAbort = true;
  }
  return s_composeAbort;
}

void zoomSetPending(int zoom) { s_pendingZoom = zoom; }

int zoomTakePending() {
  const int z = s_pendingZoom;
  s_pendingZoom = -1;
  return z;
}

bool zoomHasPending() { return s_pendingZoom >= 0; }

bool zoomClearPendingIf(int zoom) {
  if (s_pendingZoom != zoom) {
    return false;
  }
  s_pendingZoom = -1;
  return true;
}

void zoomSetPendingFeedback(ZoomPendingFeedbackFn fn) {
  s_pendingFeedback = fn;
}

void inputSetBlockingUiService(BlockingUiServiceFn fn) {
  s_blockingUiService = fn;
}

void inputServiceDuringBlock() {
  if (s_blockingUiService) {
    s_blockingUiService();
  }
  const ButtonEvent ev = buttonPoll();
  if (ev != ButtonEvent::ShortPress && ev != ButtonEvent::LongPress) {
    return;
  }

  int next = s_defaultZoom;
  if (ev == ButtonEvent::ShortPress) {
    // 以屏上所见档为基准 +1，避免造片失败后逻辑档超前造成跳档
    if (s_displayedZoom >= ZOOM_MIN && s_displayedZoom <= ZOOM_MAX &&
        s_displayedZoom != s_zoom) {
      Serial.printf("input resync logic=z%d display=z%d\n", s_zoom,
                    s_displayedZoom);
      s_zoom = s_displayedZoom;
    }
    next = zoomCycleNext();
  } else {
    zoomSetCurrent(next);
  }

  zoomSetPending(next);
  composeRequestAbort();
  Serial.printf("input: %s -> pending z%d (abort)\n",
                ev == ButtonEvent::ShortPress ? "short press" : "long press",
                next);
  if (s_pendingFeedback) {
    s_pendingFeedback(next);
  }
}
