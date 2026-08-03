# Phase 3 上游机制规格书（Ship + Space + Executioner/Fabricator）

> 面向"没读过上游源码的实现者"的移植规格。数值一律以上游 master/main 源码为准，本文所有数字均逐行核对过源码，不依赖记忆。
> 上游仓库：`doublespeakgames/adarkroom`（默认分支 `main`，`master` 为其别名）。
> 许可：上游为 MPL-2.0；本仓库沿用同源，文案 glyph 走既定闭包流程（`tools/gen_cjk_font.py`）。
>
> 本文抓取的上游文件（commit 时点 2026-08）：
> - `script/ship.js`（177 行）— 星舰页：hull/thrusters 升级与起飞
> - `script/space.js`（631 行）— 太空躲陨石小游戏 + 通关结局
> - `script/fabricator.js`（244 行）— 制造机（蓝图系统）
> - `script/events/executioner.js`（2343 行）— 处刑者战舰大地牢
> - `script/events.js`（1487 行）— 事件/战斗引擎（本期新增 status/specials/explosion 机制）
> - `script/world.js`（1109 行）— `doSpace` 分支、`goHome` 解锁链、`redeemBlueprints`
> - `script/path.js`（341 行）— 背包容量/重量（cargo drone 档）
> - `script/engine.js`（942 行）— tab ring 与模块切换
> - `script/header.js`（35 行）— `Header.addLocation` 的插入位置语义
> - `script/scoring.js`（35 行）/ `script/prestige.js`（103 行）— 结局分数与轮回
> - `script/events/setpieces.js`（3587 行）— `ship` setpiece（W 地标）
> - `css/space.css` / `css/main.css` — 太空关的几何（700×700 场地、32px 陨石）
> - `lang/zh_cn/strings.po`（3337 行，793 msgid）— 官方简体中文翻译
>
> **术语约定**：时间常量单位见上下文（战斗冷却/attackDelay 为**秒**，动画与 specials/status 为**毫秒**，Space 的 delay 为**毫秒**）。凡标「未核实」处，实现前需再对源码确认。
> **对照文档**：`docs/research-phase2.md`（Path + World）。凡本文与其记载冲突处，均在 §5.4 单列勘误。

## 目录

**上半部：原作机制全量拆解**
- §0 Phase 3 是什么 / 三条主线与移植难点速览
- §1 Ship — 星舰页（解锁、hull/thrusters、起飞流程）
- §2 Space — 太空关完整帧循环拆解
- §3 Executioner — 处刑者战舰 setpiece 全图
- §4 Fabricator — 蓝图与制造表
- §5 与 Phase 1/2 既有系统的耦合点清单 + 勘误
- §6 文案 / zh_cn 覆盖度（Phase 3 是翻译大缺口）

**下半部：PaperDarkRoom 适配设计**
- §7 硬件与显示前提（MSG 1bpp 驱动的既定事实）
- §8 太空玩法在墨水屏上的重设计
- §9 残影治理节奏
- §10 Ship 页 / Executioner / Fabricator 到现有模块的映射表
- §11 分期建议与验收标准
- §12 开放问题清单

---

## §0 Phase 3 是什么

Phase 1 = Room/Outside/Trade/事件；Phase 2 = Path/World（探索与战斗）。Phase 3 是**通关线**，由三块彼此串联的内容组成：

```
World 走到 W(SHIP,距村庄 28) → salvage → 回村 goHome → 开「星舰」页
World 走到 X(EXECUTIONER,距村庄 28) → 打穿 4 个区 → 回村 goHome → 开「制造机」页
制造机造 alien alloy 装备 ↔ 星舰页用 alien alloy 加固船身/升级引擎
星舰页 lift off → 太空关（60 秒躲陨石）→ 通关结算 + 存档删除 + prestige
```

**三条主线的独立性**：
- **Ship + Space** 是**完整可玩的最小通关路径**——只要 W 地标被 salvage、并用 1 个 alien alloy 加固过一次船身，就能起飞并通关。Executioner/Fabricator **不是通关必需**，是难度缓冲与内容扩展。
- **Executioner** 是纯 setpiece 内容（2343 行，5 个事件、96 个场景），产出蓝图 → 喂 Fabricator。
- **Fabricator** 是一个新"位置页"，把 alien alloy 变成 Phase 2 已埋好档位的顶级装备（`cargo drone` 110 容量、`fluid recycler` 110 水、`kinetic armour` 85 HP、`plasma rifle` 12 伤）。

**移植难度最大的三点**：

1. **Space 是全游戏唯一的实时动作玩法（§2 / §8）**。上游用 `setInterval(33ms)` 移动飞船 + jQuery 逐帧 `progress` 回调做碰撞检测，键盘按住即持续移动，屏幕上同时有十几个 32px 旋转字符在下落。移植到 e-ink（1bpp、43.4 Hz 扫描、整帧重绘、只有触摸）必须**从根上重设计**：帧率、操船输入、陨石可读性三件事都不能照抄。这是 Phase 3 的核心风险。
2. **Executioner 的规模（§3）**。96 个场景、25 处战斗、4 种共享敌人、6 种蓝图掉落，且引入了 Phase 2 战斗引擎里**完全没有的机制**：`status`（shield/enraged/energised/meditation/venomous/boost）、`specials`（定时施加状态）、`atHealth`（血线触发）、`explosion`（死亡自爆反伤）、`nextEvent`（事件间跳转）。现有 `setpiece_engine` 的 `SpScene` 结构装不下，需要扩表。
3. **通关即销毁存档（§2.6）**。上游 `Space.endGame()` 里 `Engine.deleteSave(true)` + `Prestige.save()`——通关会**删档并把部分库存带进下一轮**。这套语义在一块常驻桌面的墨水屏上要不要原样保留，是必须先拍板的产品决策（§12 Q1）。

---

## §1 Ship — 星舰页

出处：`ship.js`（整文件，177 行）。

### 1.1 解锁链：W 地标 → `Ship.init()`

三步，缺一不可：

| 步骤 | 出处 | 说明 |
|---|---|---|
| 1. 地图上有 W | `world.js:147` `World.LANDMARKS[World.TILE.SHIP] = { num: 1, minRadius: 28, maxRadius: 28, scene: 'ship', label: _('A&nbsp;Crashed&nbsp;Starship')}` | 距村庄曼哈顿距离**恒为 28**（min==max ⇒ `Math.random()*(max-min)`=0） |
| 2. 踩上去触发 setpiece | `world.js doSpace()` → `Events.startEvent(Events.Setpieces['ship'])` | 见下 |
| 3. 活着走回村庄格 | `world.js goHome()`：`if(World.state.ship && !$SM.get('features.location.spaceShip')) { Ship.init(); Engine.event('progress','ship'); }` | **死了就白跑**（`World.state` 整个丢弃） |

`ship` setpiece（`setpieces.js:3140-3163`）只有一个场景，**无战斗、无消耗、无失败分支**：

```js
"ship": { /* Finding a way off this rock */
  title: _('A Crashed Ship'),
  scenes: { 'start': {
    onLoad: function() {
      World.markVisited(World.curPos[0], World.curPos[1]);   // tile 变 'W!'，永不再触发
      World.drawRoad();                                       // 并入路网
      World.state.ship = true;                                // 临时态标志
    },
    text: [ 'the familiar curves of a wanderer vessel rise up out of the dust and ash. ',
            "lucky that the natives can't work the mechanisms.",
            'with a little effort, it might fly again.' ],
    buttons: { 'leavel': { text: _('salvage'), nextScene: 'end' } }   // 注：上游 key 拼错成 leavel
  }},
  audio: AudioLibrary.LANDMARK_CRASHED_SHIP
}
```

注意 `title` 是 `A Crashed Ship`，而地图 tooltip label 是 `A Crashed Starship`，星舰页标题又是 `An Old Starship`——**三个不同的串**，别合并。

### 1.2 状态与常量

`ship.js:5-9`：

| 常量 | 值 | 含义 |
|---|---|---|
| `LIFTOFF_COOLDOWN` | 120（秒） | 起飞按钮冷却 |
| `ALLOY_PER_HULL` | 1 | 加固一次船身的 alien alloy 成本 |
| `ALLOY_PER_THRUSTER` | 1 | 升级一次引擎的 alien alloy 成本 |
| `BASE_HULL` | **0** | 初始船身 |
| `BASE_THRUSTERS` | **1** | 初始引擎 |

持久状态（`ship.js:17-23`）：

```js
if(!$SM.get('features.location.spaceShip')) {
  $SM.set('features.location.spaceShip', true);
  $SM.setM('game.spaceShip', { hull: Ship.BASE_HULL, thrusters: Ship.BASE_THRUSTERS });
}
```
另有 `game.spaceShip.seenShip`（首次进页通知的一次性标志）与 `game.spaceShip.seenWarning`（首次起飞确认事件的一次性标志）。

**关键语义**：`Ship.getMaxHull()` 直接返回 `game.spaceShip.hull`（`ship.js:129-131`）——**hull 既是上限也是"买了几点"**，没有"当前 hull"这个持久量；太空关里的耗损写在 `Space.hull` 这个临时变量上，坠毁**不扣持久 hull**。

**上限：无。** `reinforceHull` / `upgradeEngine` 没有任何 maximum 判定，只要有 alien alloy 就能一直加。所以"hull/thrusters 上限"这一提法在上游不成立——真正的上限是 alien alloy 的产出速率。

### 1.3 三个按钮

| 按钮 | id | 成本 | 效果 | 音效 |
|---|---|---|---|---|
| `reinforce hull` | `reinforceButton` | `{'alien alloy': 1}` | `stores['alien alloy'] -= 1`；`game.spaceShip.hull += 1`；hull 从 0 变正时**解禁起飞按钮**；刷新 hullRow | `REINFORCE_HULL` |
| `upgrade engine` | `engineButton` | `{'alien alloy': 1}` | `stores['alien alloy'] -= 1`；`game.spaceShip.thrusters += 1`；刷新 engineRow | `UPGRADE_ENGINE` |
| `lift off` | `liftoffButton` | — | `Ship.checkLiftOff()`；带 `cooldown: 120` 秒 | `LIFT_OFF`（在 `liftOff()` 里） |

余量不足时（`ship.js:105-108`、`:119-122`）推通知 `not enough alien alloy` 并 `return false`（不扣冷却）。

`init()` 末尾：`if($SM.get('game.spaceShip.hull') <= 0) Button.setDisabled(b, true);`——**初次开页起飞按钮是禁用的，必须先加固一次船身**（`BASE_HULL=0`）。

页面 UI 极简（`ship.js:36-45`）：两行 `hull:` / `engine:` 的 key-val（各 70px 宽，`ship.css`），下面三个 100px 按钮。

### 1.4 起飞流程 `checkLiftOff()` → `liftOff()`

`ship.js:133-172`：

```
checkLiftOff():
  if !game.spaceShip.seenWarning:
      Events.startEvent({ title: 'Ready to Leave?', scenes: { start: {
          text: ["time to get out of this place. won't be coming back."],
          buttons: {
            'fly'  : text 'lift off' → onChoose: seenWarning=true; Ship.liftOff()  → nextScene 'end'
            'wait' : text 'linger'   → onChoose: Button.clearCooldown($('#liftoffButton')) → 'end'
          }}}})
  else:
      Ship.liftOff()

liftOff():
  $('#outerSlider').animate({top:'700px'}, 300)   // 场景整体上移 700px，Space 面板从 top:-700px 滑进来
  Space.onArrival()
  Engine.activeModule = Space
  AudioEngine.playSound(AudioLibrary.LIFT_OFF)
```

要点：
- **首次警告只出现一次**（`seenWarning` 持久化）。选 `linger` 会**清掉起飞按钮的 120 秒冷却**（因为按钮点击时冷却已经起转），选 `fly` 则不清——但紧接着就进太空关了。
- 起飞**不消耗任何资源**，也**不检查 Path 背包**。太空关里唯一的资源就是 hull。

### 1.5 与 world/executioner 的状态联动

| 联动 | 出处 | 说明 |
|---|---|---|
| W 清完 → 开星舰页 | `world.js:965-968` | `World.state.ship` → `Ship.init()` |
| X 清完 → 开制造机页 | `world.js:969-973` | `World.state.executioner` → `Fabricator.init()` + 通知 `builder knows the strange device when she sees it. takes it for herself real quick. doesn't ask where it came from.` |
| 重开档恢复页面 | `engine.js:230-234` | `if(features.location.fabricator) Fabricator.init();` **然后** `if(features.location.spaceShip) Ship.init();`——**Fabricator 先于 Ship 初始化** |
| tab 插入位置 | `fabricator.js:99` `Header.addLocation(_("A Whirring Fabricator"), "fabricator", Fabricator, 'ship')` + `header.js:20-31` | 第 4 参数 `before`：制造机 tab **插在星舰 tab 之前** |
| 结局分数含 hull | `scoring.js:23` | `fullScore += Ship.getMaxHull() * 50` |

**tab ring 顺序**（`engine.js:716-751`，键盘左右键的实际遍历）：

```
Room ← Outside ← Path ← Fabricator ← Ship        (左键)
Room → Outside → Path → Fabricator → Ship        (右键)
```
即 Phase 3 在既有 ring 尾部追加 **两页**：Fabricator、Ship。注意 `engine.js:621-629` 把 Room/Path/Fabricator 归为"横向 locationSlider 页"，Outside/World/Space 走 outerSlider——这是 DOM 布局细节，移植时不需要保留。

---

## §2 Space — 太空关

出处：`space.js`（631 行）、`css/space.css`、`css/main.css`。

### 2.1 常量表（含**未使用**标记）

`space.js:5-13`：

| 常量 | 值 | 是否被使用 | 实际作用 |
|---|---|---|---|
| `SHIP_SPEED` | 3 | ✔ | 基础速度（px / 33ms 帧） |
| `BASE_ASTEROID_DELAY` | 500 | ✘ **死代码** | 全文件仅此一处声明，无引用 |
| `BASE_ASTEROID_SPEED` | 1500 | ✔ | 陨石下落动画时长基数（ms） |
| `FTB_SPEED` | 60000 | ✔ | fade-to-black 时长 = **通关总时长 60 秒** |
| `STAR_WIDTH` / `STAR_HEIGHT` | 3000 / 3000 | ✔ | 星空图层尺寸 |
| `NUM_STARS` | 200 | ✔ | 每层星星数 |
| `STAR_SPEED` | 60000 | ✔ | 前景星层滚动周期；背景层 ×2 |
| `FRAME_DELAY` | 100 | ✘ **死代码** | 声明后无引用 |

> **勘误提示**：任务书里提到的 "BASE_ASTEROID_DELAY 与难度曲线"在当前 master 已**不成立**——陨石节奏由 `1000 - altitude*10` 硬编码（§2.4），`BASE_ASTEROID_DELAY` 是历史遗留。

### 2.2 场地几何

- 场地 = `div.location` **700 × 700 px**（`main.css:177-183`，`#content` height 700）。
- 飞船 `#ship`：文本 `@`，`position:absolute`，`margin-top:-10px; margin-left:-7.5px`（`space.css`）——即 CSS `left/top` 是"文字左上角"，视觉中心被偏移半个字符。
- 初始位置：`Space.ship.css({top:'350px', left:'350px'})`（`space.js:64-67`）——正中心。
- 移动边界（`space.js:223-232`）：`x ∈ [10, 690]`，`y ∈ [10, 690]`。
- 陨石 `.asteroid`：`font-size: 32px`，`top:-40px` 起始，CSS `animation: 1s linear infinite spin`（持续自转）。
- 陨石 x：`Math.floor(Math.random() * 700)` ∈ [0, 699]（`space.js:116`）。
- 陨石落到 `top:'740px'` 后 `remove()`——总行程 **780 px**。
- HUD `#hullRemaining`：绝对定位 `top:0; left:0`，宽 70px，内容 `hull: <cur>/<max>`。

### 2.3 移动模型 `moveShip()`（`space.js:191-243`）

定时器：`Space._shipTimer = setInterval(Space.moveShip, 33)`（`space.js:69`）≈ **30 fps**。

