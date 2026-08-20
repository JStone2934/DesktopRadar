#pragma once

#include <stdint.h>

/** 当前显示 zoom。 */
int zoomCurrent();

void zoomSetCurrent(int zoom);

/** 默认/回到的缩放档：启动使用，运行中按住 S 键 2 秒跳回。 */
void zoomSetDefault(int zoom);
int zoomDefault();

/** 记录屏上已显示的档（秒切/上屏后调用），供造片中短按对齐基准。 */
void zoomNoteDisplayed(int zoom);
int zoomDisplayed();

/** 循环 +1（ZOOM_MIN..ZOOM_MAX，跳过 ZOOM_SKIP）。 */
int zoomCycleNext();

/** 是否可造片（ZOOM_MIN..ZOOM_MAX，跳过 ZOOM_SKIP；z>7 雷达上采样）。 */
bool zoomCanCompose(int zoom);

/** 清空并排队可用档：当前档优先，其余 stale 档按雷达时刻从旧到新。 */
void zoomPrefetchResetAround(int centerZoom);

/** 限制 ResetAround 的邻档半径。全档秒切模式使用最大半径。 */
void zoomSetPrefetchRadius(int radius);

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

/** 注册阻塞 HTTP/解码循环中的 UI 泵；用于保持风场动画。 */
typedef void (*BlockingUiServiceFn)();
void inputSetBlockingUiService(BlockingUiServiceFn fn);

/**
 * 将当前 FreeRTOS 任务登记为唯一 UI/按键消费任务。
 * 后台造片任务仍可检查 composeAbortRequested()，但不会消费按键事件或碰 LCD。
 */
void inputBindUiTaskToCurrent();

/** 初始化全档缓存期间锁住缩放输入，并禁止物理按键中止后台造片。 */
void inputSetLocked(bool locked);
