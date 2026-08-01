#include "zoom_ctrl.h"

#include "config.h"
#include "frame_cache.h"

static int s_zoom = MAP_ZOOM;
// 可造片档最多 RAINVIEWER_MAX_ZOOM - ZOOM_MIN + 1（当前为 5）
static int s_prefetchQ[8];
static int s_prefetchLen = 0;
static volatile bool s_composeAbort = false;
static int s_pendingZoom = -1;

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

int zoomCycleNext() {
  int z = s_zoom + 1;
  if (z > ZOOM_MAX) {
    z = ZOOM_MIN;
  }
  s_zoom = z;
  return s_zoom;
}

bool zoomCanCompose(int zoom) {
  return zoom >= ZOOM_MIN && zoom <= RAINVIEWER_MAX_ZOOM;
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
  // 与 DesktopRadar zoom_priority_order 一致：center, ±1, ±2…
  enqueueUnique(centerZoom);
  const int maxDelta = RAINVIEWER_MAX_ZOOM - ZOOM_MIN;
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