```
speed = getSpeed() = SHIP_SPEED(3) + game.spaceShip.thrusters
dx = dy = 0
if up:    dy -= speed   else if down:  dy += speed     // 上下互斥，up 优先
if left:  dx -= speed   else if right: dx += speed     // 左右互斥，left 优先
if dx≠0 and dy≠0: dx /= √2 ; dy /= √2                  // 对角归一化，斜向不加速
if lastMove≠null:                                       // 帧间隔补偿
    dt = Date.now() - lastMove
    dx *= dt/33 ; dy *= dt/33
x += dx ; y += dy ; clamp 到 [10,690]
shipX = x ; shipY = y ; 写回 CSS ; lastMove = Date.now()
```

**推导出的速度**：thrusters=1（初始）→ speed=4 px/帧 → 4/0.033 ≈ **121 px/s**；横穿 680px 场地需 5.6 s。每升一级引擎 +30 px/s。thrusters=10 → speed 13 → 394 px/s，横穿 1.7 s。

### 2.4 陨石生成与难度曲线 `createAsteroid()`（`space.js:102-189`）

**字形抽取**（`space.js:103-114`）：`r=Math.random()`，`<0.2 → '#'`，`<0.4 → '$'`，`<0.6 → '%'`，`<0.8 → '&'`，否则 `'H'`。五种等概率各 20%。

**下落时长**（`space.js:126`）：
```js
duration: Space.BASE_ASTEROID_SPEED - Math.floor(Math.random() * (Space.BASE_ASTEROID_SPEED * 0.65))
        = 1500 - floor(random * 975)   →  525 ~ 1500 ms
```
对应垂直速度 780px / (0.525~1.5 s) = **520 ~ 1486 px/s**——即使是最慢的陨石也比初始飞船快 4 倍。

**每波数量（难度随高度）**（`space.js:166-184`）：主调用 `createAsteroid()`（`noNext=false`）先造 1 颗，然后：

| altitude 区间 | 额外生成 | 每波总数 |
|---|---|---|
| 0 – 10 | — | **1** |
| >10 – 20 | +1 | **2** |
| >20 – 40 | +1 +2 | **4** |
| >40 – 60 | +1 +2 +2 | **6** |

**下一波间隔**（`space.js:186`）：`Engine.setTimeout(Space.createAsteroid, 1000 - (Space.altitude * 10), true)`
→ altitude 0 时 1000 ms，altitude 60 时 400 ms。**线性收紧**。

**综合陨石密度**：

| altitude | 每波 | 间隔(ms) | 颗/秒 |
|---|---|---|---|
| 0 | 1 | 1000 | 1.0 |
| 10 | 1 | 900 | 1.1 |
| 11 | 2 | 890 | 2.2 |
| 20 | 2 | 800 | 2.5 |
| 21 | 4 | 790 | 5.1 |
| 40 | 4 | 600 | 6.7 |
| 41 | 6 | 590 | 10.2 |
| 60 | 6 | 400 | **15.0** |

同屏存活数 ≈ 颗/秒 × 平均寿命(≈1.0 s)，末段约 15 颗。

### 2.5 碰撞检测与 hull 扣减

碰撞跑在 jQuery 动画的 `progress` 回调里（`space.js:128-161`），即**每个陨石各自每动画帧检测一次**（jQuery 默认 `fx.interval` 13 ms / rAF）。判定是**点对盒**：

```js
a.data({ xMin: x, xMax: x + a.width(), height: a.height() });   // 生成时缓存
// progress:
if (xMin <= Space.shipX && xMax >= Space.shipX) {
    aY = parseFloat(css('top'));
    if (aY <= Space.shipY && aY + height >= Space.shipY) {
        t.remove();
        Space.hull--;  Space.updateHull();
        <播 ASTEROID_HIT_n>
        if (Space.hull === 0) Space.crash();
    }
}
```

要点：
- 玩家被当成**一个点**（`shipX/shipY` 是 CSS left/top，不是视觉中心；由于 `margin-left:-7.5px; margin-top:-10px`，判定点大约在 `@` 字形的**右下偏内**）。
- 陨石是它的**字形包围盒**（32px 字体 ⇒ 宽约 18–21px、高约 37px，取决于字体度量）。
- 命中即**移除该陨石**，一颗陨石只能打一次。
- `Space.hull` 是**临时量**，初值 `Ship.getMaxHull()`；`hull === 0` 才 crash（严格等于，因为每次只减 1）。
- **持久 `game.spaceShip.hull` 全程不变**——坠毁没有资源惩罚。

**音效映射**（`space.js:143-154`）：`r = Math.floor(Math.random()*2)` ∈ {0,1}
| altitude | 索引 | 实际播放 |
|---|---|---|
| >40 | r+6 | `ASTEROID_HIT_6` / `_7` |
| >20 | r+4 | `ASTEROID_HIT_4` / `_5` |
| 其他 | r+1 | `ASTEROID_HIT_1` / `_2` |

> **发现**：`AudioLibrary` 里声明了 `ASTEROID_HIT_1..8`（`audioLibrary.js:82-89`），但 `_3` 和 `_8` **永远不会被播放**（r 只能取 0/1）。又一处上游死代码。

其它音效：`MUSIC_SPACE`（背景乐，`onArrival` 里播了**两次**，`space.js:56` 与 `:71`——重复调用，无害）、`CRASH`、`MUSIC_ENDING`、`LIFT_OFF`（在 `Ship.liftOff`）。

**音量渐弱**：`Space._volumeTimer = setInterval(Space.lowerVolume, 1000)`，`newVolume = 1.0 - altitude/60`（`space.js:623-630`），到通关时降为 0。

### 2.6 高度、大气层分段与胜利判定

**高度推进**（`space.js:271-279`）：`Space._timer = setInterval(..., 1000)`，每秒 `altitude += 1`；`altitude > 60` 时 `clearInterval`。

**标题（大气层分段）**（`space.js:74-92`，`setTitle()`）：

| altitude | 文案 key | zh_cn |
|---|---|---|
| < 10 | `Troposphere` | 对流层 |
| < 20 | `Stratosphere` | 平流层 |
| < 30 | `Mesosphere` | 中气层 |
| < 45 | `Thermosphere` | 增温层 |
| < 60 | `Exosphere` | 外逸层 |
| ≥ 60 | `Space` | 太空 |

> **上游 bug**：`setTitle()` 只在 `altitude % 10 === 0` 时被调用（`space.js:274-276`）。所以 **45 km 的 Thermosphere→Exosphere 边界不会在 45 生效，要等到 altitude=50**。移植时按 `%10` 触发会复刻这个 bug；按阈值触发则是"修正版"。建议按阈值触发（§12 Q4）。

**通关判定**：不是"高度到 60"，而是 `startAscent()` 里 `$('body').animate({backgroundColor: to_color}, {duration: FTB_SPEED(60000), complete: Space.endGame})`（`space.js:257-269`）——**背景色渐变动画结束即通关**。60 秒。altitude 只是显示与难度参数。

**其他视觉**：
- `startAscent()` 亮色模式把 body 从 `#FFFFFF` 渐变到 `#000000`（暗色模式 `#272823 → #EEEEEE`）；
- `Space._panelTimeout` 在 `FTB_SPEED/2`（30 s）时把 `#spacePanel` 与菜单文字变白（暗色模式变 `#272823`）——**背景变黑到一半时把前景反色**，保持可读；
- `drawStars()` 造两层星空（`#stars` 速度 60 s、`#starsBack` 速度 120 s + `opacity:0.5`），每层 200 颗 `.` ，用 `drawStarAsync` 每 100 ms 加一颗异步铺开。

### 2.7 crash 流程（`space.js:339-380`）

```
if(Space.done) return
Engine.keyLock = true ; Space.done = true
clearInterval(_timer / _shipTimer / _volumeTimer) ; clearTimeout(_panelTimeout)
body 移除 noMask，300ms 动画把背景色拉回 #FFFFFF（暗色 #272823）
  complete: 移除 stars/starsBack/#starsContainer，清空 body/#notifyGradient/#spacePanel 的内联样式
菜单文字 300ms 变 #666
#outerSlider 300ms animate top→0（场景滑回星舰页）
Engine.activeModule = Ship ; Ship.onArrival()
Button.cooldown($('#liftoffButton'))        // 重新起 120s 冷却
Engine.event('progress','crash')
AudioEngine.playSound(AudioLibrary.CRASH)
```

**crash 的代价 = 120 秒冷却，仅此而已。** hull / thrusters / 库存全部不变，可以无限重试。这一点对难度设计非常关键。

### 2.8 胜利结局序列（`space.js:382-567`）

`endGame()` 清掉全局所有定时器（`Engine._saveTimer`、`Outside._popTimeout`、`Engine._incomeTimeout`、`Events._eventTimeout`、`Room._fireTimer`、`Room._tempTimer`），并把 `Room.Craftables[*].button` / `Room.TradeGoods[*].button` 全部置 null。然后按时间线演出：

| t（累计，ms） | 动作 | 出处 |
|---|---|---|
| 0 | `Engine.event('progress','win')`；`MUSIC_ENDING` 开播；`#hullRemaining` 500ms 淡出 | `:384,403,405` |
| 0 → 3000 | 飞船 3000ms 线性移到 `top:350px, left:240px`（**向左偏移，为字幕留位**） | `:406-409` |
| 3000 → 5000 | 等待 2000ms | `:410` |
| 5000 → 5200 | 飞船 200ms 飞出 `top:-100px` | `:411-413` |
| 5200 | 重置 `#outerSlider` 定位；移除 `#locationSlider,#worldPanel,#spacePanel,#notifications`；`$('#header').empty()` | `:415-417` |
| 5200 → 7200 | 等待 2000ms | `:418` |
| 7200 → 9200 | `#starsContainer` 2000ms 淡出至 `opacity:0` + 背景色 `#000`（暗色 `#EEE`） | `:425-435` |
| 9200 | `Engine.GAME_OVER = true`；`Score.save()`；`Prestige.save()`；移除 `#content,#notifications`；`showExpansionEnding()` | `:437-446` |
| 之后 | `showEndingOptions()`；`Engine.options = {}`；**`Engine.deleteSave(true)`** | `:443-445` |

**`showExpansionEnding()`（`space.js:455-512`）** —— 仅当 `stores['fleet beacon'] > 0`（即打穿了 Executioner 指挥甲板）才播；否则立即 resolve：

| t（ms） | 文案 |
|---|---|
| 2000 | `the beacon pulses gently as the ship glides through space.<br>coordinates are locked. nothing to do but wait.` |
| 7000 | `the beacon glows a solid blue, and then goes dim. the ship slows.<br>gradually, the vast wanderer homefleet comes into view.<br>massive worldships drift unnaturally through clouds of debris, scarred and dead.` |
| 14000 | `the air is running out.` |
| 17000 | `the capsule is cold.` |
| 19500 | 出现 `wait` 按钮；点击后 5000ms 淡出 + 3000ms 延迟 → resolve |

（这几条是 hardcode 的 `.html()/.text()`，**没有走 `_()`**，所以 po 里也没有。）

**`showEndingOptions()`（`space.js:514-567`）**：
- `score for this game: {0}` ← `Score.calculateScore()`
- `total score: {0}` ← `Prestige.get().score`
- `restart.`（点 → `Engine.confirmDelete`）
- `expanded story. alternate ending. behind the scenes commentary. get the app.` + `iOS.` / `android.`（外链，移植时删）

**分数公式（`scoring.js`）**：
```
factor = [1, 1.5, 1, 2, 2, 3, 3, 2, 2, 2, 2, 1.5, 1, 1, 10, 30, 50, 100, 150, 150, 3, 3, 5, 4]
score  = Σ( stores[Prestige.storesMap[i].store] × factor[i] )
       + stores['alien alloy']  × 10
       + stores['fleet beacon'] × 500
       + Ship.getMaxHull()      × 50
```
`Prestige.storesMap` 的 24 项顺序（`prestige.js:12-35`）：wood, fur, meat, iron, coal, sulphur, steel, cured meat, scales, teeth, leather, bait, torch, cloth, bone spear, iron sword, steel sword, bayonet, rifle, laser rifle, bullets, energy cell, grenade, bolas。

对齐后的单项权重：

| 物品 | 权重 | 物品 | 权重 |
|---|---|---|---|
| wood | 1 | cloth | 1 |
| fur | 1.5 | bone spear | 10 |
| meat | 1 | iron sword | 30 |
| iron | 2 | steel sword | 50 |
| coal | 2 | bayonet | 100 |
| sulphur | 3 | rifle | 150 |
| steel | 3 | laser rifle | 150 |
| cured meat | 2 | bullets | 3 |
| scales | 2 | energy cell | 3 |
| teeth | 2 | grenade | 5 |
| leather | 2 | bolas | 4 |
| bait | 1.5 | alien alloy | **10** |
| torch | 1 | fleet beacon | **500** |
| | | hull（每点） | **50** |

**Prestige（轮回）**（`prestige.js:60-63,79-93`）：`Prestige.save()` 存 `previous.stores = getStores(true)`——把当前库存**按类型随机除以一个系数**再向下取整：`g`（原料）÷ `floor(rand*10)`、`w`（武器）÷ `floor(floor(rand*10)/2)`、`a`（弹药）÷ `ceil(rand*10*ceil(rand*10))`，除数为 0 时取 1。下一轮 `previous.stores` 存在 ⇒ 地图上生成 `U`(CACHE) 地标，踩上去 `Prestige.collectStores()` 全部返还。

### 2.9 输入模型（键盘）

`space.js:569-617`：`keyDown`/`keyUp` 只处理四向，**支持组合键（8 向）**：

| 键 | keyCode | 状态位 |
|---|---|---|
| ↑ / W | 38 / 87 | `Space.up` |
| ↓ / S | 40 / 83 | `Space.down` |
| ← / A | 37 / 65 | `Space.left` |
| → / D | 39 / 68 | `Space.right` |

状态位在 `onArrival` 全部清零。派发在 `engine.js:691-706`：`Engine.keyDown` 有 `Engine.keyPressed` 与 `Engine.keyLock` 双门（crash 时置 `keyLock=true` 锁死输入）。

**注意**：`Space` **没有** `swipeLeft/Right/Up/Down` 处理器（`engine.js:761-783` 会转发给 activeModule，但 Space 没实现）。**上游 Web 版在纯触摸设备上基本无法玩太空关**——移动端是靠原生 App 另做的控制。这就是我们必须自己设计触摸操船方案的原因（§8.3）。

---

## §3 Executioner — 处刑者战舰

出处：`events/executioner.js`（2343 行）。**5 个 `Events.Executioner[*]` 事件、共 96 个场景、25 处战斗。**

### 3.1 顶层结构与进入条件

`world.js doSpace()`（`world.js:573-576`）：

```js
} else if(curTile === World.TILE.EXECUTIONER) {
    const scene = World.state.executioner ? 'executioner-antechamber' : 'executioner-intro';
    Events.startEvent(Events.Executioner[scene]);
}
```

即：**X 格永远可重入**（不像 W 用 `markVisited` 变成 `W!` 后失效）。`World.state.executioner` 为假 → 走序章；为真 → 直接进前厅（电梯厅）。

`World.state` 上新增 **4 个布尔**（全部只在 `World.state` 临时态，**回村 `goHome` 才落库、死亡全丢**）：

| 字段 | 置位处 | 作用 |
|---|---|---|
| `executioner` | `executioner-intro` 场景 `'7'` 的 `onLoad`（同时 `World.drawRoad()`） | 解锁前厅；回村 → `Fabricator.init()` |
| `engineering` | `executioner-engineering` 场景 `'8'` 的 `onLoad` | 前厅按钮变灰 |
| `medical` | `executioner-medical` 场景 `'17'` 的 `onLoad` | 同上 |
| `martial` | `executioner-martial` 场景 `'13'` 的 `onLoad` | 同上 |

三个区全清 → 前厅出现 `command deck` 按钮 → 打穿指挥甲板场景 `'7'` 的 `onLoad` 调 `World.clearDungeon()`（X 格变 `OUTPOST` + 画路）。

**移植警示**：因为这些标志活在 `World.state` 里，玩家**必须活着走回村庄**才能保住区进度。一趟远征里打穿一个区、然后在回村路上饿死 → 全部作废。这与 Phase 2 的地牢语义一致，但 Executioner 的单区时长远超普通地牢，风险被放大（§12 Q6）。

### 3.2 共享敌人表 `Enemies.Executioner`（`executioner.js:1-115`）

