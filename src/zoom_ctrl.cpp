#include "zoom_ctrl.h"

#include "button.h"
#include "config.h"
#include "frame_cache.h"

static int s_zoom = MAP_ZOOM;
static int s_displayedZoom = -1;
static int s_prefetchQ[ZOOM_MAX - ZOOM_MIN + 1];
static int s_prefetchLen = 0;
static uint32_t s_prefetchCoolUntil[ZOOM_MAX - ZOOM_MIN + 1];
static volatile bool s_composeAbort = false;
static int s_pendingZoom = -1;
static ZoomPendingFeedbackFn s_pendingFeedback = nullptr;

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
  if (!zoomCanCompose(zoom) || frameCacheHas(zoom) || zoomPrefetchCooling(zoom)) {
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
  const int maxDelta = ZOOM_MAX - ZOOM_MIN;
  for (int delta = 1; delta <= maxDelta; ++delta) {
    enqueueUnique(centerZoom - delta);
    enqueueUnique(centerZoom + delta);
  }
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
  if (frameCacheHas(*outZoom) || zoomPrefetchCooling(*outZoom)) {
    return zoomPrefetchPop(outZoom);
  }
  return true;
}

void composeRequestAbort() { s_composeAbort = true; }
void composeClearAbort() { s_composeAbort = false; }
bool composeAbortRequested() { return s_composeAbort; }

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

void inputServiceDuringBlock() {
  const ButtonEvent ev = buttonPoll();
  if (ev != ButtonEvent::ShortPress) {
    return;
  }
  // 以屏上所见档为基准 +1，避免造片失败后逻辑档超前造成跳档
  if (s_displayedZoom >= ZOOM_MIN && s_displayedZoom <= ZOOM_MAX &&
      s_displayedZoom != s_zoom) {
    Serial.printf("input resync logic=z%d display=z%d\n", s_zoom,
                  s_displayedZoom);
    s_zoom = s_displayedZoom;
  }
  const int next = zoomCycleNext();
  zoomSetPending(next);
  composeRequestAbort();
  Serial.printf("input: short press -> pending z%d (abort)\n", next);
  if (s_pendingFeedback) {
    s_pendingFeedback(next);
  }
}
