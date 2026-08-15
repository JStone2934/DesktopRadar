# GitHub 固件更新与 recovery 布局

从 `v0.2.0` 开始，4 MB Flash 使用单主程序槽、LittleFS 暂存包和独立
factory 安装器。`v0.1.x` 的分区布局不能在线迁移，必须先用 USB 安装
`v0.2.0` 基础版本；之后才可从 Web 页面安装签名的 `v0.2.1+`。

## 设备行为

- 主程序在 WiFi、SNTP、十档缓存都就绪并连续空闲 60 秒后检查一次
  `latest.json`；NVS 中先记录尝试时间，同一天失败不会反复请求。
- 日检只保存经过 ECDSA P-256 验证的 manifest，不下载固件、不清缓存。
- Reset 后 Web 配置页会显示当前版本、候选版本、检查时间与错误原因。
- 只有用户勾选提示并提交独立的 `POST /update/confirm` 才创建安装请求。
- 热点关闭后再次联网、校时、重新验证 manifest，并用 Range GET 检查资源。
  候选版本、大小或 SHA 有变化时取消，不清缓存，要求重新确认。
- 预检成功后才格式化 LittleFS，最多从零下载三次；完整文件经过大小、镜像
  头和 SHA-256 校验后原子改名为 `/update/firmware.bin`。
- factory 不联网。它再次校验 manifest 签名和暂存文件 SHA，擦写 app0，
  从 Flash 回读校验，然后把 app0 标为首次待确认启动。
- 新主程序在配置门户前完成分区、版本、NVS、LittleFS、LCD、按键、后台任务
  和堆空间自检。成功后先调用 IDF OTA 确认，再清理暂存包。

HTTPS 下载使用精简模式，不在主固件中携带完整 CA 根证书包；安装真实性由
固化公钥验证的 manifest 保证，固件完整性由签名 payload 内的大小和 SHA-256
保证。此取舍节省约 69 KiB，主固件仍符合 1,650,000 字节发布上限。

## 分区

| 名称 | 偏移 | 大小 | 用途 |
|---|---:|---:|---|
| nvs | `0x9000` | `0x5000` | 配置、manifest、双槽 journal |
| otadata | `0xe000` | `0x2000` | factory/app0 启动状态 |
| factory | `0x10000` | `0x80000` | recovery-1 安装器 |
| app0 | `0x90000` | `0x1b0000` | 雷达主程序 |
| spiffs | `0x240000` | `0x1b0000` | LittleFS 缓存和暂存包 |
| coredump | `0x3f0000` | `0x10000` | 崩溃转储 |

构建会自动拒绝大于 1,650,000 字节的主程序或大于 500 KiB 的 factory。

## 构建与签名

签名私钥不能放入仓库。设置路径后生成完整发布目录：

```bash
export RADAR_SIGNING_KEY=/secure/path/radar-prod-1.pem
python3 tools/package_release.py \
  --version 0.2.1 --version-code 2001 \
  --notes '修复说明'
```

输出目录 `dist/v0.2.1/` 包含：

- `DesktopRadar-v0.2.1-esp32c3.bin`：OTA 主程序资源；
- `latest.json`：小于 4 KiB 的 schema-2 签名 manifest；
- `factory-recovery-1.bin`：独立安装器；
- `DesktopRadar-v0.2.1-usb-full.bin`：会清除全部数据的全量恢复镜像；
- `SHA256SUMS`：发布资源校验值。

Release 中主程序资源名和 tag 必须固定；上传后再把同一份 `latest.json` 作为
资源发布。设备不调用 GitHub API。

## 首次 USB 迁移

先构建两个工程及 `v0.2.0` 主程序，然后使用安装脚本。它先备份 20 KiB NVS，
只擦除 otadata、factory、app0、新 LittleFS 和 coredump，保留 WiFi/PEAP 与地图
设置：

```bash
python3 tools/install_base.py \
  --port /dev/cu.usbmodem101 \
  --app dist/v0.2.0/DesktopRadar-v0.2.0-esp32c3.bin \
  --factory dist/v0.2.0/factory-recovery-1.bin
```

全量恢复镜像只用于需要清空所有数据的情况，应先 `erase-flash`，再从 `0x0`
写入；它不会保留 NVS。

## 恢复规则

- 下载中断：主程序仍在；下次启动显示 5 秒取消窗口，再从零下载。
- factory 写 app0 中断：factory 与暂存包仍在；从 app0 开头重新安装。
- 连续三次安装失败：停止自动擦写；短按 S 明确重试，或进入 USB 恢复。
- 首次启动崩溃/自检失败：bootloader 回到 factory，同一包不会自动重装。
- 任一 manifest/journal 双槽有一个 CRC 有效时使用 sequence 更新者；两个均坏时
  factory 不擦 app0。
- 4 MB 版本不保留旧雷达固件，因此坏包无法自动恢复旧版本；真正 A/B 回退留给
  未来 8 MB 硬件。