四种机械敌人，被各区反复 `...spread` 进场景：

| key | enemy / enemyName | chara | ranged | damage | hit | attackDelay(s) | health | 特殊 | notification |
|---|---|---|---|---|---|---|---|---|---|
| `guard` | mechanical guard | `G` | ✔ | 10 | 0.8 | 2 | 60 | — | `tripped a motion sensor.` |
| `quadruped` | mechanical quadruped | `Q` | ✘ | 8 | 0.8 | 1 | 70 | — | `a mobile defence platform trundles around the corner.` |
| `medic` | broken medic | `M` | ✘ | 15 | 0.8 | 3 | 80 | `atHealth {40: → venomous}` | `a medical drone wheels out of control.` |
| `turret` | defence turret | `T` | ✔ | 25 | 0.8 | 4 | 50 | — | `one of the defence turrets still works.` |

掉落表：

| key | 掉落（min–max @chance） |
|---|---|
| `guard` | energy cell 1–5@0.8；laser rifle 1–1@0.8；alien alloy 1–1@0.2 |
| `quadruped` | **alien alloy 2–4@0.2**（见下） |
| `medic` | alien alloy 1–2@1；hypo 1–4@0.2 |
| `turret` | energy cell 1–5@0.8；alien alloy 1–1@0.8；laser rifle 1–1@0.2 |

> **上游 bug**：`quadruped.loot` 里 `'alien alloy'` 这个 key **出现了两次**（`executioner.js:44-53`）：`{min:1,max:1,chance:1}` 与 `{min:2,max:4,chance:0.2}`。JS 对象字面量重复键**后者覆盖前者**，所以实际生效的只有 `2–4@0.2`——四足机器人 80% 概率**一无所获**。移植时按"实际行为"复刻（只留 2–4@0.2）还是按"显然的作者意图"修（两条都保留）需要拍板（§12 Q5）。

掉落数量公式仍是 Phase 2 §4.2 那条（`events.js drawLoot`）：命中后 `num = floor(random*(max−min)) + min`，即实际范围 `[min, max−1]`；`max==min` 时恒为 `min`。所以 `energy cell 1–5` 实为 1–4，`alien alloy 1–1` 恒为 1。

### 3.3 新增战斗机制（Phase 2 战斗引擎没有的）

`events.js` 本期为 Executioner 引入了一整套状态系统。**这是 Phase 3 战斗层最大的改动点。**

**常量**（`events.js:15-22`）：

| 常量 | 值 | 用途 |
|---|---|---|
| `STUN_DURATION` | 4000 ms | bolas/disruptor 眩晕时长 |
| `ENERGISE_MULTIPLIER` | **4** | `energised` 状态下一击伤害 ×4 |
| `EXPLOSION_DURATION` | 3000 ms | 自爆前摇 |
| `ENRAGE_DURATION` | 4000 ms | `enraged` 持续 |
| `MEDITATE_DURATION` | 5000 ms | `meditation` 持续 |
| `BOOST_DURATION` | 3000 ms | 玩家 `boost` 持续 |
| `BOOST_DAMAGE` | 10 | 使用 stim 的自伤 |
| `DOT_TICK` | 1000 ms | 持续伤害 tick |

**六种 status**（`events.js setStatus` / `damage` / `enemyAttack`）：

| status | 施加者 | 效果 | 出处 |
|---|---|---|---|
| `shield` | 敌方 specials（`unstable prototype`、`immortal wanderer`）；玩家按 `shield` 钮（需持有 kinetic armour） | 承伤方带 shield 时，伤害**反向变成治疗**（`enemyHp = min(maxHp, hp + dmg)`，浮字显 `+N`）；**一击即破**（`enemy.data('status','none')`） | `events.js:617,635-636,651-654` |
| `enraged` | 敌方 specials（`malformed experiment`、`immortal wanderer`） | 若施加对象是敌人 → `startEnemyAttacks(0.5)`，攻击间隔强制降到 **0.5 秒**，持续 `ENRAGE_DURATION(4s)` 后恢复原 `attackDelay` | `events.js:182-188` |
| `energised` | 敌方 specials（`murderous robot`） | 攻击方下一击伤害 **×4**（`ENERGISE_MULTIPLIER`），命中后立即清除 | `events.js:618,626-628,680-684` |
| `venomous` | `medic` 的 `atHealth {40}`（血量首次跌破 40 时） | 攻击方下一击命中后，对承伤方挂 `setInterval(DOT_TICK=1s)` 的**持续伤害**，每 tick `floor(dmg/2)`；被 shield 挡下则不挂 | `events.js:619,644-649` |
| `meditation` | 敌方 specials（`immortal wanderer`） | 承伤方进入冥想：期间**所有伤害被吸收累加进 `Events._meditateDmg`，不掉血**；`enemyAttack` 时若 `_meditateDmg > 0` 则把累计值**一次性反弹**给玩家（无视命中判定）。持续 5 s，期间敌人不主动攻击 | `events.js:189-193,630-633,739-748` |
| `boost` | 玩家按 `boost`（stim） | 攻击按钮进入 boosted 视觉；同时 `dotDamage(player, BOOST_DAMAGE=10)` **自伤 10**；3 s 后清除。（注：源码里 `boost` 只设了状态与自伤，**攻击按钮的 `boosted:` 回调是 Button 层的视觉/加速，伤害倍率未在 `damage()` 中体现**——移植时需自行定义 boost 的实际收益，§12 Q7） | `events.js:373,461-466,195-199` |

**`specials`**（`events.js:161-171`）：场景级数组，每项 `{ delay: <秒>, action: fighter => status }`。`startCombat` 里 `Engine.setInterval(..., delay*1000)` **周期性重复触发**（不是一次性）。

**`atHealth`**（`events.js:559-567`）：场景级对象 `{ <hpThreshold>: fn }`，在玩家攻击落地后检查 `enemyHp <= threshold && enemyHp + dmg > threshold`（即**本次攻击跨过阈值**）才触发一次。

**`explosion`**（`events.js:560,572-574,583-595`）：场景级数字。敌人 HP 归零时不直接 `winFight()`，而是 `explode()`：3 秒前摇（加 `.exploding` class），然后把 `chara` 换成 `*`，对玩家造成 `explosion` 点 ranged 伤害；玩家没被打死才 `winFight()`。

**`nextEvent`**（`events.js:1271-1275`）：按钮字段，值为事件 key。触发 `Events.switchEvent()`——移除当前事件面板、`eventStack.shift()`、`startEvent(新事件)`。前厅靠这个跳进四个区。

**`available`**（按钮字段，函数）：前厅用它做"已清区变灰"。

### 3.4 `executioner-intro` — 序章（14 场景）

`title: A Ravaged Battleship`，`audio: LANDMARK_CRASHED_SHIP`。

| 场景 | 类型 | 内容要点 | 掉落 / 成本 | 出口 |
|---|---|---|---|---|
| `start` | 文本 | notify `the remains of a huge ship are embedded in the earth.`；3 段文本 | `enter` **cost `{torch: 1}`** | `enter`→`1`；`leave`→end |
| `1` | 文本 | 船内冰冷黑暗，墙壁嗡鸣 | — | `continue`→**{0.4:`2-1`, 0.8:`2-2`, 1:`2-3`}**（三条支线各 40/40/20%）；`leave`→end |
| **支线 A（蛛网 / 节肢）** | | | | |
| `2-1` | 拾取 | 蛛网走廊，挂着一个小背包 | cured meat 1–5@0.8；bullets 1–5@0.5；energy cell 1–5@0.2 | →`3-1` |
| `3-1` | 战斗 | `chitinous horror` chara `H`，dmg **1**，hit 0.7，**attackDelay 0.25 s**，HP 60 | meat 5–10@0.8；scales 5–10@0.5 | →`4-1` |
| `4-1` | 战斗 | `chitinous queen` chara `Q`，dmg 1，hit 0.7，attackDelay 0.25 s，HP 70 | meat 8–12@0.8；scales 8–12@0.5 | →`5` |
| **支线 B（军方营地）** | | | | |
| `2-2` | 战斗 | `operative` chara `O`，dmg 8，hit 0.8，delay 2 s，HP 60 | bayonet 1–1@0.5；bullets 1–5@0.8；cured meat 1–5@0.8 | →`3-2` |
| `3-2` | 拾取 | 军方小营地 | cured meat 1–5@1；torch 1–3@0.8；bullets 1–5@0.5；alien alloy 1–2@0.2 | →`4-2` |
| `4-2` | 战斗 | `researcher` chara `R`，dmg 1，hit 0.8，delay 2 s，HP 20 | torch 1–3@0.8；cloth 1–5@0.8；cured meat 1–5@0.8 | →`5` |
| **支线 C（路障 / 古兽）** | | | | |
| `2-3` | 拾取 | 路障后遗弃的武器 | **laser rifle 1–3@1**；energy cell 1–5@0.8；**plasma rifle 1–1@0.2** | →`3-3` |
| `3-3` | 拾取 | 被啃食的流浪者遗骸 | energy cell 1–5@0.5；cloth 1–5@0.8 | →`4-3` |
| `4-3` | 战斗 | `ancient beast` chara `A`，dmg 6，hit 0.8，delay 1 s，HP 60 | fur 5–10@1；meat 5–10@1；teeth 5–10@0.8 | →`5` |
| **汇合** | | | | |
| `5` | 文本 | 维修面板 + 密封舱门 | — | `power cycle`→`6`；`leave`→end |
| `6` | 战斗 | `automated turret` chara `T`，**ranged**，dmg 10，hit 0.8，delay **2.5 s**，HP 60 | energy cell 1–5@0.8；laser rifle 1–1@0.2 | →`7` |
| `7` | 文本 | `onLoad: World.drawRoad(); World.state.executioner = true;` | — | 唯一按钮 `take device and leave`→end |

**注意 `3-1` / `4-1` 的 `attackDelay: 0.25`** —— 每秒攻击 4 次、每次 1 点伤害的"细密啃咬"型敌人。上游是实时战斗所以成立；移到 1 秒 tick 的离散战斗上必须重新表达（§10.4）。

### 3.5 `executioner-antechamber` — 前厅（1 场景）

```
start: text ×2（宽走廊 → 电梯厅，电梯仍能用）
  buttons:
    engineering  available: !World.state.engineering  → nextEvent 'executioner-engineering'
    medical      available: !World.state.medical      → nextEvent 'executioner-medical'
    martial      available: !World.state.martial      → nextEvent 'executioner-martial'
    command deck available: engineering && medical && martial → nextEvent 'executioner-command'
    leave        → 'end'
```

**每次离开区（`leave`）都直接 end 整个事件，回到世界地图**——不会退回前厅。要打下一个区必须再踩一次 X 格。

### 3.6 `executioner-engineering` — 工程区（21 场景）

`title: Engineering Wing`。终点掉 **hypo blueprint** + **kinetic armour blueprint**。

```
start → {0.3: '1-1', 0.7: '1-2', 1: '1-3'}          (30% / 40% / 30%)

支线 1: 1-1  拾取 energy cell 1–5@0.8, laser rifle 1–1@0.2
        → {0.5:'2-1a', 1:'2-1b'}
        2-1a 战斗 unruly welder  'W' dmg13 hit0.8 delay2 HP50
                 loot energy cell 1–5@0.8, alien alloy 1–1@0.2      → '3-1'
        2-1b 文本                                                    → '3-1'
        3-1  = guard                                                 → '4'

支线 2: 1-2  = turret                                                → '2-2'
        2-2  拾取 引擎室残骸 alien alloy 2–5@1
             → {0.5:'3-2a', 1:'3-2b'}
        3-2a = guard  → '4'      3-2b 文本 → '4'

支线 3: 1-3  火灾走廊【无 leave 按钮，必须二选一】
             'extinguish' cost {water: 5}   → {0.5:'2-3a', 1:'2-3b'}
             'rush through' cost {hp: 10}   → {0.5:'2-3a', 1:'2-3b'}
        2-3a = guard → '3-3'     2-3b 文本 → '3-3'
        3-3  拾取 energy cell 1–5@0.8, laser rifle 1–1@0.7,
                  grenade 1–3@0.6, plasma rifle 1–1@0.2              → '4'

汇合 4   研发室：'use machine' cost {alien alloy: 1}
                  onChoose: World.setHp(World.getMaxHealth())  → '4-heal'
                'continue' → {0.5:'5-1', 1:'5-2'} ; 'leave' → end
     4-heal 文本 → {0.5:'5-1', 1:'5-2'}
     5-1  = turret → '6'          5-2 文本 → '6'
     6    拾取 **hypo blueprint 1–1@1**                              → '7-intro'
     7-intro 文本【唯一按钮 'fight'】                                 → '7'
     7    战斗 unstable prototype 'P' dmg5 hit0.8 delay2 **HP 150**
              specials: [{ delay: 5s → shield }]
              loot alien alloy 1–3@1, **kinetic armour blueprint 1–1@1** → '8'
     8    onLoad: World.state.engineering = true ; 'leave' → end
```

**关键成本**：`1-3` 的 `cost {hp: 10}` 是全上游**唯一一处用 HP 当按钮成本**的地方（`{water: 5}` 则是唯一一处用 water 当成本）。现有 `SpButton.costSlot` 只支持 Res/Item，需扩（§10.3）。

### 3.7 `executioner-martial` — 军事区（25 场景）

`title: Martial Wing`。终点掉 **plasma rifle blueprint** + **disruptor blueprint**。

```
start 文本 → '1'
1  走廊分岔：
   'blow it down'  cost {grenade: 1}  → '2-1'      （军械库大奖）
   'continue right'                    → {0.5:'2-2', 1:'2-3'}
   'leave' → end

   2-1 军械库拾取【本区最大奖】
       energy blade 2–5@1 ; laser rifle 2–5@1 ; energy cell 5–20@1 ;
       grenade 1–5@0.8 ; plasma rifle 1–1@0.2                        → '3-1'
   3-1 = turret → '4-1'      4-1 文本 → '5'

   2-2 = turret → {0.5:'3-2a', 1:'3-2b'}
   3-2a = quadruped → '4-2'   3-2b 文本 → '4-2'
   4-2 拾取 energy cell 1–5@1, energy blade 1–1@0.2                  → '5'

   2-3 拾取 alien alloy 1–3@1 → {0.5:'3-3a', 1:'3-3b'}
   3-3a = guard → '4-3'       3-3b 文本 → '4-3'
   4-3 = quadruped → '5'

5  文本（路障 / 尸体）→ '6'
6  拾取 **plasma rifle blueprint 1–1@1** → {0.5:'7-1', 1:'7-2'}

   7-1 作战计划室：
       'scavenge maps'  onChoose: for(i<3) World.applyMap()  → '8-1a'
       'continue'                                            → '8-1b'
       'leave' → end
       8-1a = guard（notify `drew some attention with all that noise.`）→ '9-1'
       8-1b 文本 → '9-1'
       9-1  = guard（notify `ran straight into another one.`）→ '10'

   7-2 安检站 → {0.5:'8-2a', 1:'8-2b'}
       8-2a 文本 → '9-2'
       8-2b 拾取 laser rifle 2–2@1, energy cell 5–10@1 → '9-2'
       9-2  = quadruped → '10'

10 训练场：'use machine' cost {alien alloy:1} onChoose 满血 → '11'
          'continue' → '11' ; 'leave' → end
11 文本【唯一按钮 'engage'】→ '12'
12 战斗 murderous robot 'M' dmg10 hit0.8 delay3 **HP 250**
       specials: [{ delay: 13s → energised }]
       loot alien alloy 1–3@1, **disruptor blueprint 1–1@1**
       【唯一按钮 'continue'，无 leave】→ '13'
13 onLoad: World.state.martial = true ; 'leave' → end
```

> **上游 bug（`7-1` 的 scavenge maps）**：`World.applyMap()`（`world.js:687-697`）写的是 **`$SM.get('game.world.mask')`（持久态掩码）**，而探索期间地图渲染与提交用的都是 `World.state.mask`（`onArrival` 的深拷贝）。因此：
> - 活着回村 → `goHome()` 的 `$SM.setM('game.world', World.state)` 用 `World.state.mask` **覆盖掉**刚揭开的区域 ⇒ **3 次揭图全部作废**；
> - 死在半路 → `World.state` 丢弃，持久掩码保留 ⇒ **揭图反而生效**。
> 移植时应写 `World.state.mask`（即修掉这个 bug）。另注：`applyMap` 在当前 master 里**只有这一个调用点**（全仓 grep 确认）。

