# PaperDarkRoom

**A Dark Room**（Doublespeak Games，MPL-2.0）在 M5PaperS3 上的独立 e-ink 固件移植——官方简体中文文本首发。项目从 [dashboard-fw](https://github.com/) 的 `adarkroom` 分支迁出为独立仓库；固件本身与 dashboard-fw dashboard 固件共用同一块板级/分区/BLE 协议，可通过 BLE OTA 互相刷入同一台设备。

非官方社区移植，与 Doublespeak Games 无隶属/背书关系。

## 硬件

- **M5PaperS3**（ESP32-S3-R8 + 4.7" ED047TC1 540×960 16 级灰度墨水屏）
- 板载 8 MB PSRAM、cap-touch、RTC（BM8563）、USB-C 原生 CDC

## 项目结构

```
platformio.ini   单一 env:adarkroom，src/ 即全部源码
partitions.csv   A/B + big-nvs 分区表（与 dashboard-fw dashboard 固件同布局，OTA 互刷的前提）
src/             固件源码
tools/           构建期生成器 + BLE OTA 刷机脚本
fonts/           CJK 字体源（fusion-pixel 12px，OFL-1.1）
docs/            调研文档 + 版权 NOTICE
```

## 构建

```powershell
& "$env:USERPROFILE\.ai-desk-card\venv\Scripts\python.exe" -m platformio run -e adarkroom
```

## 刷机

**首刷 / 救砖走 USB：**

```powershell
& "$env:USERPROFILE\.ai-desk-card\venv\Scripts\python.exe" -m platformio run -e adarkroom -t upload
```

**日常更新走 BLE OTA**（设备需已唤醒并完成过一次配对）：

```powershell
& "$env:USERPROFILE\.ai-desk-card\venv\Scripts\python.exe" tools\ble_ota.py
```

`tools/ble_ota.py` 复用与 dashboard-fw dashboard 固件（`dashboard-env` env）完全相同的 BLE GATT UUID 集合与 OTA 协议（8 字节头 `<II total|crc32>` + DATA 特征分块流 + STAT 特征通知），因此同一台设备上两个固件**可反复互刷**：本仓库的 `adarkroom.bin` 可以刷给一台正跑 dashboard-fw dashboard 固件的设备，反之亦然，物理前提是双方共用同一份 A/B 分区表（`partitions.csv`）。

> **GATT 缓存陷阱**：新增/改动 BLE characteristic 后，Windows/WinRT 可能缓存旧的特征表，导致新特征"隐形"（`BleakCharacteristicNotFoundError`）。首次从 dashboard 固件互刷过来后，若 OTA 脚本报特征找不到，在 Windows 蓝牙设置里删除设备重新配对一次即可。不要在 tray/其它 BLE 中心设备仍连接着的情况下跑 `ble_ota.py`（一对多中心会冲突）。

## 生成器工具

游戏文本与 CJK 字体是从上游官方简体中文表构建期生成的，不手改生成产物：

```powershell
# 1) 官方中文翻译表 -> C 头（需要本地克隆一份上游 adarkroom 仓库，见下方致谢链接）
& "$env:USERPROFILE\.ai-desk-card\venv\Scripts\python.exe" tools\gen_adr_strings.py `
    --in path\to\adarkroom\lang\zh_cn\strings.js --out src\strings_zh.h

# 2) 从中文表提取字形闭包 -> 稀疏点阵 CJK 字体
& "$env:USERPROFILE\.ai-desk-card\venv\Scripts\python.exe" tools\gen_cjk_font.py `
    --ttf fonts\fusion-pixel-12px-proportional-zh_hans.ttf `
    --strings src\strings_zh.h --out src\cjk_font12.h
```

详见 `docs/research.md`（移植调研与架构设计全文）。

## 许可

- **本项目整体采用 Mozilla Public License 2.0（MPL-2.0）**，见 `LICENSE`。移植自上游 A Dark Room 的数据表/文本/逻辑本就是 MPL-2.0 源文件的衍生作品，相关文件头部保留 MPL-2.0 声明（Exhibit A）。
- **CJK 字体** `fonts/fusion-pixel-12px-proportional-zh_hans.ttf` 采用 **SIL Open Font License 1.1（OFL-1.1）**，见 `fonts/fusion_pixel_OFL.txt`。
- 完整署名与合规说明见 `docs/NOTICE`。

## 上游致谢

- 游戏本体与官方简体中文翻译：[doublespeakgames/adarkroom](https://github.com/doublespeakgames/adarkroom)（Doublespeak Games，MPL-2.0）
- CJK 字体：[fusion-pixel-font](https://github.com/TakWolf/fusion-pixel-font)（TakWolf，OFL-1.1）
