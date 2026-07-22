# GM / Debug 命令通道（`adr:`）

v0.3.5-adarkroom 引入。用于运行中通过 BLE 直接给游戏注入资源，供开发/测试时快速摆脱早期资源瓶颈，不必肉眼干磨 tick。

## 1. 是什么

复用已加密的 CTRL GATT 特征（BLE OTA/翻页协议共用的那个），不新增 characteristic：一条以 ASCII `"adr:"` 开头的写入被固件拦截为游戏命令，而不是二进制帧头。之所以这样设计，是为了规避 Windows/WinRT 的 **GATT 缓存陷阱**——新增/改动 characteristic 后 Windows 可能缓存旧的特征表，导致新特征"隐形"（`BleakCharacteristicNotFoundError`），需要删除设备重新配对才能恢复（见 `README.md` 「GATT 缓存陷阱」一节）。协议头的 `u32 LE total` 字段是小计数值，永远不可能等于 `0x3A726461`（`"adr:"` 的 LE 表示），所以这个复用是无冲突的，老 host 也不受影响。

## 2. 快速用法

前提：
- 设备已唤醒（电源键或 USB 供电）；
- 已与本机完成过配对（BLE bond 存在）；
- 没有其他 BLE central 正占用设备（tray、`ble_ota.py` 等一对一连接，两个 central 会冲突）。

```powershell
& "$env:USERPROFILE\.ai-desk-card\venv\Scripts\python.exe" tools\adr_cmd.py give iron 300
# 一次给所有 19 种资源各注入 100 万：
& "$env:USERPROFILE\.ai-desk-card\venv\Scripts\python.exe" tools\adr_cmd.py give-all 1000000
```

`give` 会扫描设备名、连接、写入 `adr:give iron 300` 到 CTRL 特征后立即退出（fire-and-forget，见下）。`give-all` 在一次连接里遍历全部 `RES_KEY` 逐条写入，每条之间停约 0.3s 让固件 save+刷屏跟上。

## 3. 协议细节

- **线上格式**：`adr:give <res> <amount>`，纯 ASCII 文本，无二进制头。
- **数量范围**：`1..1000000`，整数，whole 单位（人类可读的“个”，不是内部定点数）。上限 100 万下 `1e6 × FP(100) = 1e8`，远在 int32 `stores`（约 2.1e9）余量内，单条注入不会溢出。
- **单位换算**：固件内部按定点数存储，实际写入 `stores[r] += amount * FP`（`FP = 100`，见 `src/game_data.h`）。
- **资源键全表**（`RES_KEY`，`src/game_data.h`）：

  ```
  wood, fur, meat, bait, leather, cured meat,
  iron, coal, sulphur, steel, teeth, scales, cloth, charm,
  bullets, medicine, energy cell, alien alloy, compass
  ```

  多词键（`cured meat` / `energy cell` / `alien alloy`）用**下划线代替空格**发送（`cured_meat` 等），使其保持单 token 不被 `sscanf` 的 `%s` 截断；固件解析出 `<res>` 后把 `'_'` 换回 `' '` 再与 `RES_KEY` 比对，因此全部 19 种资源都可注入。`tools/adr_cmd.py` 的 `give <res>` 参数里若写了空格也会自动转成下划线。
- **反馈方式**（无 BLE ack，纯 fire-and-forget）：
  - 成功：高音 beep（1800Hz/80ms）+ 立即重绘当前页 + `g_game.save()` 落盘；
  - 失败（解析失败 / 数量越界 / 资源键未知）：低音 beep（600Hz/120ms）+ 仅 Serial log，无视觉反馈。
  - host 端不等待任何响应确认，`tools/adr_cmd.py` 写入后直接打印一行提示并退出。

## 4. 固件端实现索引

| 位置 | 职责 |
|---|---|
| `src/ble_link.cpp` `CtrlCb::onWrite`（"adr:" 前缀拦截） | **capture**：识别前缀、把整行拷进 `rx.gameCmd`、置位 `rx.gameCmdPending` |
| `src/ble_link.h` `Rx` 结构体 `gameCmd`/`gameCmdPending` 字段 | 两者之间的交接槽位（BLE 回调线程写，主循环读一次即清） |
| `src/main.cpp` `applyPendingGameCmd()` | **act**：解析、clamp、resolve 资源键、写 `stores`、save、beep、重绘 |

与 `TIME_CONFIG` 的 `applyPendingTimeConfig()` 同一套分工：BLE 回调只做搬运，不做任何有副作用的游戏状态改动；真正“执行”永远发生在主循环里。

## 5. 扩展指南

要加一个新动词（比如 `set` / `clear`），需要动以下三处：

1. `src/main.cpp` `applyPendingGameCmd()`：加一条 `sscanf` 分支解析新动词的参数、执行对应的游戏状态改动；
2. 如果新动词的参数格式与 `give <res> <amount>` 差异较大，顺带更新 `src/ble_link.h` 头部注释里 `"adr:…"` 那段协议说明；
3. `tools/adr_cmd.py`：给 `verb` 的 `choices` 加上新值，并按需调整参数解析/校验（如新增 flag）。

CTRL 拦截逻辑（`ble_link.cpp` 的前缀识别）和 `Rx.gameCmd` 交接槽位通常不需要改——它们只搬运整行文本，新动词的语义只在 `applyPendingGameCmd()` 里解析。

## 6. 注意事项

- 这是**开发/测试用的 GM 工具**，正常游玩时不要使用——会破坏经济系统的平衡性和真实感。
- 命令执行后立即 `save()` 落盘，**无法撤销**；发错资源/数量只能再发一条命令手动纠正（或干等它在正常玩法里被消耗掉）。
