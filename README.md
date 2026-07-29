# PaperDarkRoom

**A Dark Room**（Doublespeak Games，MPL-2.0）在 M5PaperS3 上的独立 e-ink 固件移植——官方简体中文文本首发。

非官方社区移植，与 Doublespeak Games 无隶属/背书关系。

## 当前状态

**Phase 1 已完成**（v0.2.0-adarkroom）：Room 火焰/builder/craftables 全链路、Outside 村庄人口与 10 职业分配、官方简体中文界面与文本、离线结算引擎（睡眠期间 income/火焰状态正确推进）、SD 卡存档。**不含** Path/World 探索、事件系统与 Ship/Space 结局。

Phase 2（Path + World 地图探索与战斗）、Phase 3（全事件池 + setpiece + Ship 结局）计划见 `docs/research.md` §6 分期计划。

## 硬件

- **M5PaperS3**（ESP32-S3-R8 + 4.7" ED047TC1 540×960 16 级灰度墨水屏）
- 板载 8 MB PSRAM、cap-touch、RTC（BM8563）、USB-C 原生 CDC

## 项目结构

```
platformio.ini   单一 env:adarkroom，src/ 即全部源码
partitions.csv   A/B + big-nvs 分区表（BLE OTA 的分区前提）
src/             固件源码
tools/           构建期生成器 + BLE OTA 刷机脚本
fonts/           CJK 字体源（fusion-pixel 12px，OFL-1.1）
docs/            调研文档 + 版权 NOTICE
```

## 构建

> 前提：已安装 PlatformIO CLI（`pip install platformio`）与 Python。

```
pio run -e adarkroom
```

### 打包 dist 镜像（M5Burner 上架用）

```
python tools/make_dist.py            # 已构建过可加 --skip-build 跳过构建
```

`tools/make_dist.py` 会先跑一遍构建（已构建过可加 `--skip-build` 跳过），再用 esptool `merge_bin` 把 bootloader、分区表、`boot_app0`、`firmware.bin` 按各自的 flash 偏移拼成一份完整镜像 `dist/adarkroom-<版本>-merged.bin`（版本号取自 `platformio.ini` 的 `-DCARD_VERSION`），并自动校验产物合法性。M5Burner/烧录时该文件整体写入 **0x0**，无需再分四份地址烧录。

## 刷机

**首刷 / 救砖走 USB：**

```
pio run -e adarkroom -t upload
```

**日常更新走 BLE OTA**（设备需已唤醒并完成过一次配对）：

```
python tools/ble_ota.py
```

`tools/ble_ota.py` 使用固件内置的 BLE GATT OTA 协议（8 字节头 `<II total|crc32>` + DATA 特征分块流 + STAT 特征通知）推送镜像；A/B 双分区布局（`partitions.csv`）下 OTA 只写非活动 slot，失败可回滚。

> **GATT 缓存陷阱**：新增/改动 BLE characteristic 后，Windows/WinRT 可能缓存旧的特征表，导致新特征"隐形"（`BleakCharacteristicNotFoundError`）。首次从 dashboard 固件互刷过来后，若 OTA 脚本报特征找不到，在 Windows 蓝牙设置里删除设备重新配对一次即可。不要在 tray/其它 BLE 中心设备仍连接着的情况下跑 `ble_ota.py`（一对多中心会冲突）。

### 通过 Launcher 安装（多固件启动管理器）

[bmorcelli/Launcher](https://github.com/bmorcelli/Launcher) 等第三方多固件启动管理器接管 bootloader 与分区表，用户从菜单里选一个 app-only `.bin` 由 Launcher 自己刷进它管理的 OTA 槽。这种形态下应使用 `python tools/make_dist.py` 额外产出的 `dist/adarkroom-<版本>-launcher.bin`，拷进 SD 卡后走 Launcher 自身的安装流程，**不要**整包刷 0x0（那是给 merged.bin 用的）。

注意事项：

1. Launcher 形态下固件内置的 BLE OTA 会被主动禁用——写入前会校验分区表指纹，识别到非本项目分区表（比如 Launcher 自己的表）就拒绝整个 OTA 会话，防止误把 Launcher 本体覆盖掉。这种形态下升级请通过 Launcher 重新安装新版 `launcher.bin`，不要指望游戏内 BLE OTA。
2. Launcher 分区表的 `nvs` 分区通常只有 20 KB 且与 PHY 校准数据共享（本项目自己的分区表把 `nvs` 挪到了 1 MB 就是为了绕开这个问题，见 `partitions.csv` 头部注释）。在 Launcher 下长期高频冷启动，`partitions.csv` 注释里描述的"nvs 写满被静默擦除、BLE 配对信息丢失"问题可能会回归——这是试玩形态的已知限制，非本项目代码的 bug。
3. 想恢复完整形态（大 nvs、自有分区表、BLE OTA 可用），用 `dist/adarkroom-<版本>-merged.bin` 整包刷回 0x0 即可。

## 生成器工具

游戏文本与 CJK 字体是从上游官方简体中文表构建期生成的，不手改生成产物：

```powershell
# 1) 官方中文翻译表 -> C 头（需要本地克隆一份上游 adarkroom 仓库，见下方致谢链接）
python tools\gen_adr_strings.py `
    --in path\to\adarkroom\lang\zh_cn\strings.js --out src\strings_zh.h

# 2) 从中文表提取字形闭包 -> 稀疏点阵 CJK 字体
python tools\gen_cjk_font.py `
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