### 3.8 `executioner-medical` — 医疗区（30 场景，最长）

`title: Medical Wing`。终点掉 **glowstone blueprint** + **stim blueprint**。

```
start 文本 → '1'
1  = turret → '2'
2  文本 → {0.5:'3a', 1:'3b'}
3a = quadruped → '4'    3b 文本 → '4'
4  文本（病床输送系统）→ {0.5:'5-1', 1:'5-2'}

   5-1 = medic → {0.5:'6-1a', 1:'6-1b'}
       6-1a = medic（notify `it had friends.`）→ '7-1'
       6-1b 文本 → '7-1'
       7-1 拾取 laser rifle 1–1@1, energy cell 3–10@1 → '8'

   5-2 临时作战室 + 保险柜：
       'force locker' → '6-2a-intro' ; 'continue' → '6-2b' ; 'leave' → end
       6-2a-intro 拾取 energy cell 5–10@1, **hypo 1–3@1** → '6-2a'
       6-2a = medic（notify `the noise draws attention.`）→ '7-2'
       6-2b 文本 → '7-2'
       7-2 = quadruped → '8'

8  战斗 unstable automaton 'A' dmg10 hit **0.7** delay2 HP100
       **explosion: 30**（死亡时对玩家 30 点爆炸伤害）
       loot **glowstone blueprint 1–1@1**                            → '9'
9  文本 → {0.5:'10a', 1:'10b'}
10a = guard → '11'      10b 文本 → '11'
11  = medic → {0.5:'12-1', 1:'12-2'}

   12-1 冷藏样本 拾取 cured meat 5–10@1 → {0.5:'13-1a', 1:'13-1b'}
        13-1a = guard → '14-1'   13-1b 文本 → '14-1'
        14-1  = medic → '15'
   12-2 手术器械 → {0.5:'13-2a', 1:'13-2b'}
        13-2a = medic → '14-2'
        13-2b 拾取 grenade 3–8@1 → '14-2'
        14-2  = medic → '15'

15 文本（收容间全开）→ '16'
16 战斗 malformed experiment 'E' dmg5 hit0.8 delay2 **HP 200**
       specials: [{ delay: 16s → enraged }]
       loot **stim blueprint 1–1@1**
       【唯一按钮 'continue'】→ '17'
17 onLoad: World.state.medical = true ; 'leave' → end
```

医疗区是四区里 medic（HP 80、dmg 15、跌破 40 血挂 venomous）出现最多的（最多 5 次），加上 `8` 的自爆 30——**是四区中对 HP 压力最大的一条**。

### 3.9 `executioner-command` — 指挥甲板（9 场景，最终 BOSS）

`title: Command Deck`。前置：三区全清。

```
start 文本 → '1'
1  = guard → '2'
2  军官休息室 → {0.5:'3a', 1:'3b'}
3a 拾取 energy cell 3–10@1, grenade 1–5@0.8 → '4'
3b 拾取 hypo 1–3@1                          → '4'
4  文本（房间中央静坐的矮小身影，一瞬间已经站起）
   'approach' → '5' ; 'leave' → end
5  文本（流浪者形态，胸口水晶脉动；它说它看到了叛乱将至；说它不会死）
   【唯一按钮 'observe'】→ '6'
6  战斗 **immortal wanderer** chara **'@'** dmg 12 hit 0.8 delay 2 **HP 500**
     onLoad: Events._lastSpecial = 'none'
     specials: [{ delay: 7s → 从 ['shield','enraged','meditation'] 中
                              随机取一个（**排除上次用过的那个**）并施加 }]
     loot **fleet beacon 1–1@1**
     【唯一按钮 'continue'】→ '7'
7  文本（水晶亮起又熄灭，身影虚化消失）
   onLoad: World.clearDungeon()      // X 格 → OUTPOST + 画路
   'leave' → end
```

**BOSS 设计**：HP 500，每 7 秒随机换一种状态（护盾反伤 / 狂暴 0.5 s 攻击间隔 / 冥想吸伤后一次性反弹），且**保证不连续两次同状态**。掉落 `fleet beacon`——这是通关时 `showExpansionEnding()` 的开关，也是分数里 **500 分**的单项。

**注意**：`fleet beacon` 不在 `Path.Weight`（重 1）、不在 `World.leaveItAtHome` 的白名单里 ⇒ 回村会**留在家（stores）**，不会继续带出门。这正是 `Score.calculateScore()` 读 `stores['fleet beacon']` 的前提。

### 3.10 蓝图掉落总表

| 蓝图 | 掉落处 | 概率 |
|---|---|---|
| `hypo blueprint` | engineering `6` | 1.0（保底） |
| `kinetic armour blueprint` | engineering `7`（unstable prototype 击杀） | 1.0 |
| `plasma rifle blueprint` | martial `6` | 1.0 |
| `disruptor blueprint` | martial `12`（murderous robot 击杀） | 1.0 |
| `glowstone blueprint` | medical `8`（unstable automaton 击杀） | 1.0 |
| `stim blueprint` | medical `16`（malformed experiment 击杀） | 1.0 |

**六张蓝图全部 100% 掉落**，且每区恰好 2 张（工程/军事/医疗），指挥甲板不掉蓝图只掉 `fleet beacon`。

**兑换**：`World.redeemBlueprints()`（`world.js:989-1009`），在 `goHome()` 里、`World.state=null` **之前**调用：

```js
const redeem = (blueprint, item) => {
  if (Path.outfit[blueprint]) {
    $SM.set(`character.blueprints['${item}']`, true);
    delete Path.outfit[blueprint];
    redeemed = true;
  }
};
redeem('hypo blueprint','hypo');            redeem('kinetic armour blueprint','kinetic armour');
redeem('disruptor blueprint','disruptor');  redeem('plasma rifle blueprint','plasma rifle');
redeem('stim blueprint','stim');            redeem('glowstone blueprint','glowstone');
// redeemed → notify 'blueprints feed into the fabricator data port. possibilities grow.'
```

即：蓝图是**背包物品**，重量 1（不在 Weight 表），必须活着背回村；`character.blueprints[<成品名>] = true` 是持久 flag。

---

## §4 Fabricator — 制造机

出处：`fabricator.js`（244 行）。

### 4.1 解锁与页面

- 解锁：`world.js:969-973`，`World.state.executioner && !features.location.fabricator` → `Fabricator.init()` + 通知。
- tab：`Header.addLocation(_("A Whirring Fabricator"), "fabricator", Fabricator, 'ship')` —— **插在 ship tab 之前**（`fabricator.js:99`）。
- 面板同样 `insertBefore(Ship.panel)`（`fabricator.js:104-109`）。
- 首次进页通知：`the familiar hum of wanderer machinery coming to life. finally, real tools.`（`game.fabricator.seen` 一次性标志）。
- 背景乐复用 `MUSIC_SHIP`。
- 面板内容：顶部 `blueprints` 分组（已解锁蓝图的只读列表），下面 `fabricate:` 分组（按钮网格，每个 150px 宽）。

### 4.2 制造表 `Fabricator.Craftables`（`fabricator.js:7-90`）

**成本全部是 alien alloy，且只有 1 或 2。**

| key | 显示名 | type | maximum | 需蓝图 | 成本 | 数量 | buildMsg |
|---|---|---|---|---|---|---|---|
| `energy blade` | energy blade | weapon | — | ✘ | alien alloy **1** | 1 | `the blade hums, charged particles sparking and fizzing.` |
| `fluid recycler` | fluid recycler | upgrade | **1** | ✘ | alien alloy **2** | 1 | `water out, water in. waste not, want not.` |
| `cargo drone` | cargo drone | upgrade | **1** | ✘ | alien alloy **2** | 1 | `the workhorse of the wanderer fleet.` |
| `kinetic armour` | kinetic armour | upgrade | **1** | ✔ | alien alloy **2** | 1 | `wanderer soldiers succeed by subverting the enemy's rage.` |
| `disruptor` | disruptor | weapon | — | ✔ | alien alloy **1** | 1 | `somtimes it is best not to fight.`（原文拼写如此） |
| `hypo` | hypo | tool | — | ✔ | alien alloy **1** | **5** | `a handful of hypos. life in a vial.` |
| `stim` | stim | tool | — | ✔ | alien alloy **1** | 1 | `sometimes it is best to fight without restraint.` |
| `plasma rifle` | plasma rifle | weapon | — | ✔ | alien alloy **1** | 1 | `the peak of wanderer weapons technology, sleek and deadly.` |
| `glowstone` | **glow stone** | tool | — | ✔ | alien alloy **1** | 1 | `a smooth, perfect sphere. its light is inextinguishable.` |

要点：
- **三件无需蓝图**：`energy blade` / `fluid recycler` / `cargo drone`——只要开了制造机就能造。
- `hypo` 一次产 **5 个**（`quantity: 5`），按钮文本自动带 `(x5)`。
- `maximum` 只在三个 upgrade 上（各 1 件）；武器/工具无上限。
- `canFabricate(key)` = `!blueprintRequired || character.blueprints[key]`（`fabricator.js:213-215`）——**没蓝图的条目按钮根本不创建**，蓝图到手后由 `stateUpdate` 触发 `updateBuildButtons()` 动态加进来（带 300ms 淡入）。
- 制造音效复用 `AudioLibrary.CRAFT`。

> **上游 bug（`fabricate` 的 maximum 判定）**：`fabricator.js:220` 写的是 `const numThings = Math.min(0, $SM.get('stores[thing]', true));`——`Math.min(0, x)` 对非负库存**恒返回 0**，于是 `if (craftable.maximum <= numThings)` 永远为假。真正拦住重复制造的是 `updateBuildButtons()` 里的 `Button.setDisabled`（`:175-179`）。移植时按"按钮禁用"这条实现即可（作者显然想写 `Math.max`）。

### 4.3 产物的实际数值（散落在 world.js / path.js）

| 产物 | 生效位置 | 数值 |
|---|---|---|
| `cargo drone` | `path.js getCapacity()` | 背包容量 `10 + 100 = 110`（分级取最高，压过 convoy 70） |
| `fluid recycler` | `world.js getMaxWater()` | 水上限 `10 + 100 = 110` |
| `kinetic armour` | `world.js getMaxHealth()` | 最大 HP `10 + 75 = 85`；另外 `events.js:153-155` 据此在战斗里加 `shield` 按钮 |
| `plasma rifle` | `world.js Weapons` | verb `disintegrate`，ranged，**damage 12**，cooldown **1 s**，cost `energy cell ×1`；`Path.Weight` **5** |
| `energy blade` | `world.js Weapons` | verb `slice`，melee，**damage 10**，cooldown **2 s**，**无消耗**；不在 Weight 表 ⇒ 重 1 |
| `disruptor` | `world.js Weapons` | verb `stun`，ranged，**damage `'stun'`**，cooldown **15 s**，**无消耗**（对比 bolas 要消耗自身）；不在 Weight 表 ⇒ 重 1 |
| `hypo` | `world.js hypoHeal()` = `HYPO_HEAL` | 回 **30** HP；战斗按钮 cd `_HYPO_COOLDOWN = 7 s`，cost `hypo ×1` |
| `stim` | `events.js useStim` | 置玩家 `boost` 状态 3 s，同时 `dotDamage(player, 10)` 自伤；按钮 cd `_STIM_COOLDOWN = 10 s`。**注意 `useStim` 并不扣 `Path.outfit['stim']`**（只有 `startCombat` 用 `Path.outfit['stim'] > 0` 决定要不要画按钮）——又一处上游疏漏 |
| `glowstone` | — | **全仓无任何消费点**。只是一个可制造的 tool，没有任何机制读它。纯收藏品/未完成内容 |
| `shield` 按钮 | `events.js:153-155,343-351,455-459` | 需 `stores['kinetic armour'] > 0`；cd `_SHIELD_COOLDOWN = 10 s`；置玩家 `shield` 状态（挡下一击并把伤害转成治疗） |

**Phase 2 留档点核对**：`docs/research-phase2.md` §1.1 记 cargo drone 110、§1.2 记 plasma rifle 重 5、§3.2 记 fluid recycler 110、§4.3 记 plasma rifle 12/1、energy blade 10/2、disruptor stun/15、§4.4 记 kinetic armour 85 —— **全部与本次核对一致**。

### 4.4 `leaveItAtHome` 对 Phase 3 物品的行为

`world.js:1020-1024`：
```js
leaveItAtHome: thing =>
  thing != 'cured meat' && thing != 'bullets' && thing != 'energy cell' &&
  thing != 'charm' && thing != 'medicine' && thing != 'stim' && thing != 'hypo' &&
  typeof World.Weapons[thing] == 'undefined' && typeof Room.Craftables[thing] == 'undefined';
```

| 物品 | 回村后 | 原因 |
|---|---|---|
| `hypo` / `stim` | **留背包** | 显式白名单 |
| `plasma rifle` / `energy blade` / `disruptor` | **留背包** | 在 `World.Weapons` 里 |
| `energy cell` | **留背包** | 显式白名单 |
| `alien alloy` | 留家 | 不在任何白名单（**这是对的**——它要留在 stores 供星舰页/制造机消费） |
| 六种 `* blueprint` | 不适用 | `redeemBlueprints()` 在 `returnOutfit()` **之前**就 `delete` 掉了 |
| `fleet beacon` | 留家 | 不在白名单；且 `Score` 读的是 `stores` |
| `glowstone` | 留家 | 不在白名单，也不是 Room.Craftables |
| `kinetic armour` / `cargo drone` / `fluid recycler` | 留家 | upgrade，本来也不进背包（`Path.updateOutfitting` 只渲染 `type` 为 tool/weapon 的行） |

**注意**：`Path.updateOutfitting` 的 carryable 集合是 `内置表 ∪ Room.Craftables ∪ Fabricator.Craftables`（`path.js:167`）。所以 `hypo`(tool)、`stim`(tool)、`glowstone`(tool)、`energy blade`(weapon)、`disruptor`(weapon)、`plasma rifle`(weapon) 都会自动出现在 Path 装载行里；三个 upgrade 因为 `type: 'upgrade'` 被过滤掉。

---

## §5 与 Phase 1/2 既有系统的耦合点清单

### 5.1 `stores` 新增条目

| 名称 | 来源 | 用途 |
|---|---|---|
| `alien alloy` | 已有（Phase 2 borehole/battlefield/城市掉落 + 交易站） | 星舰页与制造机的**唯一货币** |
| `energy cell` | 已有 | laser rifle / plasma rifle 弹药 |
| `hypo` | Fabricator | 战斗治疗 30 |
| `stim` | Fabricator | boost |
| `glowstone` | Fabricator | 无消费点 |
| `energy blade` / `disruptor` / `plasma rifle` | Fabricator + Executioner 掉落 | 武器 |
| `kinetic armour` / `cargo drone` / `fluid recycler` | Fabricator | upgrade |
| `fleet beacon` | Executioner BOSS | 结局分支 + 500 分 |
| 6 × `* blueprint` | Executioner | 中转物，回村即消费 |

### 5.2 `World.state` 新增字段

| 字段 | 类型 | 置位 | 消费 |
|---|---|---|---|
| `ship` | bool | `ship` setpiece onLoad | `goHome` → `Ship.init()` |
| `executioner` | bool | `executioner-intro` `7` onLoad | `goHome` → `Fabricator.init()`；`doSpace` 决定进序章还是前厅 |
| `engineering` / `medical` / `martial` | bool | 各区终点 onLoad | 前厅按钮 `available` |

### 5.3 事件引擎需要的新能力

