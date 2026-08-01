#pragma once

#include <stdint.h>

/** 当前显示 zoom。 */
int zoomCurrent();

void zoomSetCurrent(int zoom);

/** 循环 +1（ZOOM_MIN..ZOOM_MAX）。 */
int zoomCycleNext();

/** 是否可造片（z3–12；z>7 雷达上采样）。 */
bool zoomCanCompose(int zoom);

/** 清空并按距离排队入队全部档（z3–12）：当前档优先，再 ±1、±2… */
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

/**
 * 阻塞操作期间调用：轮询 BOOT 短按 → 记 pending 并请求 abort。
 * HTTP / 造片长循环里应频繁调用。
 */
void inputServiceDuringBlock();
