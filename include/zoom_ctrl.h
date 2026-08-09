#pragma once

#include <stdint.h>

/** 当前显示 zoom。 */
int zoomCurrent();

void zoomSetCurrent(int zoom);

/** 记录屏上已显示的档（秒切/上屏后调用），供造片中短按对齐基准。 */
void zoomNoteDisplayed(int zoom);
int zoomDisplayed();

/** 循环 +1（ZOOM_MIN..ZOOM_MAX，跳过 ZOOM_SKIP）。 */
int zoomCycleNext();

/** 是否可造片（ZOOM_MIN..ZOOM_MAX，跳过 ZOOM_SKIP；z>7 雷达上采样）。 */
bool zoomCanCompose(int zoom);

/** 清空并按距离排队入队可用档（跳过 ZOOM_SKIP）：当前档优先，再 ±1、±2… */
void zoomPrefetchResetAround(int centerZoom);

/** 取出下一个预取目标；无则返回 false。 */
bool zoomPrefetchPop(int* outZoom);

/** 预取造片失败：该档冷却一段时间，避免死循环占满队列。 */
void zoomPrefetchNoteFail(int zoom);
void zoomPrefetchNoteOk(int zoom);

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
/** 若 pending 等于 zoom 则清除（反馈里已秒切成功时避免再切一次）。 */
bool zoomClearPendingIf(int zoom);

/**
 * 阻塞操作期间短按切档时的即时回调（可在造片中 blit 已缓存档）。
 * 传入 nullptr 清除。
 */
typedef void (*ZoomPendingFeedbackFn)(int zoom);
void zoomSetPendingFeedback(ZoomPendingFeedbackFn fn);

/**
 * 阻塞操作期间调用：轮询 S 键短按 → 记 pending 并请求 abort。
 * HTTP / 造片长循环里应频繁调用。
 */
void inputServiceDuringBlock();