| 能力 | 出处 | 现有 `SpScene` 是否支持 |
|---|---|---|
| 按钮 `cost` 支持 **water** / **hp** | engineering `1-3` | ✘ |
| 按钮 `available`（运行时判定） | 前厅四个入口 | ✘ |
| 按钮 `nextEvent`（跨事件跳转） | 前厅 | ✘ |
| 场景级 `specials`（周期施加 status） | 4 处 BOSS | ✘ |
| 场景级 `atHealth`（血线触发） | `medic` | ✘ |
| 场景级 `explosion`（死亡自爆） | medical `8` | ✘ |
| 战斗 status：shield / enraged / energised / venomous / meditation / boost | 全区 | ✘ |
| 按钮 `onChoose` 调 `World.setHp(max)` / `World.applyMap()` | 3 处 | 部分（现有 `SpEffect` 枚举可扩） |
| 单按钮无 leave 的"强制推进"场景 | `7-intro`/`11`/`5`/`12`/`16`/`6` 等 | ✔（`btnCount=1`） |
| 敌人复用（共享敌人表 spread） | 4 种 × 20 处 | ✔（`SetpieceEnemy` 数组可共享） |

### 5.4 与 `docs/research-phase2.md` 的不一致（勘误）

逐条核对上游后发现 **3 处**需要更正 Phase 2 文档的记载：

| # | Phase 2 文档处 | 原文 | 实际（上游 master） |
|---|---|---|---|
| 1 | §2.5 可见性 | 「`applyMap()`（Room 侧"绘制地图"道具，可后置）」 | **Room 侧没有这个道具**。全仓 grep `applyMap` 只有两处：定义在 `world.js:687`，唯一调用点是 `executioner.js:1349`（军事区 `7-1` 的 `scavenge maps`，一次调 3 遍）。它是 **Phase 3 内容**，不是 Room 道具。且该函数写的是持久 `game.world.mask` 而非 `World.state.mask`（§3.7 的 bug）。 |
| 2 | §4.3 武器表 | 表中未列 `disruptor` 的 `cost` 列为空、`energy blade` 未标"不在 Weight 表" | 数值本身全对；补充：`disruptor` 与 `energy blade` **均无 cost**（disruptor 是无消耗的 stun 武器，比 bolas 强），且两者都**不在 `Path.Weight`** ⇒ 重量按默认 1 计（对比 plasma rifle 重 5）。 |
| 3 | §5 setpiece 摘要表 | 「坠毁星舰 ship … `salvage` 一步」的标题列 | setpiece 的 `title` 是 **`A Crashed Ship`**，与地图 label `A Crashed Starship`、星舰页标题 `An Old Starship` 是三个不同的串，glyph 闭包时不能合并。 |

此外补充两条 Phase 2 文档没提、但 Phase 3 会用到的事实：
- `Events.startEvent` 的事件对象支持 `audio` 字段（Executioner 五个事件全用 `LANDMARK_CRASHED_SHIP`）。
- `Events.switchEvent`（`events.js`）会 `eventStack.shift()` 后重新 `startEvent`——即前厅→区是**替换**而非**压栈**，离开区不会回到前厅。

---

## §6 文案 / zh_cn 覆盖度

对 `lang/zh_cn/strings.po`（793 msgid）做了逐条比对（提取各文件里所有 `_('...')` 字面量后查表）：

| 文件 | `_()` 调用数 | 去重后 | 已译 | **未译** |
|---|---|---|---|---|
| `ship.js` | 15 | 12 | **12** | 0 |
| `space.js` | 14 | 14 | 13 | **1**（`wait`） |
| `fabricator.js` | 24 | 23 | **0** | **23** |
| `executioner.js` | 375 | 191 | 3 | **188** |
| 合计 | 428 | 240 | 28 | **212** |

**结论：Ship + Space 几乎全译（2013 年的老内容），Fabricator + Executioner 零覆盖（后加的扩展内容）。** 这与 Phase 2 的"覆盖近乎 100%"是**完全不同的局面**——Phase 3 需要人工补译 **212 条**，其中 188 条是 Executioner 的叙事文本（多为整句场景描述，平均 15–25 词）。

### 6.1 已译的 Ship / Space 串（可直接用）

| 英文 key | zh_cn |
|---|---|
| `Ship` | 飞船 |
| `An Old Starship` | 破旧星舰 |
| `hull:` / `hull: ` | 外壳: / 船身:（**两个不同 key，译文也不同**） |
| `engine:` | 引擎: |
| `reinforce hull` | 加固船身 |
| `upgrade engine` | 升级引擎 |
| `lift off` | 点火起飞 |
| `linger` | 裹足徘徊 |
| `Ready to Leave?` | 准备好要离开了吗? |
| `time to get out of this place. won't be coming back.` | 是时候离开这里了。不再回头 |
| `somewhere above the debris cloud, the wanderer fleet hovers. been on this rock too long.` | 碎片云上方的某处，流浪者舰队徘徊着。在这块岩石上太久了。 |
| `not enough alien alloy` | 外星合金不足 |
| `Troposphere` / `Stratosphere` / `Mesosphere` / `Thermosphere` / `Exosphere` / `Space` | 对流层 / 平流层 / 中气层 / 增温层 / 外逸层 / 太空 |
| `score for this game: {0}` | 游戏得分: {0} |
| `total score: {0}` | 总分: {0} |
| `restart.` | 重启 |
| `expanded story. alternate ending. behind the scenes commentary. get the app.` | 尾声 游戏结束 评论 应用商店（**这条是外链广告，移植时应删**） |
| `alien alloy` / `energy cell` | 外星合金 / 能量元件 |

### 6.2 需要补译的清单（分组）

**A. Fabricator 全量（23 条）**：`Fabricator` `A Whirring Fabricator` `fabricate:` `blueprints` + 9 个成品名（`energy blade` `fluid recycler` `cargo drone` `kinetic armour` `disruptor` `hypo` `stim` `plasma rifle` `glow stone`）+ 9 条 buildMsg + `the familiar hum of wanderer machinery coming to life. finally, real tools.`

**B. Executioner 区/事件标题（5 条）**：`A Ravaged Battleship` `Engineering Wing` `Martial Wing` `Medical Wing` `Command Deck`

**C. Executioner 敌人名（约 12 条）**：`mechanical guard` `mechanical quadruped` `broken medic` `defence turret` `chitinous horror` `chitinous queen` `operative` `researcher` `ancient beast` `automated turret` `unruly welder` `unstable prototype` `murderous robot` `unstable automaton` `malformed experiment` `immortal wanderer`

**D. Executioner 按钮（约 18 条）**：`power cycle` `take device and leave` `engineering` `medical` `martial` `command deck` `use machine` `scavenge maps` `blow it down` `continue right` `extinguish` `rush through` `force locker` `approach` `observe` `engage` `fight`（`continue` / `enter` / `leave` 已有）

**E. Executioner 叙事文本（约 130 条）**：各场景 `text[]` 与 `notification`，长句为主。

**F. 战斗新增 UI（7 条）**：`use hypo` `boost` `shield` `disintegrate` `slice` `stun`（作为 verb）`kinetic`（护甲名）

**G. Space 缺口（1 条）**：`wait`（扩展结局的按钮）

**H. 硬编码未走 `_()`（3 条，需自行决定）**：`showExpansionEnding()` 的三段字幕 + `redeemBlueprints` 的通知 `blueprints feed into the fabricator data port. possibilities grow.`（`world.js:1007`，**上游漏了 `_()` 包裹**）

> **glyph 闭包影响**：212 条新译文按平均 20 字估算约 4200 个中文字符，去重后预计新增 400–700 个不重复汉字。`src/cjk_font12.h` 目前来自 797 条 / 6669 中文字符 / 19679 字节的闭包；Phase 3 后规模大约 **翻倍**。需要在 §11 的分期里给字体重生成留出验证窗口（proof.png 目检 + flash 占用核对）。

---

# 下半部：PaperDarkRoom 适配设计

## §7 硬件与显示前提（既定事实，不再论证）

固件已在 `exp/msg-1bpp` 迁移到自研 MSG 驱动（`lib/msg/`）。以下数值直接引用，作为本章所有设计的输入：

| 事实 | 数值 | 出处 |
|---|---|---|
| 扫描频率 | **43.4 Hz / 23.05 ms 一帧**（esp_lcd 传输路径实测，bench 2026-07-29，n=37） | `lib/msg/src/msg.c:345-353` |
| 像素格式 | **1bpp**，960×540 landscape 帧缓冲，64800 B，MSB = 最左，置位 = 黑 | `lib/msg/src/msg.h:98-110` |
| UI 坐标系 | **540×960 竖屏**，sprite 绑 rotation 3，零拷贝转置；触摸坐标同系 | `src/msg_bridge.h:59-60`，`src/msg_bridge.cpp:24-31` |
| 驱动帧数 | `MSG_DRIVE_FRAMES = 7` ⇒ 一次完整黑白翻转需 **7 × 23.05 ≈ 161 ms**，自带拖尾 | `lib/msg/src/msg_kernel.h:135-146` |
| 提交延迟 | `msg_flip()` 返回后，该帧**再过一个扫描周期（≈23 ms）才上玻璃** | `lib/msg/src/msg.h:174-186` |
| 整帧渲染 | 一次完整 UI 渲染 ≈ **8 ms**（spike 分段实测；源码注释写作 `~8 ms`） | `src/msg_bridge.h:40-43`，`src/msg_spike/spike.cpp:151-155` |
| 局部刷新 | **不存在**。双缓冲 + free-running 扫描 ⇒ 每次都是整帧 540×960 | `src/page.h:24-30` |
| 单帧可辨最短停留 | **≈60 ms**（`FLASH_MS = 120` 的推导依据） | `src/pager.cpp:39-43` |
| 清残影 | image-mode `pushImage`，**8 场白 + 8 场原图 ≈ 400 ms**，必须成对 | `src/pager.cpp:295-323`，`msg.h:74` |
| 触摸 | GT911 @ I2C1 400 kHz，5 点，长按 500 ms，移动阈值 30 px，无固定采样率（跟 app loop） | `src/touch_gt911.cpp:24-36,70-79` |
| 面板写者 | `msg_flip()` 只有**一个等待者槽**，第二个调用者会永久阻塞 | `msg.c:731,1310-1320`，`msg_bridge.cpp:155-189` |
| 任务拓扑 | scan 任务 core 1 pri 1；app 任务 `"adr"` core 0 pri 1 stack 16 KB；Arduino `loop()` 已 suspend | `msg.c:2302`，`main.cpp:782,788` |
| app loop 节拍 | `delay(5)` + 泵，空闲 ≈150–200 Hz；一旦触发 `present()` 该趟变 ≈23 ms | `src/main.cpp:565-739` |
| **governor park 陷阱** | 画面完全静止时扫描停车，`present()` **不再阻塞** ⇒ 渲染循环必须自带节拍器 | `msg.c:1644-1650`，`msg.h:196-200` |
| 现成动画骨架 | `bindFrame → render → msg_flip()`，8 px/帧弹跳方块 | `src/msg_spike/spike.cpp:274-296` |
| 现成躲陨石基线 | 飞船 48×36 实心矩形 + 12 颗实心圆陨石（M5GFX 对照组） | `src/bench/bench.cpp:29-36` |

**两个必须先处理的驱动缺口**（否则 §8 无法落地）：

1. **`env:adarkroom` 没有 `-O2`**。只有 `[env:msg-spike]` 有 `build_unflags = -Os` + `-O2`；主固件的 `msg.c` 以 `-Os` 编译，而 43.4 Hz 是 `-O2` 下测出来的。上太空关之前必须先复测主 env 的实际 fps（串口 STATS 行），或把 `-O2` 加进主 env。
2. **触摸没有"仍按下"语义**。`touch::Detail` 只有 `clicked` / `held` 两个**边沿**位（各只为一个 pass 为真），`count()` 又把本 pass 释放的触点混算进来。内部 `s_liveN` 已有全部信息（`touch_gt911.cpp:88-97,340-343`），需要给 `Detail` 加一个 `bool down`（或暴露 `liveCount()`），改动约 3–4 行。**这是 §8.3 操船方案的硬前提。**

---

## §8 太空玩法在墨水屏上的重设计

### 8.1 为什么不能照抄

| 上游做法 | 在 MSG 上的后果 |
|---|---|
| `setInterval(moveShip, 33)` = 30 fps | 43.4 Hz 扫描 + 161 ms 驱动 ⇒ 30 fps 的相邻帧互相打断驱动，全屏变灰糊 |
| 陨石是 32px 旋转 ASCII 字形（`# $ % & H`） | 1bpp 点阵字体放大后是**线条**，线条在 161 ms 拖尾下退化成不可读的灰雾 |
| 末段同屏 15 颗/秒、瞬时 15 颗存活 | 每颗每帧都在改变一片 48×48 的像素 ⇒ 全屏 active_bytes 常驻高位，对比度崩 |
| 键盘 8 向 + 按住持续移动 | 只有触摸；且 ~200 ms 的端到端延迟下速度控制必然过冲振荡（§8.3） |
| 60 秒 = 一次性通关判定 | 保留，但难度曲线要按新帧率重标定 |

**保留不变的三件事**：60 秒时长、hull 是唯一资源、crash 只罚 120 秒冷却（§2.7）。这三条决定了太空关的"低压力重试"性格，是 e-ink 上唯一能玩的动作玩法定位。

### 8.2 10 fps 逻辑帧的实现骨架

**节拍选择**：逻辑帧锁到 **4 个扫描帧 = 92.2 ms ≈ 10.85 fps**（任务书的"每逻辑帧 flip 4~5 个扫描帧"即此）。60 秒 ≈ **652 逻辑帧**。

为什么是 4 而不是 5：5 帧 = 115.3 ms ⇒ 8.7 fps，陨石为保持"相邻帧圆相接"每帧要走 48 px（§8.4），换算成 416 px/s，穿越 704 px 场地只剩 1.7 s，反应窗口太窄。4 帧是"帧率够高 / 每帧位移够小 / 拖尾不断裂"三者的交点。

**每逻辑帧只调用一次 `present()`**。其余 3 个扫描帧里面板在**重复驱动同一内容**（这正是 `MSG_DRIVE_FRAMES=7` 需要的），不需要也不应该额外 flip。

```cpp
// src/space_page.cpp（新增），完全照 spike.cpp / *_modal 的契约
namespace space_game {

constexpr uint32_t FRAME_MS = 92;    // 4 * 23.05ms，与扫描周期整数对齐

// pager::drawFrame() 里的分支（放在 event/fight/setpiece 三个 modal 之后、页面之前）
bool active();
void renderFrame();                  // 整帧 540x960，不 flip

void run() {                         // 由 world/ship 页触发，独占 app 任务直到结束
  reset();
  uint32_t t0 = millis();
  while (!s_done) {
    uint32_t frameStart = millis();

    touch::update(frameStart);       // 不走 pager::handleTouch()，见 §8.3
    stepInput(frameStart);
    stepWorld(frameStart - t0);      // altitude / 生成 / 移动 / 碰撞
    renderFrame();                   // fillSprite + HUD + 飞船 + 陨石
    msg_bridge::present();           // 阻塞 0..23ms 到 VSYNC

    // ★ 自带节拍器：governor 停车时 present() 不阻塞，只靠它兜不住
    while ((int32_t)(millis() - frameStart) < (int32_t)FRAME_MS) delay(2);

    beeper::tick(millis());
  }
}
} // namespace
```

**三条必须遵守的约束**（来自 §7）：
1. **单写者**：`run()` 跑在 app 任务里，期间不得有任何其他代码调 `present()`。它是个阻塞式子循环，不是 `appLoop()` 的一个分支——这与 `pager::flashPressRect()` 里 `delay(FLASH_MS)` 的做法同源。
2. **不能省渲染**：每帧都是整帧。`fillSprite(TFT_WHITE)` 是一次 64800 B PSRAM memset（spike 实测里 `s_usClear` 那一段），加上 ≤16 个 `fillCircle` + HUD，预算约 5–8 ms，占 92 ms 的 8%。
3. **不能靠 `present()` 定速**：governor 在 `active_bytes == 0` 时停车。实测下太空关里像素持续在动（161 ms 驱动 > 92 ms 帧间隔）所以不会停车，但**crash 定格画面、结算画面、暂停**这三种静止状态一定会停车，`while (millis() - frameStart < FRAME_MS)` 是唯一可靠的兜底。

**端到端延迟预算**（决定 §8.3 的方案选择）：

| 环节 | 耗时 |
|---|---|
| 手指动 → 本逻辑帧采样到 | 0 – 92 ms |
| 逻辑 → `msg_flip()` 接受 | 0 – 23 ms |
| flip → 上玻璃 | +23 ms |
| 上玻璃 → 肉眼可辨（约 2–3 个驱动帧） | +46 – 70 ms |
| **合计** | **≈ 70 – 210 ms，保守取 200 ms** |

