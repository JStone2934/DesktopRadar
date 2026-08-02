#include "zoom_ctrl.h"

#include "button.h"
#include "config.h"
#include "frame_cache.h"

static int s_zoom = MAP_ZOOM;
static int s_prefetchQ[ZOOM_MAX - ZOOM_MIN + 1];
static int s_prefetchLen = 0;
static volatile bool s_composeAbort = false;
static int s_pendingZoom = -1;
static ZoomPendingFeedbackFn s_pendingFeedback = nullptr;

int zoomCurrent() { return s_zoom; }

void zoomSetCurrent(int zoom) {
  if (zoom < ZOOM_MIN) {
    zoom = ZOOM_MIN;
  }
  if (zoom > ZOOM_MAX) {
    zoom = ZOOM_MAX;
  }
  if (zoom == ZOOM_SKIP) {
    zoom = ZOOM_SKIP - 1;  // z11 → z10
  }
  s_zoom = zoom;
}

int zoomCycleNext() {
  int z = s_zoom + 1;
  if (z == ZOOM_SKIP) {
    z = ZOOM_SKIP + 1;
  }
  if (z > ZOOM_MAX) {
    z = ZOOM_MIN;
  }
  s_zoom = z;
  return s_zoom;
}

bool zoomCanCompose(int zoom) {
  return zoom >= ZOOM_MIN && zoom <= ZOOM_MAX && zoom != ZOOM_SKIP;
}

void zoomPrefetchClear() { s_prefetchLen = 0; }

static void enqueueUnique(int zoom) {
  if (!zoomCanCompose(zoom) || frameCacheHas(zoom)) {
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
  if (frameCacheHas(*outZoom)) {
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

void zoomSetPendingFeedback(ZoomPendingFeedbackFn fn) {
  s_pendingFeedback = fn;
}

void inputServiceDuringBlock() {
  const ButtonEvent ev = buttonPoll();
  if (ev != ButtonEvent::ShortPress) {
    return;
  }
  const int next = zoomCycleNext();
  zoomSetPending(next);
  composeRequestAbort();
  Serial.printf("input: short press -> pending z%d (abort)\n", next);
  if (s_pendingFeedback) {
    s_pendingFeedback(next);
  }
}
