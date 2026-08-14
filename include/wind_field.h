#pragma once

#include <stdint.h>

/** 初始化 LittleFS 风场小缓存目录。LittleFS 必须已经挂载。 */
bool windFieldBegin();

/** Web/NVS 总开关。关闭时保留缓存但停止请求与采样。 */
void windFieldSetEnabled(bool enabled);
bool windFieldEnabled();

/** 选择当前地图视口；变更后优先装入该 zoom 的磁盘缓存。 */
void windFieldSelect(float lat, float lon, int zoom);

/**
 * 到期时同步请求一次 Open-Meteo。应在主循环调用；HTTPS 阻塞循环会继续
 * 调用 inputServiceDuringBlock，因此已有风场动画仍会推进。
 * @return 本次是否实际发起过请求。
 */
bool windFieldService();

/** 首次无缓存时短暂阻止雷达预取，让风场先完成首拉。 */
bool windFieldBlocksPrefetch();

bool windFieldReadyFor(int zoom);

/** 按屏幕坐标双线性采样，输出向东/向南的 m/s 分量。 */
bool windFieldSample(float screenX, float screenY, float* east,
                     float* south);

/** 每次成功装入/更新网格递增，粒子层据此重置轨迹。 */
uint32_t windFieldRevision();
uint32_t windFieldModelTime();