### 8.3 触摸操船方案

**候选 A：按住屏幕左/右半横移（速度控制）**
- 优点：最接近上游键盘手感；实现最简单（只需 `down` 位 + 触点 x）。
- **致命缺陷**：速度控制在 200 ms 延迟下必然过冲。玩家看到飞船开始动的时候，实际已经多移动了 `v × 0.2 s`；以 §8.4 的 v=420 px/s 计是 **84 px**，接近飞船宽度（48）的两倍。想停在某一列必然要"点—看—反向点"来回振荡，这正是所有低刷新率设备上速度控制失败的典型症状。
- 另有一个具体问题：`pager::handleTouch()` 会丢弃边缘 24 px（`pager.cpp:611-616`），左右两端的控制格被削掉，靠边躲避直接失效。

**候选 B：手指跟随（绝对位置控制）**
- 优点：绝对映射**对延迟免疫**——延迟只影响"跟手感"，不影响"能不能停在目标列"，因为目标位置由手指的绝对位置直接决定，没有积分误差。
- 缺陷：手指遮挡飞船与其正上方的判定区，而这恰好是玩家最需要看的区域。

**推荐：候选 B 的改良版 —— 底部绝对映射控制带（1D）**

```
飞船 y 固定；x 由控制带内手指的 x 直接决定（1:1 绝对映射）。
控制带位于 y ∈ [800, 960)，高 160 px，横跨全宽 540。
手指在控制带内任意 y 落点均有效；抬起后飞船保持在原位（不回中）。
```

理由（按重要性排序）：
1. **对 200 ms 延迟免疫**（同候选 B），这是唯一能让这个玩法在 43.4 Hz 上成立的性质。
2. **手指不遮挡战场**：控制带在飞船下方 160 px 处，playfield 完全裸露。
3. **1:1 映射消除标定问题**：控制带宽度 = 屏宽 = 飞船可达范围，玩家一眼就知道"手指在这，船就在这"。不需要教学。
4. **降到 1D 是有意的**：上游的纵向移动收益本来就很低（陨石从上往下、飞船在下方），去掉 Y 轴换来一半的输入精度需求和一半的碰撞检测成本；同时让"控制带"这个隐喻成立。2D 版本列为待定（§12 Q2）。
5. **必须绕开 `pager::handleTouch()`**：太空关自己在 `run()` 里直接调 `touch::update()` + `touch::detail(0)`，不走 pager 的边缘带丢弃 / 350 ms 去抖 / 三指抓握闩锁。这与三个 modal 在 `handleTouch()` 最前面拦截并 `return` 的既有模式一致（`pager.cpp:484-491` 等）。

**输入代码骨架**（依赖 §7 缺口 2 的 `down` 位）：

```cpp
static void stepInput(uint32_t nowMs) {
  if (touch::count() > 0) {
    touch::Detail d = touch::detail(0);
    if (d.down && d.y >= CTRL_TOP) {          // CTRL_TOP = 800
      int want = d.x;                          // 1:1 绝对映射
      // 单帧位移上限，防止"瞬移"读作闪烁（见 §8.4 速度上限同理）
      int dx = want - s_shipX;
      if (dx >  SHIP_MAX_STEP) dx =  SHIP_MAX_STEP;   // 64 px/帧 ≈ 695 px/s
      if (dx < -SHIP_MAX_STEP) dx = -SHIP_MAX_STEP;
      s_shipX += dx;
      s_shipX = clamp(s_shipX, SHIP_W/2, UI_W - SHIP_W/2);
    }
  }
  // 抬起：保持原位，不回中，不减速
}
```

**thrusters 怎么进模型**：上游 `getSpeed() = 3 + thrusters` 是速度控制的参数，在绝对映射下没有对应物。改为**单帧位移上限 `SHIP_MAX_STEP`**：

| thrusters | `SHIP_MAX_STEP`（px/逻辑帧） | 等效横移速度 |
|---|---|---|
| 1（初始） | 40 | 434 px/s |
| 2 | 48 | 521 px/s |
| 3 | 56 | 608 px/s |
| 4 | 64 | 695 px/s |
| ≥5 | 72（封顶 = 手指瞬移即到） | 781 px/s |

公式：`SHIP_MAX_STEP = min(72, 32 + 8 × thrusters)`。语义完全保住："引擎越好，飞船越跟得上手指"。thrusters ≥5 后升级无收益——**这是对上游"引擎无上限"的一个有意收敛**，值得在 §12 Q11 讨论是否要额外给点别的收益。

### 8.4 陨石渲染：拖影下的可读性

**决策 1：字形 → 实心几何体。** 上游的 `# $ % & H` 在 12px 点阵字体 ×3 放大后是稀疏线条，161 ms 拖尾会把线条之间的白缝填成灰，整体退化为一团灰雾。改用**实心圆**（`canvas.fillCircle`），与 `src/bench/bench.cpp:29-36` 已验证过的基线一致。视觉多样性靠**三档直径**（40 / 48 / 56 px，概率 0.35 / 0.45 / 0.20）而非字形差异。

**决策 2：尺寸下限 48 px（直径）。**
- 12px 字体的一个汉字是 12 px；48 px = 4 个字高，在 540 宽的屏上一眼可辨。
- 拖尾把圆在运动方向上"抹长"约 1.75 个逻辑帧的位移（161 ms / 92 ms）。40 px 圆以 24 px/帧移动时，视觉包络是 40 × (40+42) = 40×82 的竖长条，仍然读作"一个下落的东西"。再小（<40）就读作"一条竖线"。
- **最小直径 40 px 是硬下限，标称 48 px。**

**决策 3：速度上下限来自"相邻帧圆是否相接"。**

设直径 D、每逻辑帧位移 s：
- `s < D/2` ⇒ 相邻帧重叠 >50%，拖影糊成一条几乎不动的粗带，**读作静止的污渍**；
- `D/2 ≤ s ≤ D` ⇒ 相邻帧的圆重叠或相切，拖影是连续的一条，**读作运动**；这是目标区间；
- `s > D` ⇒ 相邻帧的圆完全断开，**读作频闪**，同时超出玩家 200 ms 延迟下的可预判量。

以标称 D = 48 px：

| 项 | 值 |
|---|---|
| **速度下限** | 24 px/逻辑帧 = **260 px/s**（穿越 704 px 场地 2.7 s） |
| **速度上限** | 48 px/逻辑帧 = **521 px/s**（穿越 1.35 s） |
| 上游对应值 | 520 – 1486 px/s（§2.4）——**上游的上限在我们这里是下限**，快陨石必须整体减速到 1/3 |
| 200 ms 延迟内陨石下落 | 52 – 104 px，占场地 7% – 15%，可预判 |

**决策 4：难度曲线重标定。** 上游的 15 颗/秒（§2.4）在 92 ms 帧、48 px 圆、540 px 宽的场地上是灾难（同屏 20+ 颗会把 playfield 涂黑）。重标定为：

| altitude (km) | 每波颗数 | 波间隔（逻辑帧 / ms） | 颗/秒 | 同屏峰值 |
|---|---|---|---|---|
| 0 – 10 | 1 | 10 / 920 | 1.1 | ~2 |
| 11 – 20 | 1 | 7 / 644 | 1.6 | ~3 |
| 21 – 30 | 2 | 7 / 644 | 3.1 | ~6 |
| 31 – 45 | 2 | 5 / 460 | 4.3 | ~9 |
| 46 – 60 | 3 | 5 / 460 | 6.5 | ~14 |

**同屏硬上限 16 颗**（超出则跳过本次生成）——渲染预算与 `bench.cpp` 的 12 颗基线同量级，且保证 playfield 黑占比 < 25%。

陨石个体速度在生成时从 `[24, 48] px/帧` 均匀抽取（对应上游 `1500 - rand*975` 的随机时长）。

**决策 5：碰撞用 AABB，不用上游的点对盒。** 上游把玩家当成一个点（`shipX/shipY` 是 CSS left/top，还被 margin 偏移过），是明显的实现妥协。我们用飞船 48×36 的矩形 vs 陨石的外接正方形：

```cpp
bool hit = !(a.x + a.r < ship.x - SHIP_W/2 || a.x - a.r > ship.x + SHIP_W/2 ||
             a.y + a.r < ship.y - SHIP_H/2 || a.y - a.r > ship.y + SHIP_H/2);
```
命中后移除该陨石、`hull--`、`beeper::tone(...)`、`hull == 0 → crash()`。与上游一致：**一颗陨石只能命中一次**。

**命中反馈**（替代上游的 8 个 `ASTEROID_HIT_*` 音效）：
- 音：`beeper::tone(freq, 60)`，`freq` 按 altitude 三档 —— `≤20 km: 880 Hz`、`21–40: 1200 Hz`、`>40: 1600 Hz`（复刻上游"越高音越高"的意图，但只用 3 档而非 8 个采样）。
- 视：命中当帧把**飞船矩形整块反色**（在白底上就是画白 + 黑描边）持续 2 个逻辑帧（184 ms > 60 ms 可辨门槛）。不做全屏闪，全屏闪的残影债太贵（§9）。

### 8.5 HUD 布局（540 × 960）

太空关是**全屏 modal**，自己拥有整帧（照 `fight_modal::renderFrame()` 的契约，不画 `status_bar`）。

```
y    0 ─────────────────────────────────────────────  ┐
        [hull pips]           层名(scale 3, 36px)      │ HUD 84px
        24×24 方块×N          居中                     │
y   84 ═══════════════════════════════════════════════╡ 2px 实线
                                                       │
                    playfield                          │ 704px
                    540 × 704                          │
        陨石(实心圆) 自 y=84 生成，落到 y>788 移除       │
                                                       │
        飞船 48×36，y 固定 = 740（底部上方 48px）       │
y  788 ═══════════════════════════════════════════════╡ 2px 实线
y  792 ┌───────────────────────────────────────────┐  ┐
        │        控制带 (160px)                     │  │
        │  刻度线 ×11（每 54px 一格，12px 高短线）   │  │ 168px
        │  当前 x 的 caret：↑ 形三角，24px 宽        │  │
y  960 └───────────────────────────────────────────┘  ┘
```

**右侧高度计**：playfield 右缘内侧 12 px 宽、y ∈ [96, 776] 的竖条，实心部分从底部往上长，60 秒走满 680 px ≈ **11.3 px/s**。每逻辑帧只增长约 1 px，远低于拖影阈值，**不会造成额外残影**。层边界（10/20/30/45 km）画 3 px 的横刻度。

**hull 显示**：**离散方块，不是数字**。每点 hull 一个 24×24 实心方块，间距 8 px，从 x=16 起排。最多显示 12 个（12×32 = 384 px，留出层名空间）；hull > 12 时显示 12 个方块 + `+N`（scale 2）。
- 理由：数字 `3/5` 变化时只有 1–2 个字形的像素在动，在 161 ms 拖尾下"3 变 2"会有小半秒是两个数字叠着的糊团；方块是**整块消失**，语义在拖尾期间也不会读错。
- 掉血时被移除的那个方块位置画 3 帧的 `drawRect`（空心框）作为"刚失去"的提示，然后彻底消失。

**层名**：`tr("Troposphere")` 等 6 个已译串（§6.1），scale 3 = 36 px 居中。**按阈值切换，不复刻上游的 `%10` bug**（§2.6 / §12 Q4）。

### 8.6 crash / 胜利演出

**crash（`hull == 0`）**：

| 步 | 时长 | 内容 |
|---|---|---|
| 1 | 1 逻辑帧 | 飞船位置画一个 96×96 的实心方块（"爆开"）；`beeper::tone(300, 240)`（沿用 `world_page.cpp:355` 的死亡音） |
| 2 | 3 逻辑帧（276 ms） | **playfield 整块填黑**，保持。这是全局最大的一次像素翻转，也正好是后面 deghost 的理由 |
| 3 | 1 帧 | 全屏白 + 居中 `tr("...")` 坠毁文案（scale 3） |
| 4 | — | **`pager::deghost()`**（8 场白 + 8 场原图 ≈ 400 ms）。此刻玩家在读文案，无感 |
| 5 | — | 退出 `run()`，回星舰页；起 120 秒起飞冷却（与上游一致） |

**胜利（60 秒到）**：

| 步 | 时长 | 内容 |
|---|---|---|
| 1 | 1 帧 | 停止生成陨石；已在屏的继续下落 |
| 2 | 3 帧（276 ms） | 飞船每帧上移 240 px，第 3 帧飞出屏幕上沿 |
| 3 | 8 帧（736 ms） | 全白，剩余陨石继续落完并移除 |
| 4 | — | **`pager::deghost()`** |
| 5 | 静态页 | 结算页：`score for this game: {0}` / `total score: {0}` + 一个 `restart.` 按钮（走 `action_band::draw`）。**删掉上游的 iOS/android 外链两行** |

结算页是静态页面，走正常的 `pages::Page` 路径（不再有游戏循环），残影问题不存在。

---

## §9 残影治理节奏

### 9.1 两种"清屏"必须分清

| 手段 | 机制 | 代价 | 能否局内用 |
|---|---|---|---|
| **image-mode 白推**（`pushImage(白, true)` + `pushImage(原图, false)`） | 走固定 LUT 的 8 场波形，**绕过 video 状态模型** | ≈ 8×23 + 8×23 ≈ **400 ms**，且必须成对 | **绝对不能**。400 ms = 4 个逻辑帧全丢，等于送一次碰撞 |
| **video-mode 全白帧**（`canvas.fillSprite(TFT_WHITE)` + `present()`） | 就是一个普通的 video 帧，走正常状态模型 | **1 次 flip（≤23 ms）**，之后 161 ms 内所有黑像素被驱向白 | **可以**，而且应该 |

这个区分是本节的核心：**局内的"洗屏"用 video 全白帧，局外的"还债"用 image 白推。** `pager::deghost()`（`pager.cpp:295-323`）属于后者，注释里已经写死了"必须成对、必须以 video 相信的那一帧结束"的铁律，不能拆开用。

### 9.2 太空关的残影债账本

一局 60 秒 = 652 次 `present()`。`pager` 现有的记账（`QUALITY_EVERY = 8`、`HIGH_WATER = 24`，`pager.cpp:35-37`）会被瞬间打爆，而在局内还债又不可接受。因此：

**太空关期间冻结 `pager` 的债务计数器**，改用一套独立规则：

| 时机 | 手段 | 理由 |
|---|---|---|
| **进入太空关之前** | `pager::deghost()`（400 ms） | 从静态 UI 页切到全屏游戏是整局最大的一次内容替换，在这里还清历史债，让游戏从"干净玻璃"开始 |
| **局内：层过场 ×5** | **video 全白帧，保持 3 逻辑帧（276 ms）** | 见 §9.3 |
| **局内：其余时刻** | 什么都不做 | playfield 里的像素本来就在持续翻转（161 ms 驱动 > 92 ms 帧间隔），扫描从不停车，对比度靠不断的重驱动维持；插任何额外操作只会打断驱动 |
| **crash / 胜利之后** | `pager::deghost()`（400 ms） | 玩家在读文案/分数，无感；把整局累积的债一次还清 |
| **回到 UI 页** | `s_fastCount = 0` | 由 `deghost()` 自动完成 |

一局的残影开销：**进场 400 ms + 层过场 5 × 276 ms（计入游戏时长，见下）+ 出场 400 ms ≈ 2.2 s**，全部在玩家不操作的时刻。

### 9.3 层过场：把清洗做成演出

上游没有"层过场"这个东西（`setTitle` 只是改浏览器标题）。我们**主动造一个**，一举三得：

```
altitude 跨过 10 / 20 / 30 / 45 / 60 km 时：
  1. 暂停陨石生成；暂停高度计时（★ 保证 60 秒有效游戏时长不被过场吃掉）
  2. playfield 整块填白（video 全白帧），保持 3 逻辑帧 = 276 ms
     — 276 ms > 161 ms 驱动时间，白得干净
  3. 白底上居中画新层名（scale 3，36px），同时画一条 4px 横线扫过（3 帧内从 y=84 扫到 y=788）
  4. 恢复：陨石继续下落（过场期间它们的位置照常推进，只是没画出来），计时恢复
```

