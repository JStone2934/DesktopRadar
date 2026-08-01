#pragma once

#include <stdint.h>

/** 当前显示 zoom。 */
int zoomCurrent();

void zoomSetCurrent(int zoom);

/** 循环 +1（ZOOM_MIN..ZOOM_MAX）。 */
int zoomCycleNext();

/** 是否可在本轮造片（z <= RAINVIEWER_MAX_ZOOM）。 */
bool zoomCanCompose(int zoom);

/** 清空并按距离排队入队全部可造片档（z3–RAINVIEWER_MAX_ZOOM）：当前档优先，再 ±1、±2… */
void zoomPrefetchResetAround(int centerZoom);

/** 取出下一个预取目标；无则返回 false。 */
bool zoomPrefetchPop(int* outZoom);

/** 用户切档时清空预取队列。 */
void zoomPrefetchClear();

/** 请求中止正在进行的合成（预取可被打断）。 */
void composeRequestAbort();
void composeClearAbort();
bool composeAbortRequested();

/** 合成中短按产生的待切换 zoom；-1 表示无。 */
void zoomSetPending(int zoom);
int zoomTakePending();
bool zoomHasPending();