三个收益：
1. **残影卫生**：playfield 里所有黑像素被驱向白，等于每 10 km 做一次局部软洗。
2. **节奏**：给玩家 276 ms 的呼吸，也是"我升到下一层了"的正反馈——上游完全没有这个反馈（标题栏在游戏里根本看不见）。
3. **难度换挡的预告**：每次过场之后陨石密度都会提升一档（§8.4），过场就是"变难了"的信号。

**代价**：5 次 × 276 ms = 1.38 s 的额外时长（因为高度计时暂停，实际游戏总时长变成 61.4 s）。可接受。

### 9.4 什么时候**不能**清

- **不能在 `crash()` 的第 2 步（playfield 填黑）之前 deghost**：填黑那一帧是故意制造的高对比度，deghost 会把它洗掉。
- **不能在 `pushImage` 之间插任何 video 帧**：`pager.cpp:295-323` 的注释明说了，image mode 不更新 video 状态模型，序列必须以"video 相信的那一帧"收尾，中间插帧会让后续所有 video 帧画错。
- **不能在游戏循环里调 `pager::repaint()`**：它会 `drawFrame()` + `present()`，与游戏循环抢 `msg_flip()` 的单等待者槽（§7），第二个调用者永久阻塞。

---

## §10 Ship / Executioner / Fabricator → 现有模块映射

### 10.1 页面层

| 上游概念 | 落到现有模块 | 具体改动 |
|---|---|---|
| `Ship` 模块（`An Old Starship` tab） | 新增 `ShipPage : pages::Page`，`name() = "ship"` | 照 `TechPage`（`src/tech_page.h`）的模式：`open()/close()/isOpen()` 命名空间闩锁 + `available()` 门 |
| `Fabricator` 模块（`A Whirring Fabricator` tab） | 新增 `FabricatorPage : pages::Page`，`name() = "fabricator"` | 同上 |
| `Space` 模块 | **不是页面**，是全屏 modal + 阻塞子循环 | 新增 `src/space_page.cpp`，接口照 `fight_modal.h:31-37` 的 `active()` / `renderFrame()` 契约（§8.2） |
| 页面 ring | `src/client_pages.cpp:53-54` 的 `s_reg[]` | 追加到**末尾**：`{ room, outside, trade, assign, path, world, tech, fabricator, ship }`。ring 索引对既有页不变；`meta.json` 存的是 `curName` 不是索引（`frame_store.cpp:64-105`），所以顺序调整是安全的 |
| `Header.addLocation(..., 'ship')` 的插入语义 | `s_reg[]` 里 fabricator 排在 ship 前面即可 | — |
| tab 标题栏 | `src/page_tabs.cpp:69-125` | Ship/Fabricator **不进 tab 栏**（tab 栏只有 Room/Outside/Trade 三个硬编码槽 `TabSlot s_slots[3]`），靠翻页到达；页内自绘 36px 标题 |
| `Ship.onArrival` 的一次性通知 | `GameState` 的 `seen` 位集（`game_state.cpp:693-694`） | 加 2 位：`SEEN_SHIP`、`SEEN_FABRICATOR` |
| 结局/结算页 | 新增 `EndingPage` 或复用 `space_page` 的静态终态 | 见 §12 Q1（是否删档） |

### 10.2 数据表扩展

**`Res` 枚举**（`src/game_data.h:59-68`，现 19 项）新增 4 项：

| 新增 | key | 用途 |
|---|---|---|
| `R_HYPO` | `"hypo"` | 战斗治疗 30 |
| `R_STIM` | `"stim"` | boost |
| `R_FLEET_BEACON` | `"fleet beacon"` | 结局分支 + 500 分 |
| `R_GLOWSTONE` | `"glow stone"` | **可选**，上游无消费点（§12 Q8） |

**`Item` 枚举**（`src/game_data.h:93-104`，现 18 项）新增 6 项：

| 新增 | key | 类别 |
|---|---|---|
| `I_PLASMA_RIFLE` | `"plasma rifle"` | 武器（重 5，已在 `WEIGHTS` 表 `world_data.h:185`） |
| `I_ENERGY_BLADE` | `"energy blade"` | 武器（重 1） |
| `I_DISRUPTOR` | `"disruptor"` | 武器（重 1） |
| `I_KINETIC_ARMOUR` | `"kinetic armour"` | upgrade（`HEALTH_KINETIC=85` 已在 `world_data.h:220`） |
| `I_CARGO_DRONE` | `"cargo drone"` | upgrade（`BAG_CARGO_DRONE=11000` 已在 `world_data.h:206`） |
| `I_FLUID_RECYCLER` | `"fluid recycler"` | upgrade（`WATER_RECYCLER=110` 已在 `world_data.h:213`） |

> **Phase 2 已经把三个 upgrade 的数值常量埋好了**，只差 `Item` 槽位和读取它们的判定分支（`WorldState::maxWater/maxHealth/bagCapacityCenti`，`world_state.h:287-289`）。

**武器表**（`src/combat_data.h:75-90`）新增 3 项，接在 `WEAPON_BOLAS` 之后：

| WeaponId | key | verb | damage | cooldownS | ammoRes |
|---|---|---|---|---|---|
| `WEAPON_PLASMA_RIFLE` | plasma rifle | `disintegrate` | 12 | 1 | `R_ENERGY_CELL` |
| `WEAPON_ENERGY_BLADE` | energy blade | `slice` | 10 | 2 | 无 |
| `WEAPON_DISRUPTOR` | disruptor | `stun` | `DMG_STUN` | 15 | 无 |

**蓝图**：**不新增 6 个 `Res` 槽**，改用位掩码（省 6 个数组槽位、省 `savedOutfitRes` 膨胀）：
- `Expedition` 加 `uint8_t bpFound;`（6 位）——远征期间捡到的蓝图；
- `GameState` 加 `uint8_t blueprints;`（6 位）——已兑换的蓝图，照 `perks` 位集的写法序列化（`game_state.cpp:693-694`）；
- `LootDrop` 加一种 kind：现有 `bool isItem` 扩成 `uint8_t kind { LK_RES, LK_ITEM, LK_BLUEPRINT }`，`slot` 复用为蓝图索引 0..5；
- `goHome()` 里做 `redeemBlueprints`：`g_game.blueprints |= ex.bpFound;`（对应 `world.js:989-1009`）。

**Fabricator 制造表**：新建独立表（不塞进 `CRAFT`，因为成本恒是 alien alloy、门是蓝图而非建筑）：
```cpp
// src/game_data.h
struct Fabricatable {
    const char* key; const char* buildMsg;
    uint8_t kind;          // FK_RES / FK_ITEM
    uint8_t slot;          // Res 或 Item 下标
    int8_t  blueprintBit;  // -1 = 无需蓝图
    int16_t maximum;       // -1 = 无上限
    int16_t alloyCost;     // 1 或 2
    int16_t quantity;      // hypo = 5，其余 1
};
static const Fabricatable FABRICATE[9] = { ... };   // §4.2 逐行照抄
```

**成本/收益/状态常量**（`src/world_data.h` / `combat_data.h`）：
```cpp
constexpr int   ALLOY_PER_HULL      = 1;
constexpr int   ALLOY_PER_THRUSTER  = 1;
constexpr int   SHIP_BASE_HULL      = 0;
constexpr int   SHIP_BASE_THRUSTERS = 1;
constexpr int   LIFTOFF_COOLDOWN_S  = 120;
constexpr int16_t HYPO_HEAL         = 30;    // 已在 world_data.h:50
constexpr int   FIGHT_HYPO_COOLDOWN_S   = 7;
constexpr int   FIGHT_STIM_COOLDOWN_S   = 10;
constexpr int   FIGHT_SHIELD_COOLDOWN_S = 10;
constexpr int   BOOST_DAMAGE   = 10;
constexpr int   BOOST_TICKS    = 3;   // 3000ms → 3 tick
constexpr int   ENRAGE_TICKS   = 4;   // 4000ms
constexpr int   MEDITATE_TICKS = 5;   // 5000ms
constexpr int   ENERGISE_MULT  = 4;
constexpr int   EXPLOSION_TICKS = 3;  // 3000ms 前摇
```

**存档**：`SAVE_VER` 3 → **4**。新增顶层扁平键（照 `game_state.cpp:675-726` 的惯例）：`"shiph"`（hull）、`"shipt"`（thrusters）、`"bp"`（blueprints 位集）；`fl` 位集加 `seenShipWarning`；`seen` 位集加 2 位。`fromJson` 的"缺字段 → 默认值"惯例保证 v3 存档可平滑升级。`Expedition` 的 cleared 打包字节（`world_state.cpp:996-997`）现用到 bit4，新增 `engineering/medical/martial` 占 bit5/6/7，`bpFound` 单独一字节，`TREK_VER` 1 → 2。

### 10.3 Setpiece 引擎扩展

Executioner 是 5 个事件、96 个场景、靠 `nextEvent` 互跳（§3.1）。**不能塞进一个 `SpDef`**。

| 上游概念 | 落点 | 改动 |
|---|---|---|
| 5 个 `Events.Executioner[*]` | **5 个 `SetpieceId`**：`SP_EXEC_INTRO` / `SP_EXEC_ANTE` / `SP_EXEC_ENG` / `SP_EXEC_MED` / `SP_EXEC_MAR` / `SP_EXEC_CMD`（6 个，含前厅） | 替换 `setpieces_data.h:545` 的空表占位；`world_data.h:132` 的 `SetpieceId` 加 5 项 |
| `nextEvent` 跨事件跳转 | `SpButton.next` 新增哨兵 `SP_SCENE_EVENT = 0xFC`，此时 `probStart` 复用为目标 `SetpieceId` | `setpiece_engine.cpp` 的 `choose()` 里加一个分支：`end()` 当前 setpiece 再 `begin(targetId)`，**保留 `WorldState` 不变** |
| 按钮 `available: fn` | `SpButton` 加 `uint8_t availCond`（枚举 `SPA_ALWAYS / SPA_NOT_ENGINEERING / SPA_NOT_MEDICAL / SPA_NOT_MARTIAL / SPA_ALL_WINGS`） | `setpiece_modal` 渲染时跳过不可用按钮 |
| 按钮 `cost: {water: 5}` / `{hp: 10}` | `SpButton` 的 `costIsItem: bool` 扩成 `uint8_t costKind { CK_NONE, CK_RES, CK_ITEM, CK_WATER, CK_HP }` | engineering `1-3` 是唯一用到 water/hp 成本的场景（§3.6） |
| `onChoose: World.setHp(max)` | `SpEffect` 加 `SPE_HEAL_FULL` | engineering `4`、martial `10` |
| `onChoose: World.applyMap()×3` | `SpEffect` 加 `SPE_REVEAL_MAP3`，且**写 `ex.revealed` 而非已提交层**（修上游 bug，§3.7） | martial `7-1` |
| 各区终点 `World.state.xxx = true` | `SpEffect` 加 `SPE_MARK_ENGINEERING / _MEDICAL / _MARTIAL / _EXEC_ENTERED` | 对应 `Expedition` 新增的 3 个 bool |
| 指挥甲板 `7` 的 `World.clearDungeon()` | 已有 `SPE_CLEAR_DUNGEON`（`setpieces_data.h:52`） | 直接用 |
| 序章 `7` 的 `drawRoad()` + `state.executioner` | 已有 `clearMine()` 的 `T_EXECUTIONER` 分支（`world_state.cpp:631`） | 接上 |
| 共享敌人表（4 种 × 20 处） | `SetpieceEnemy exec_enemies[4]`，各场景 `enemy` 字段引同一下标 | 现有 `SpDef.enemies` 是数组指针，天然支持 |
| `SP_SCENE_LOOT_MAX = 8` | 够用（Executioner 单场景最多 5 项掉落，martial `2-1`） | 无需改 |

**规模估算**：6 个 setpiece、96 个 `SpScene`、约 180 个 `SpButton`、约 40 个 `SpProb`、4 个 `SetpieceEnemy`、约 60 个 `LootDrop`。按 `setpieces_data.h` 现有密度（558 行装 12 个 setpiece）估计新增 **约 1200–1500 行数据表**，是 Phase 3 单文件最大的一块。建议单独放 `src/executioner_data.h`。

### 10.4 战斗引擎扩展（1 秒 tick 下的状态系统）

现有战斗已是**离散 1 秒 tick**（`world_state.cpp:796-823`），上游的毫秒时长按整秒折算：

| 上游 | 值 | 折算 |
|---|---|---|
| `STUN_DURATION` 4000 ms | 已有 `FIGHT_STUN_S = 4` | 4 tick ✔ |
| `ENRAGE_DURATION` 4000 ms | — | 4 tick；期间 `enemyDelayS` 强制 1（上游是 0.5 s，1 s tick 下取最小值 1） |
| `MEDITATE_DURATION` 5000 ms | — | 5 tick |
| `BOOST_DURATION` 3000 ms | — | 3 tick |
| `DOT_TICK` 1000 ms | — | 每 tick 一次 ✔（天然对齐） |
| `EXPLOSION_DURATION` 3000 ms | — | 3 tick 前摇，然后结算 30 点伤害 |
| `specials.delay` 5 / 7 / 13 / 16 s | — | 直接用作 tick 计数 ✔ |

**`Combat` 结构新增**（`src/world_state.h:84-115`）：
```cpp
uint8_t  playerStatus;      // ST_NONE / ST_SHIELD / ST_BOOST
uint8_t  playerStatusLeft;  // 剩余 tick
uint8_t  enemyStatus;       // ST_NONE / ST_SHIELD / ST_ENRAGED / ST_ENERGISED /
                            // ST_VENOMOUS / ST_MEDITATION
uint8_t  enemyStatusLeft;
int16_t  meditateAccum;     // 冥想吸收的累计伤害
int16_t  dotDamage;         // 每 tick 的持续伤害；0 = 无
uint8_t  dotTicksLeft;
int16_t  specialDelay;      // 场景 specials 的周期（tick），0 = 无
int16_t  specialLeft;
uint8_t  specialKind;       // 施加哪种状态；SK_RANDOM3 = 指挥甲板 BOSS
uint8_t  lastSpecial;       // BOSS 的"不连续同状态"记忆
int16_t  atHealthThreshold; // 0 = 无
uint8_t  atHealthStatus;
int16_t  explosionDamage;   // 0 = 无
uint8_t  explodeTicksLeft;
int16_t  hypoCool, stimCool, shieldCool;
```

**`fight_modal` 按钮扩展**（`src/fight_modal.cpp:92`）：
```cpp
enum : uint8_t { BK_WEAPON, BK_EAT, BK_MEDS, BK_HYPO, BK_STIM, BK_SHIELD, BK_FLEE };
```
门条件（照 `events.js:144-155`）：`BK_HYPO` 需 `ex.outfitRes[R_HYPO] > 0`；`BK_STIM` 需 `ex.outfitRes[R_STIM] > 0`；`BK_SHIELD` 需 `g_game.items[I_KINETIC_ARMOUR] > 0`（注意上游查的是 `stores` 不是 `outfit`）。

> **布局风险**：`fight_modal` 现在是 2 列 × 80 px 高按钮，底部 `BTN_BOTTOM = 912`、`VBTN_TOP = 832`。武器最多可达 12 种（9 现有 + 3 新增），加上 5 个治疗/防御钮，最坏 17 个按钮 = 9 行 × 90 px = 810 px，**放不下**。需要在 3c 阶段专门解决（分页 / 缩小 / 只显示已装备的前 N 个）。详见 §12 Q13。

**`attackDelay < 1 s` 的敌人**：序章 `3-1` / `4-1` 的 `chitinous horror` / `chitinous queen` 是 `attackDelay 0.25, damage 1`。1 秒 tick 下无法表达，折叠为 **DPS 等价**：

| 上游 | 折算后 | 期望 DPS 校验 |
|---|---|---|
| `damage 1, hit 0.7, attackDelay 0.25` | `damage 4, hit 0.7, attackDelay 1` | 上游 4 次/秒 × 1 × 0.7 = 2.8；折算 1 次/秒 × 4 × 0.7 = 2.8 ✔ |
| `automated turret` `attackDelay 2.5` | `attackDelay 3, damage 12` | 上游 10/2.5 = 4.0 DPS；折算 12/3 = 4.0 ✔ |
| `enraged` 期间 `attackDelay 0.5` | `attackDelay 1, damage ×2` | 期望一致，方差变大 |

方差变大（一次打 4 而不是四次打 1）是这个折算的代价，在 10 HP 起步的低血量段可能造成"秒杀"观感。序章两只虫子出现在距村 28 格，玩家此时至少有 `i armour`（25 HP），4 点/秒可以接受。

### 10.5 世界层接线

| 挂钩 | 现状 | 改动 |
|---|---|---|
| `T_SHIP` / `T_EXECUTIONER` 布点 | ✔ 已就位（`world_data.h:157,161`，num=1、R=28） | 无 |
| `SP_SHIP` setpiece 表 | ✔ 已有单场景（`setpieces_data.h:228-236`） | 无 |
| `SPE_CLEAR_SHIP` | ✔ 已有（`setpieces_data.h:57`，分发 `setpiece_engine.cpp:44`） | 无 |
| `clearMine()` 的 `T_SHIP` / `T_EXECUTIONER` 分支 | ✔ 已有（`world_state.cpp:630-631`） | 无 |
| `goHome()` 的 Phase 3 解锁 | ⚠️ 只有注释占位（`world_state.cpp:541`） | 加：`if (ex.clearedShip && !shipPageOpen) ship_page::open();` / `if (ex.clearedExec && !fabPageOpen) fabricator_page::open();` + 通知 |
| X 格的"序章 vs 前厅"分支 | ✘ 无 | `WorldState::move()` 的 `STEP_LANDMARK` 分支里，`T_EXECUTIONER` 按 `ex.execEntered` 选 `SP_EXEC_INTRO` 或 `SP_EXEC_ANTE`（对应 `world.js:573-576`） |
| `redeemBlueprints` | ✘ 无 | `goHome()` 里 `g_game.blueprints \|= ex.bpFound;` |
| `leaveItAtHome` 白名单 | 已有 `leaveResAtHome()`（`world_state.cpp:515-523`） | 加 `R_HYPO` / `R_STIM`；三个新武器走 Item 路径天然留背包（§4.4） |

---

## §11 分期建议与验收标准

分成 **4 个可独立发版的小阶段**。每阶段都必须能单独刷机、单独玩通，且不破坏既有存档。

### 3a — 星舰页 + W 事件接线（最小闭环，不含太空关）

**范围**：`ShipPage`、`Item`/`Res` 不动、存档 v4（`shiph`/`shipt`）、`goHome` 的 ship 解锁分支、hull/thrusters 两个按钮、起飞确认事件（起飞按钮暂时只弹"尚未实现"或直接禁用）。

**验收**：
1. 新档从零走到 W 地标（距村 28），`salvage` 后地图出现道路、tile 变 `W!`，再踩无效。
2. 活着回村 → 通知 + 翻页能到「破旧星舰」页；**死在回村路上 → 页面不出现**（验证 `Expedition` 两层态语义）。
3. `reinforce hull` 每次扣 1 alien alloy、hull +1；alien alloy 不足时推 `tr("not enough alien alloy")` 且不扣冷却。
4. hull = 0 时起飞按钮为**虚线禁用框**（`action_band` 契约）；加固一次后变实线可用。
5. 起飞确认事件（`Ready to Leave?`）**只出现一次**；选 `linger` 清掉冷却。
6. 断电重启后 hull/thrusters/页面可见性全部保留；**v3 存档升级到 v4 不丢数据**。
7. `tools/world_smoke.cpp` 新增用例：模拟 salvage → goHome → 断言 `shipUnlocked == true`；模拟 salvage → die → 断言 `false`。host 编译通过。

### 3b — 太空玩法

**范围**：`src/space_page.cpp`（全屏 modal + 阻塞子循环）、触摸驱动加 `down` 位、层过场、crash/win 演出、结算页、`pager::deghost()` 的进出场调用。

**前置**：先复测 `env:adarkroom` 在 `-Os` 下的实际扫描 fps（§7 缺口 1）。若显著低于 43.4 Hz，必须先加 `-O2` 或调整 `FRAME_MS`。

**验收**：
1. **性能**：整局串口 STATS 行 `fps ≥ 40`、`frame_us_max < 23000`、`dma_timeouts == 0`；逻辑帧间隔实测均值 92 ± 5 ms，最大值 < 120 ms（用 spike 那套分段计时打印）。
2. **可玩性**：手指在控制带内从最左滑到最右，飞船能到达 x=24 与 x=516 两端（验证绕开了 pager 的 24 px 边缘带）；手指抬起飞船原地不动。
3. **可读性目检**：录一段慢动作视频，确认 (a) 陨石在拖尾下仍读作"一个下落的圆"而非灰雾；(b) 命中时飞船反色能看清；(c) hull 方块消失是"整块消失"不是糊团。
4. **规则**：碰撞扣 1 hull 且该陨石消失；hull 到 0 触发 crash；crash 回星舰页并起 **120 秒**冷却；持久 hull/thrusters/库存**全部不变**。
5. **通关**：不被击落时 61.4 ± 1 s 触发胜利（60 s 游戏时长 + 5 次过场各 276 ms）；分数按 §2.8 公式计算并与手算一致。
6. **层过场**：5 次白闪（10/20/30/45/60 km），每次层名正确、高度计时在过场期间暂停。
7. **残影**：进场前、crash/win 后各一次 deghost；局内**零** `pushImage` 调用（加断言或计数器验证）。
8. **不阻塞**：整局期间 BLE / OTA 不响应是**可接受的**（游戏独占 app 任务），但退出后必须自动恢复——验证退出后 2 秒内 BLE STATUS 恢复发送。

### 3c — Executioner + Fabricator

拆三个子阶段，各自可独立验收：

**3c-1 战斗引擎扩展**（不含 Executioner 数据）
- 范围：`Combat` 的 status/specials/atHealth/explosion 字段与 tick 逻辑、3 个新武器、`fight_modal` 的 5 个新按钮 + 布局重排。
- 验收：用一个**临时测试 setpiece**（不进最终版）逐个验证 6 种状态：shield 把伤害转成治疗且一击即破；enraged 期间攻击间隔降到 1 tick 且 4 tick 后恢复；energised 下一击 ×4 后清除；venomous 挂 DOT 每 tick 掉 `floor(dmg/2)`；meditation 吸伤 5 tick 后一次性反弹；explosion 死亡后 3 tick 前摇再造成 30 点。`tools/mechanics_test.cpp` 加对应的 host 用例，全部断言期望伤害。
- 验收：`fight_modal` 在**最坏 17 个按钮**下不越界、不重叠、每个按钮 ≥ 80 px 高。

**3c-2 Executioner 数据表**
- 范围：`src/executioner_data.h`（6 个 setpiece / 96 场景）、`SP_SCENE_EVENT` 跳转、`availCond`、`CK_WATER`/`CK_HP` 成本、4 个新 `SpEffect`、`Expedition` 的 3 个 wing bool + `bpFound`、trek v2。
- 验收：
  1. 首次踩 X → 序章；三条支线（0.4/0.4/0.2）各能跑通；`enter` 扣 1 torch，无 torch 时按钮禁用。
  2. 打穿序章 → X 格画路 → 回村开「制造机」页。
  3. 再踩 X → 前厅；三个区按钮初始可用，打穿一个后对应按钮变灰；三个全灰后出现 `command deck`。
  4. 六张蓝图各自 100% 掉落，回村后在制造机页的 `blueprints` 列表里可见。
  5. **中途 `leave` 直接退出整个事件**（不回前厅），再踩 X 从前厅重进。
  6. 死在区内 → wing 进度全丢（与上游一致）；host 用例断言。
  7. martial `7-1` 的 `scavenge maps` 写的是 `ex.revealed`，活着回村后揭图**保留**（修上游 bug）。

**3c-3 Fabricator 页 + 产物接线**
- 范围：`FabricatorPage`、`FABRICATE[9]` 表、6 个新 `Item` + 4 个新 `Res`、Path 装载行、`maxWater/maxHealth/bagCapacityCenti` 的新档位。
- 验收：
  1. 三件无需蓝图的（energy blade / fluid recycler / cargo drone）开页即可造；六件需蓝图的在拿到蓝图前**按钮不存在**，兑换后出现。
  2. `hypo` 一次产 5 个，按钮文本带 `(x5)`。
  3. 三个 upgrade 各限 1 件，造满后按钮变禁用。
  4. 造出 `cargo drone` 后 Path 页容量显示 110；`fluid recycler` 后水上限 110；`kinetic armour` 后 World 页 HP 上限 85 且战斗出现 `shield` 钮。
  5. `plasma rifle` 12 伤 / 1 s 冷却 / 耗 1 energy cell；`energy blade` 10 伤 / 2 s / 无耗；`disruptor` 眩晕 4 tick / 15 s / 无耗。
  6. `alien alloy` 回村留家（不留背包），`hypo`/`stim`/三把新武器留背包。

### 3d — 结局与分数

**范围**：分数公式（§2.8）、`fleet beacon` 分支的扩展结局字幕、结算页、删档/prestige 决策（§12 Q1 拍板后实施）。

**验收**：
1. 分数与手算一致（用一份固定存档做 golden test，进 `tools/mechanics_test.cpp`）。
2. 持有 `fleet beacon` 时播扩展结局四段字幕 + `wait` 按钮；不持有时直接进结算页。
3. 结算页无 iOS/android 外链。
4. 删档/轮回行为符合 Q1 的决定，且**不会因为误触发导致玩家意外丢档**（至少一次确认）。

### 跨阶段的文案工作

212 条待译（§6）建议按阶段切：
- **3a**：Ship 的 12 条已译，零工作量。
- **3b**：Space 的 `wait` 1 条 + 自造的层过场/坠毁文案（约 5 条）。
- **3c**：**主要工作量在这里**——Fabricator 23 条 + Executioner 188 条。建议在 3c-2 开工前先把 Executioner 的 188 条译完并跑一次 `gen_cjk_font.py`，**因为字体重生成会改变 flash 占用，越早暴露越好**。
- 每次改 `src/strings_zh.h` 后必须重跑 `tools/gen_cjk_font.py --proof proof.png` 并目检。

---

## §12 开放问题（需要拍板）

| # | 问题 | 背景 | 建议 |
|---|---|---|---|
| **Q1** | **通关后是否删档 + prestige 轮回？** | 上游 `Space.endGame()` 里 `Prestige.save()` + `Engine.deleteSave(true)`（`space.js:439,445`）——通关即删档，部分库存按随机系数带进下一轮。这块设备是**常驻桌面的**，删档语义可能非常刺激 | 三选一：(a) 完全照抄；(b) 通关后进"结算页 + 手动确认才重开"；(c) 通关只记分不删档，`U`(CACHE) 地标与 prestige 整个不做。**倾向 (b)**——保留仪式感但不会误删 |
| **Q2** | **太空关是 1D 还是 2D？** | §8.3 推荐 1D（飞船 y 固定）。2D 需要控制带映射两个轴（如手指 x→船 x、手指 y→船 y，控制带变成一个缩小的镜像区），复杂度和误操作率都上升 | 建议 3b 先做 1D 上线，2D 作为可选后续 |
| **Q3** | **是否改动 `touch_gt911`？** | §7 缺口 2：`Detail` 需要 `bool down`。改动 3–4 行，但触碰的是所有页面共用的输入驱动 | 建议改。加字段是纯增量，不影响现有 `clicked`/`held` 语义 |
| **Q4** | **是否修 `setTitle` 的 `%10` bug？** | 上游 45 km 的层名要等到 50 km 才更新（§2.6） | 建议修（按阈值切换）。我们的层过场就发生在 45 km，不修会出现"过场了但层名没变" |
| **Q5** | **`quadruped` 掉落的重复键 bug 怎么处理？** | 上游 JS 对象字面量重复 `'alien alloy'` 键，实际只生效 `2–4@0.2`，80% 空手（§3.2） | 建议**照实际行为复刻**（只留 2–4@0.2）。四足是最常出现的敌人（8 次），按"作者意图"补上 1–1@1 会让 alien alloy 产量翻倍，破坏 Fabricator 的稀缺感 |
| **Q6** | **Executioner 的区进度是否改为"打穿即落库"？** | 上游三个 wing 标志活在 `World.state`，死在回村路上就全丢（§3.1）。单区 20–30 个场景 + 5–8 场战斗，是全游戏最长的单次投入 | 三选一：(a) 照抄；(b) 打穿一个区就立刻写已提交层；(c) 只有 `execEntered`（序章）立刻落库，wing 照抄。**倾向 (c)**——保住"进得去"这件事，wing 的风险留着 |
| **Q7** | **`boost`（stim）的实际收益怎么定义？** | 上游 `useStim` 只设状态 + 自伤 10，**没有在 `damage()` 里定义任何伤害倍率**；`boosted:` 只是 Button 层的视觉。而且 `useStim` **不扣 `Path.outfit['stim']`**（`events.js:461-466`），可以无限点 | 必须自己定义。建议：3 tick 内所有武器伤害 ×2，且**扣 1 个 stim**（修上游的不扣道具 bug） |
| **Q8** | **`glowstone` 要不要实现？** | 上游可制造，但**全仓没有任何消费点**（§4.3）——纯未完成内容 | 建议 3c 先不做（少 1 个 Res 槽、少 1 张蓝图、少 3 条译文）。或者赋予一个自定义作用（如替代 torch 进地牢） |
| **Q9** | **页面 ring 的插入位置？** | 上游顺序是 Room→Outside→Path→Fabricator→Ship；我们的 ring 已有 trade/assign/tech 三个固件专属页 | 建议追加到 `s_reg[]` 末尾（最小 diff，且 `meta.json` 按名恢复所以安全）。若要贴近上游阅读顺序则插在 `path` 之后 |
| **Q10** | **蓝图用位掩码还是 6 个 `Res` 槽？** | 位掩码省 6 个数组槽（`stores`/`savedOutfitRes`/`outfitRes` 三处各省 6），但偏离上游"蓝图是有重量的背包物品"的语义（各重 1） | 建议位掩码（§10.2）。6 张蓝图共重 6 单位，对 110 容量的背包影响可忽略，不值得为它膨胀三个数组 |
| **Q11** | **`-O2` / `MSG_TRANSPORT_RAW` 要不要为太空关开？** | 主 env 现在是 `-Os` 且 raw 传输关闭（43.4 Hz）。开 raw 可上 55.7 Hz，但必须同时把 `MSG_DRIVE_FRAMES` 提到 9，否则黑变灰 | 建议 3b 先在 `-Os` + 43.4 Hz 下把玩法做通；性能不够再考虑 `-O2`。**不建议动 `MSG_TRANSPORT_RAW`**——它会改变所有页面的显示特性，风险远超收益 |
| **Q12** | **thrusters 升级在绝对映射下封顶（≥5 无收益）可接受吗？** | §8.3 的 `SHIP_MAX_STEP = min(72, 32 + 8×thrusters)` | 若不可接受，可让 thrusters 额外影响"控制带增益"（>1:1 映射，手指走一半屏幕船走满屏），但那会牺牲绝对映射的直观性 |
| **Q13** | **`fight_modal` 最坏 17 个按钮怎么排？** | §10.4 布局风险：2 列 × 90 px = 810 px，超出 `VBTN_TOP=832` 到 `BTN_BOTTOM=912` 的可用高度 | 三选一：(a) 只显示"当前 outfit 里伤害最高的 4 把武器"；(b) 武器按钮做横向分页；(c) 缩小到 3 列 × 60 px。**倾向 (a)**——upstream 的多武器并列本来就很少同时有用 |
| **Q14** | **crash 无惩罚要不要保留？** | 上游 crash 只罚 120 秒冷却，hull/库存全不扣（§2.7）。这让太空关变成"无限重试的耐心测试" | 建议保留。e-ink 上的操作精度本来就低于键鼠，任何额外惩罚都会把这个玩法推向挫败 |
| **Q15** | **`attackDelay < 1 s` 的 DPS 折叠可接受吗？** | §10.4：`1 伤 ×4 次/秒` → `4 伤 ×1 次/秒`，期望相同但方差变大 | 若不可接受，替代方案是把战斗 tick 从 1 s 降到 0.5 s（所有冷却数值 ×2），代价是战斗期间的 `present()` 频率翻倍、残影债翻倍 |
