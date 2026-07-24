# Phase 2 上游机制规格书（Path + World）

> 面向"没读过上游源码的实现者"的移植规格。数值一律以上游 master/main 源码为准。
> 上游仓库：`doublespeakgames/adarkroom`（默认分支 `main`，`master` 为其别名）。
> 许可：上游为 MPL-2.0；本仓库沿用同源，文案 glyph 走既定闭包流程。
>
> 本文抓取的上游文件（commit 时点 2026-07）：
> - `script/path.js`（341 行）— 出发准备 / 背包
> - `script/world.js`（1109 行）— 世界地图 / 移动 / 消耗
> - `script/events.js`（1487 行）— 事件与战斗引擎
> - `script/events/encounters.js`（437 行）— 随机遭遇敌人表
> - `script/events/setpieces.js`（3587 行）— 地标玩法
> - `script/events/executioner.js`（2343 行）— 处刑者战舰（Phase 3 边界）
> - `script/outside.js`（665 行）— worker/building 收入表（经济接口）
> - `script/room.js`（1259 行）— compass 交易与 Path 解锁
> - `lang/zh_cn/strings.po` — 官方简体中文翻译（gettext，793 msgid，见 §7）
>
> **术语约定**：所有坐标为 `[x, y]`，`x` 向东为正、`y` 向南为正。时间常量单位见上下文（战斗冷却/延迟为**秒**，动画为**毫秒**）。凡标「未核实」处，实现前需再对源码确认。

## 目录

- §0 与 Phase 1 的关系 / 移植难点速览
- §1 Path — 出发准备与背包
- §2 World — 地图尺寸、生成、可见性、道路
- §3 移动与消耗（食水 / 死亡 / 回村入库）
- §4 战斗（遭遇概率 / 敌人表 / 武器表 / 护甲 / 战斗内动作 / 逃跑）
- §5 地标 setpieces 摘要
- §6 与村庄经济的接口（现有 `game_data.h` 需扩什么）
- §7 文案清单与 zh_cn 覆盖度

---

## §0 与 Phase 1 的关系 / 移植难点速览

Phase 1 已实现 Room / Outside / Trade / 事件系统。Phase 2 引入两个新"位置模块"：

- **Path（漫漫尘途）**：一个出发前的装备/补给面板。解锁条件 = 玩家在 Room 交易站买到 `compass`（罗盘）。
- **World（荒芜世界）**：61×61 字符地图上的探索 + 实时战斗，从 Path 点 `embark` 进入，回到村庄格或死亡则离开。

数据流闭环：`Room 买 compass → 开 Path 标签 → 装 cured meat + 武器 → embark → World 探索/战斗/清地标 → 回村 goHome → 战利品入库、矿山解锁 miner 职业`。

**移植难度最大的三点（详见对应小节）**：

1. **实时战斗（§4）**。上游战斗**不是回合制**：敌人用 `setInterval(attackDelay*1000)` 定时攻击，玩家按钮各带 `cooldown` 秒。移植到 M5PaperS3 e-ink（刷新慢、无高频定时 UI）必须重新设计为回合/半回合制或离散 tick，且要保持"武器 cooldown 有快慢、敌人攻速有别、可在攻击间隙吃肉/用药"的手感。这是最大改动点。
2. **World 临时态 vs 提交态（§3）**。`embark` 时把 `game.world` 深拷贝成 `World.state` 临时探索；**只有活着走回村庄格才 `goHome` 提交**（清掉的地牢、揭开的地图、清完的矿山才落库）；**死亡直接丢弃整个 `World.state` 且清空背包**。这套"探索期改动可回滚"的语义需要一份独立的易失世界状态存储，且要和 Phase 1 的 `$SM`（持久 game state）分层清楚。
3. **61×61 地图的生成与渲染 + 道路重算（§2）**。程序化地形（stickiness 概率 + landmark 环带布点）、diamond 可见性掩码、清地牢后 `drawRoad()` 螺旋找最近路网再画 L 形路——都是纯算法，可 1:1 移植，但 e-ink 上"每步只重绘可见窗口"的增量渲染要自己做（上游是整图 `innerHTML`）。

**好消息**：官方 zh_cn 翻译对 Phase 2 覆盖近乎 100%（§7），文案排期风险很低。

---

## §1 Path — 出发准备与背包

出处：`path.js`（整文件）。

### 1.1 背包容量 `Path.getCapacity()`

`path.js Path.DEFAULT_BAG_SPACE = 10`。容量为**分级取最高**（不是叠加），判定顺序：

| 条件（`stores` 里数量 > 0） | 容量 |
|---|---|
| `cargo drone`（Phase 3，Fabricator） | 10 + 100 = **110** |
| `convoy` | 10 + 60 = **70** |
| `wagon` | 10 + 30 = **40** |
| `rucksack` | 10 + 10 = **20** |
| 都没有 | **10** |

出处 `path.js Path.getCapacity()`。注意 wagon/convoy/rucksack 都是 Phase 1 已有的 craftable（见 `game_data.h` Item 表），`cargo drone` 属 Phase 3，可先留空档。

### 1.2 单位重量表 `Path.Weight`

出处 `path.js Path.Weight`。**表中没有的物品一律重 1**（`Path.getWeight()`：非数字则取 1）。重量可为小数，容量运算走浮点。

| 物品 | 重量 |
|---|---|
| （默认，未列出的一切，如 cured meat / medicine / fur / cloth …）| **1** |
| `bone spear` | 2 |
| `iron sword` | 3 |
| `steel sword` | 5 |
| `rifle` | 5 |
| `laser rifle` | 5 |
| `plasma rifle` | 5（Phase 3）|
| `bolas` | 0.5 |
| `bullets` | 0.1 |
| `energy cell` | 0.2 |

`getFreeSpace() = getCapacity() − Σ(outfit[k] × getWeight(k))`（`path.js Path.getFreeSpace()`）。

### 1.3 可携带物品清单（outfitting 行）

出处 `path.js Path.updateOutfitting()`。可装列表 = 一份内置 `carryable` 对象 **∪** `Room.Craftables` **∪** `Fabricator.Craftables`。内置部分：

| key | type | 说明（desc） |
|---|---|---|
| `cured meat` | tool | `restores 8 hp`（8 = `World.MEAT_HEAL`）|
| `bullets` | tool | `use with rifle` |
| `grenade` | weapon | — |
| `bolas` | weapon | — |
| `laser rifle` | weapon | — |
| `energy cell` | tool | `emits a soft red glow` |
| `bayonet` | weapon | — |
| `charm` | tool | —（沼泽对话消耗品）|
| `alien alloy` | tool | — |
| `medicine` | tool | `restores 20 hp`（20 = `World.MEDS_HEAL`）|

再并上 Phase 1 craftable（torch / bone spear / iron sword / steel sword / rifle / 各护甲等）。**渲染规则**：只有 `type` 为 `tool` 或 `weapon` 且 `stores` 里持有量 > 0 的才出行；护甲和 water 单独成行（不占背包重量，见下）。每行有 +1/−1/+10/−10 四个调节钮；上限受 `min(freeSpace/weight 向下取整, 库存)` 约束（`Path.increaseSupply()`）。

### 1.4 armour / water 特殊行

- **armour 行**（`Path.updateOutfitting()`）：显示当前护甲名，**分级取最高**：`kinetic armour`→"kinetic" > `s armour`→"steel" > `i armour`→"iron" > `l armour`→"leather" > 否则"none"。护甲不进背包、不可增减，纯展示（其效果在 World 侧转成最大 HP，见 §4.4）。
- **water 行**：显示 `World.getMaxWater()`（见 §3.2），同样不占背包。embark 时水自动灌满到该上限。

### 1.5 补给规则 / 出发条件 `Path.embark()`

- **cured meat**：必须**手动装载**。`Path.updateBagSpace()` 里，`embark` 按钮**仅当 `outfit['cured meat'] > 0` 时可点**，否则禁用。这是唯一的硬出发门槛。
- **water**：不在 Path 装载；embark 后 `World.onArrival()` 自动 `setWater(getMaxWater())` 灌满。
- **embark 逻辑**（`path.js Path.embark()`）：对 `outfit` 每一项从 `stores` 扣除对应数量 → `World.onArrival()` → 切模块到 World，播 `EMBARK` 音效。embark 按钮带 `World.DEATH_COOLDOWN = 120` 秒冷却（死亡后重进 Room 时才计冷却，见 §3.4）。

### 1.6 compass 获取与 Path 解锁

出处 `room.js`。`compass` 是交易站商品（不是打怪掉落）：

- `room.js Room.TradeGoods.compass`：`type: 'special'`，`maximum: 1`，`cost = { fur: 400, scales: 20, teeth: 10 }`。
- 买到后（`room.js` 第 931 行 `updateRoom`）：`if(stores.compass && !pathDiscovery){ pathDiscovery = true; Path.openPath(); }`。
- `Path.openPath()`（`path.js`）：`Path.init()` 建 Path 标签 + 面板，`Engine.event('progress','path')`，并推送通知 `the compass points <dir>`（`<dir>` 为飞船相对村庄的罗盘方向，见 §2.7）。

即：**罗盘 = 解锁 Path 的钥匙**；**cured meat ≥ 1 = 允许 embark 的条件**。`game_data.h` 已有 `R_COMPASS` 与 TRADE 表含 compass，Phase 2 只需接上"买到 compass → 开 Path"这一步。

---

## §2 World — 地图

出处：`world.js`。

### 2.1 尺寸与常量

| 常量 | 值 | 含义 |
|---|---|---|
| `RADIUS` | 30 | 半径 |
| 地图数组尺寸 | `RADIUS*2+1` = **61×61** | 下标 0..60 |
| `VILLAGE_POS` | `[30, 30]` | 村庄永远在正中心 |
| `STICKINESS` | 0.5 | 地形粘连系数（0..1）|
| `LIGHT_RADIUS` | 2 | 可见半径（曼哈顿）|
| `BASE_WATER` | 10 | 基础水上限 |
| `BASE_HEALTH` | 10 | 基础 HP |
| `MOVES_PER_FOOD` | 2 | 每 N 步吃 1 肉 |
| `MOVES_PER_WATER` | 1 | 每 N 步喝 1 水 |
| `DEATH_COOLDOWN` | 120（秒）| 死后 embark 冷却 |
| `FIGHT_CHANCE` | 0.20 | 每步触发战斗概率 |
| `FIGHT_DELAY` | 3 | 两次战斗最少间隔步数 |
| `BASE_HIT_CHANCE` | 0.8 | 玩家基础命中 |
| `MEAT_HEAL` / `MEDS_HEAL` / `HYPO_HEAL` | 8 / 20 / 30 | 治疗量 |

方向向量：`NORTH=[0,-1] SOUTH=[0,1] WEST=[-1,0] EAST=[1,0]`。

### 2.2 TILE 字符表

出处 `world.js World.TILE`。地图每格是一个字符串（landmark 未访问时是单字符；访问过会追加 `!`，见 `markVisited`）。

| 常量 | 字符 | 含义 |
|---|---|---|
| VILLAGE | `A` | 村庄（家）|
| IRON_MINE | `I` | 铁矿 |
| COAL_MINE | `C` | 煤矿 |
| SULPHUR_MINE | `S` | 硫磺矿 |
| FOREST | `;` | 森林（可通行地形）|
| FIELD | `,` | 原野（可通行地形）|
| BARRENS | `.` | 荒原（可通行地形）|
| ROAD | `#` | 道路 |
| HOUSE | `H` | 旧房子 |
| CAVE | `V` | 洞穴 |
| TOWN | `O` | 废弃小镇 |
| CITY | `Y` | 被毁城市 |
| OUTPOST | `P` | 前哨（清完地牢后生成）|
| SHIP | `W` | 坠毁星舰 |
| BOREHOLE | `B` | 巨坑 |
| BATTLEFIELD | `F` | 战场 |
| SWAMP | `M` | 沼泽 |
| CACHE | `U` | 被毁村庄（仅 prestige）|
| EXECUTIONER | `X` | 处刑者战舰 |

"地形"三种（`isTerrain()`）：`FOREST / FIELD / BARRENS`。玩家自身在地图上渲染为 `@`，tooltip 为 `Wanderer`。

### 2.3 地形概率与 `chooseTile`

出处 `world.js World.TILE_PROBS`（`init()` 里赋值，和必须 = 1）：

| 地形 | 基础概率 |
|---|---|
| FOREST `;` | 0.15 |
| FIELD `,` | 0.35 |
| BARRENS `.` | 0.50 |

`chooseTile(x,y,map)` 算法（`world.js`）：
1. 取上下左右四邻已生成的格。
2. **若任一邻格是 VILLAGE → 直接返回 FOREST**（村庄四周强制森林）。
3. 对每个"是地形字符串"的邻格：给该地形累加 `STICKINESS(0.5)` 的权重，并从 `nonSticky` 池扣掉 0.5（`nonSticky` 初值 1）。
4. 对三种地形再各加 `TILE_PROBS[t] × nonSticky`。
5. 把 `权重+地形字符` 拼串按权重降序排序，用 `r=Math.random()` 累积区间抽取；兜底返回 BARRENS。

效果：地形有"抱团"倾向（相邻同地形概率被 stickiness 抬高），越靠外越荒。

**生成顺序 `generateMap()`**：先把中心设 VILLAGE，然后 `for r=1..RADIUS` 一圈一圈（螺旋）对每格 `chooseTile`；全部地形铺完后再布 landmark（§2.4）。

### 2.4 LANDMARKS 分布表

出处 `world.js World.LANDMARKS`（`init()` 里）。每项：`num`（数量）、`minRadius`、`maxRadius`、`scene`（对应 setpiece key）、`label`（地图 tooltip，用 `&nbsp;` 连字）。`RADIUS*1.5 = 45`。

| TILE | 字符 | num | minRadius | maxRadius | scene | label |
|---|---|---|---|---|---|---|
| OUTPOST | P | **0** | 0 | 0 | outpost | An Outpost |
| IRON_MINE | I | 1 | 5 | 5 | ironmine | Iron Mine |
| COAL_MINE | C | 1 | 10 | 10 | coalmine | Coal Mine |
| SULPHUR_MINE | S | 1 | 20 | 20 | sulphurmine | Sulphur Mine |
| HOUSE | H | 10 | 0 | 45 | house | An Old House |
| CAVE | V | 5 | 3 | 10 | cave | A Damp Cave |
| TOWN | O | 10 | 10 | 20 | town | An Abandoned Town |
| CITY | Y | 20 | 20 | 45 | city | A Ruined City |
| SHIP | W | 1 | 28 | 28 | ship | A Crashed Starship |
| BOREHOLE | B | 10 | 15 | 45 | borehole | A Borehole |
| BATTLEFIELD | F | 5 | 18 | 45 | battlefield | A Battlefield |
| SWAMP | M | 1 | 15 | 45 | swamp | A Murky Swamp |
| EXECUTIONER | X | 1 | 28 | 28 | executioner | A Ravaged Battleship |
| CACHE | U | 1 | 10 | 45 | cache | A Destroyed Village（仅当有 `previous.stores` prestige 数据时才加入）|

说明：
- **OUTPOST num=0**：初始不布点，只由玩家清完地牢（`clearDungeon()`）动态生成。
- `minRadius==maxRadius` 的（矿、ship、executioner）落点距村庄**曼哈顿距离固定**：铁矿 5、煤矿 10、硫磺矿 20、ship/executioner 28（`placeLandmark` 里 `Math.random()*(max-min)` = 0）。
- EXECUTIONER 在旧存档里若缺失会在 `init()` 补布（`features.executioner` 迁移逻辑）。

**`placeLandmark(minR,maxR,tile,map)`**（`world.js`）：从村庄出发，循环直到落在"地形"格上——每轮取 `r=floor(random*(maxR−minR))+minR`，`xDist=floor(random*r)`，`yDist=r−xDist`，各 50% 取负，得 `[RADIUS+xDist, RADIUS+yDist]`（裁剪到 0..60）；非地形则重抽。即距离 = `|xDist|+|yDist|` 落在 `[minR, maxR)` 环带内，且必须压在可通行地形上（不会覆盖已有 landmark）。

### 2.5 可见性 / 掩码

出处 `world.js`。`mask` 是 61×61 布尔数组（`newMask()`），未揭开处渲染为 `&nbsp;`。

- `lightMap(x,y,mask)`：以 `r = LIGHT_RADIUS(2) × (scout perk ? 2 : 1)` 揭图。
- `uncoverMap(x,y,r,mask)`：揭开**曼哈顿距离 ≤ r 的菱形**（`for i=-r..r, j=-(r-|i|)..(r-|i|)`）。
- 每次 `move()` 都对新位置 `lightMap`。
- `applyMap()`（Room 侧"绘制地图"道具，可后置）：在未揭开处随机点 `uncoverMap(...,5,...)`。
- `testMap()`：全揭开则置 `World.seenAll`（成就/进度）。

### 2.6 道路生成 `drawRoad()`

出处 `world.js`。触发点：清完地牢 `clearDungeon()`、清完矿、salvage ship 等的 `onLoad`。

1. `clearDungeon()`：把当前格设为 `OUTPOST`（`P`），再 `drawRoad()`。
2. `findClosestRoad(startPos)`：从当前格**螺旋外扩**（沿曼哈顿等距轮廓），找最近的 `ROAD` / `OUTPOST`（非自身）/ `VILLAGE`；找不到则回退到 `VILLAGE_POS`。搜索半径 ~`(distToVillage+2)²`。
3. 画 **L 形路**：先比较 `xDist/yDist` 决定先横后纵还是先纵后横，沿途**只把"地形"格改成 ROAD**（`isTerrain` 才覆盖，不破坏 landmark）。
4. `drawMap()` 重绘。

含义："所有路通向家"——每清一个点就把它并入既有路网，方便回村。矿山/ship/executioner 清完也画路。

### 2.7 罗盘指向（ship 方向）

出处 `world.js compassDir(pos)`。`init()` 里 `mapSearch(SHIP,map,1)` 找到飞船相对村庄的 `[x,y]`（已减去 RADIUS），`compassDir` 按 `|x|/2 vs |y|` 判主方向，输出 `north/south/east/west/northwest/northeast/southwest/southeast` 之一，喂给 Room 的 compass tooltip 与 Path 通知（§7 有全 8 向译文）。

---

## §3 移动与消耗

出处：`world.js`。

### 3.1 一步的流程 `move(direction)` → `doSpace()`

`move()`：记旧格 → 更新 `curPos` → `narrateMove(旧,新)`（跨地形时推地形转换文案，见 §7）→ `lightMap` → `drawMap` → **`doSpace()`** → 播随机脚步音 → `checkDanger()`（护甲不足且距离够远时警告，纯提示不扣血）。

`doSpace()` 分支（`world.js`）：
- 当前格是 **VILLAGE** → `goHome()`（§3.5）。
- 当前格是 **EXECUTIONER** → 触发处刑者事件（Phase 3）。
- 当前格是**其他 landmark** → 若不是"已用过的 outpost"，`Events.startEvent(Setpieces[scene])`（§5）。
- **否则（普通地形）** → `if(useSupplies()) checkFight()`：先扣食水，没死才 roll 战斗。

**关键**：踩到 landmark 的那一步**不扣食水、不 roll 战斗**（进事件）；只有走在空地形上才消耗补给并可能遇敌。

### 3.2 水上限 `getMaxWater()`

出处 `world.js`。分级取最高：

| 条件 | 水上限 |
|---|---|
| `fluid recycler`（Phase 3）| 10 + 100 = 110 |
| `water tank` | 10 + 50 = 60 |
| `cask` | 10 + 20 = 30 |
| `waterskin` | 10 + 10 = 20 |
| 无 | 10 |

waterskin/cask/water tank 均为 Phase 1 已有 craftable。

### 3.3 食水消耗 `useSupplies()`

出处 `world.js useSupplies()`。每步 `foodMove++`、`waterMove++`：

**食物**：`movesPerFood = MOVES_PER_FOOD(2) × (slow metabolism perk ? 2 : 1)`。当 `foodMove ≥ movesPerFood` 时结算一次，`num = outfit['cured meat']; num--`：
- `num === 0`（吃掉最后一块）：通知 `the meat has run out`，**不回血**。
- `num < 0`（已无肉）：`num=0`；首次 → 置 `starvation` 标志 + 通知 `starvation sets in`；再次 → `character.starved++`（≥10 且无 perk 则授予 `slow metabolism`）→ **`die()`**。
- `num > 0`（吃掉一块还有余）：清 starvation，`setHp(health + meatHeal())`，`meatHeal = MEAT_HEAL(8) × (gastronome perk ? 2 : 1)`。

**水**：`movesPerWater = MOVES_PER_WATER(1) × (desert rat perk ? 2 : 1)`。`water--`：`===0` 通知 `there is no more water`；`<0` 首次置 `thirst` + 通知 `the thirst becomes unbearable`，再次 → `character.dehydrated++`（≥10 授予 `desert rat`）→ **`die()`**；否则清 thirst。

**要点**：**饥渴没有渐进式扣血**——肉/水耗尽后先给一次警告 tick，再一次 tick 直接死亡（默认食物 = 断粮后约 2×movesPerFood=4 步内死）。断粮期间只是不再回血，HP 不主动下降；HP 只从战斗掉。移植时别自己加"每步扣血"。

### 3.4 死亡 `die()`

出处 `world.js die()`：
- 置 `dead`，播 `DEATH` 音，通知 `the world fades`。
- **`World.state = null`**（丢弃本次探索的一切地图改动：揭图、清完的地牢/矿**都不保存**）。
- **`Path.outfit = {}` 且 `$SM.remove('outfit')`**（**背包全部物品/战利品清空**，无掉落机制——死了就是全丢）。
- 动画切回 Room；2 秒后 `Room.onArrival()`，并给 embark 按钮上 `DEATH_COOLDOWN(120s)` 冷却。

即：**死亡 = 本趟血本无归**（连出发时带的肉/武器也没了，因为它们此刻都在 outfit 里）。

### 3.5 回村入库 `goHome()`

出处 `world.js goHome()`（踩到村庄格触发）：
1. **`$SM.setM('game.world', World.state)`**：把临时态提交为持久态（清完的地牢/矿、揭开的图落库）。
2. 矿山解锁（**经济闭环关键**）：
   - `state.ironmine && buildings["iron mine"]==0` → `buildings["iron mine"]++`，`progress: iron mine`。
   - `state.coalmine` → `buildings["coal mine"]++`，`progress: coal mine`。
   - `state.sulphurmine` → `buildings["sulphur mine"]++`，`progress: sulphur mine`。
3. `state.ship && !location.spaceShip` → `Ship.init()`（**Phase 3**）。
4. `state.executioner && !location.fabricator` → `Fabricator.init()` + 通知（**Phase 3**）。
5. `redeemBlueprints()`：把背包里的 `*blueprint` 兑换成 `character.blueprints[...]`（喂 Fabricator，Phase 3）。
6. **`returnOutfit()`**：`outfit` 每项 `stores[k] += outfit[k]`（战利品/剩余补给入库）；随后 `leaveItAtHome(k)` 为真的项把 `outfit[k]` 归零（留家），为假的项**保留在 outfit 里**供下次出发继续带。
7. 切回 Path 模块。

`leaveItAtHome(thing)`（`world.js`）：**留在背包（下次继续带）的**是 `cured meat / bullets / energy cell / charm / medicine / stim / hypo` 以及**任何武器**（`World.Weapons` 里的）和**任何 Room.Craftables**；其余（原材料如 fur/iron/teeth…）归零留家。

**总结**：活着回村 = 战利品入库 + 补给/武器留背包；矿山首次清理会自动"建成"对应矿山 building，解锁 miner 职业（§6）。

---

## §4 战斗

出处：`events.js`（引擎）、`encounters.js`（敌人表）。**上游是实时战斗。**

### 4.1 遭遇触发 `checkFight()` / `triggerFight()`

出处 `world.js checkFight()`：
- `fightMove++`；仅当 `fightMove > FIGHT_DELAY(3)`（即距上次战斗 ≥4 步）才 roll。
- `chance = FIGHT_CHANCE(0.20) × (stealthy perk ? 0.5 : 1)`；`Math.random() < chance` → `fightMove=0`，`Events.triggerFight()`。

`events.js triggerFight()`：从 `Events.Encounters` 里筛 `isAvailable()` 为真的（按**距离带 + 当前地形**），随机取一个 `startEvent`。距离分层同时决定战斗背景乐（Tier1/2/3）。

### 4.2 敌人表（`encounters.js`）

`isAvailable = 距离带 && 地形`。距离用曼哈顿 `getDistance()`。每个敌人的战斗字段：`health`（HP）、`damage`、`hit`（命中率）、`attackDelay`（攻击间隔秒）、`ranged`（是否远程动画）、`chara`（战斗中显示的单字符）、`loot`。

**Tier 1（距离 ≤ 10）**

| 敌人 | 地形 | HP | dmg | hit | delay | chara | 掉落（min–max @chance）|
|---|---|---|---|---|---|---|---|
| snarling beast | FOREST | 5 | 1 | 0.8 | 1 | R | fur 1–3@1, meat 1–3@1, teeth 1–3@0.8 |
| gaunt man | BARRENS | 6 | 2 | 0.8 | 2 | E | cloth 1–3@0.8, teeth 1–2@0.8, leather 1–2@0.5 |
| strange bird | FIELD | 4 | 3 | 0.8 | 2 | R | scales 1–3@0.8, teeth 1–2@0.5, meat 1–3@0.8 |
| two-headed creature | FIELD | 10 | 2 | 0.5 | 3 | K | fur 2–4@1, teeth 2–3@0.8, meat 2–3@0.8 |

**Tier 2（10 < 距离 ≤ 20）**

| 敌人 | 地形 | HP | dmg | hit | delay | chara | 掉落 |
|---|---|---|---|---|---|---|---|
| shivering man | BARRENS | 20 | 5 | 0.5 | 1 | E | cloth 1@0.2, teeth 1–2@0.8, leather 1@0.2, medicine 1–3@0.7 |
| man-eater | FOREST | 25 | 3 | 0.8 | 1 | T | fur 5–10@1, meat 5–10@1, teeth 5–10@0.8 |
| scavenger | BARRENS | 30 | 4 | 0.8 | 2 | E | cloth 5–10@0.8, leather 5–10@0.8, iron 1–5@0.5, medicine 1–2@0.1 |
| lizard（Huge Lizard）| FIELD | 20 | 5 | 0.8 | 2 | T | scales 5–10@0.8, teeth 5–10@0.5, meat 5–10@0.8 |

**Tier 3（距离 > 20）**

| 敌人 | 地形 | HP | dmg | hit | delay | chara | ranged | 掉落 |
|---|---|---|---|---|---|---|---|---|
| feral terror | FOREST | 45 | 6 | 0.8 | 1 | T | — | fur 5–10@1, meat 5–10@1, teeth 5–10@0.8 |
| soldier | BARRENS | 50 | 8 | 0.8 | 2 | D | ✔ | cloth 5–10@0.8, bullets 1–5@0.5, rifle 1@0.2, medicine 1–2@0.1 |
| sniper | FIELD | 30 | 15 | 0.8 | 4 | D | ✔ | cloth 5–10@0.8, bullets 1–5@0.5, rifle 1@0.2, medicine 1–2@0.1 |

掉落数量公式（`events.js drawLoot`）：`Math.random() < chance` 命中，命中则 `num = floor(random*(max−min)) + min`（注意**是 `max−min` 不含 max，实际范围 `[min, max−1]`；当 `max==min` 恒为该值**）。

### 4.3 武器表 `World.Weapons`

出处 `world.js World.Weapons`。`cooldown` 为**秒**（玩家攻击按钮冷却）。`cost` 为每次攻击消耗。`damage='stun'` 表示不掉血、使敌人眩晕。

| 武器 | verb | type | damage | cooldown | cost |
|---|---|---|---|---|---|
| fists | punch | unarmed | 1 | 2 | — |
| bone spear | stab | melee | 2 | 2 | — |
| iron sword | swing | melee | 4 | 2 | — |
| steel sword | slash | melee | 6 | 2 | — |
| bayonet | thrust | melee | 8 | 2 | — |
| rifle | shoot | ranged | 5 | 1 | bullets ×1 |
| laser rifle | blast | ranged | 8 | 1 | energy cell ×1 |
| grenade | lob | ranged | 15 | 5 | grenade ×1 |
| bolas | tangle | ranged | **stun** | 15 | bolas ×1 |
| plasma rifle | disintegrate | ranged | 12 | 1 | energy cell ×1（Phase 3）|
| energy blade | slice | melee | 10 | 2 | —（Phase 3 Fabricator）|
| disruptor | stun | ranged | **stun** | 15 | —（Phase 3）|

战斗开始时（`events.js startCombat`）只为 `outfit` 里数量 > 0 的武器建攻击钮；带 `cost` 但弹药不足的不计数；**若没有任何可用武器则回退到 `fists`（拳头）**。

**玩家伤害结算**（`events.js useWeapon`）：`Math.random() <= getHitChance()` 命中，否则 `dmg=-1`（显示 `miss`）。命中后按 perk 加成：unarmed 系 boxer×2 / martial artist×3 / unarmed master×2（可叠乘）；melee 系 barbarian `floor(dmg×1.5)`。`getHitChance() = BASE_HIT_CHANCE(0.8) + (precise perk ? 0.1 : 0)`。拳头攻击累计 `character.punches`，到 50/150/300 分别授予 boxer/martial artist/unarmed master。

### 4.4 护甲对 HP `getMaxHealth()`

出处 `world.js getMaxHealth()`。分级取最高：

| 条件 | 最大 HP |
|---|---|
| `kinetic armour`（Phase 3）| 10 + 75 = 85 |
| `s armour` | 10 + 35 = 45 |
| `i armour` | 10 + 15 = 25 |
| `l armour` | 10 + 5 = 15 |
| 无 | 10 |

`onArrival` 时 `setHp(getMaxHealth())` 灌满。`checkDanger()`（§3.1 提示）阈值：无 `i armour` 且距离 ≥ 8、或无 `s armour` 且距离 ≥ 18 时警告"没有妥善防护远行很危险"。

### 4.5 敌人攻击 / 战斗节奏

出处 `events.js`：
- `startEnemyAttacks()`：`setInterval(enemyAttack, attackDelay*1000)`——**敌人每 attackDelay 秒自动攻击一次**。
- `enemyAttack()`：`toHit = scene.hit × (evasive perk ? 0.8 : 1)`；命中 `dmg=scene.damage`，否则 miss。敌人若 `stunned` 或 `meditation` 则跳过。
- 玩家点武器钮攻击，受 `weapon.cooldown` 秒限制；`animateMelee`/`animateRanged` 走 `_FIGHT_SPEED=100ms` 动画后落伤害。
- **实时**：谁先把对方 HP 打到 ≤0 谁赢/输。玩家 HP≤0 → `checkPlayerDeath()` → `World.die()`。

关键杂项常量（`events.js` 顶部）：`_EAT_COOLDOWN 5`、`_MEDS_COOLDOWN 7`、`_HYPO_COOLDOWN 7`、`_SHIELD_COOLDOWN 10`、`_STIM_COOLDOWN 10`、`_LEAVE_COOLDOWN 1`、`STUN_DURATION 4000ms`、`_FIGHT_SPEED 100ms`。

### 4.6 战斗内动作

出处 `events.js`：
- **eat meat**（`createEatMeatButton`，cd `_EAT_COOLDOWN=5`s，cost cured meat×1）→ `doHeal('cured meat', meatHeal(), …)`，回 `MEAT_HEAL(8)×(gastronome?2:1)`。
- **use meds**（cd 7s，cost medicine×1）→ 回 `MEDS_HEAL(20)`。
- **use hypo**（cd 7s，cost hypo×1，Phase 3）→ 回 `HYPO_HEAL(30)`。
- **boost / stim**（`useStim`，cd 10s，Phase 3）：给自己 `boost` 状态并 `dotDamage(BOOST_DAMAGE=10)`（自伤换增伤）。
- **shield**（`useShield`，需持有 `kinetic armour`，cd 10s，Phase 3）：置 `shield` 状态，挡下一击。
- 治疗钮只有在 `health < maxHealth` 时可用（`setHeal`）；shield 例外。

Phase 2 只需实现 **eat meat + use meds**（其余 hypo/boost/shield 属 Fabricator/Phase 3）。

### 4.7 战利品拾取 / 掉落

出处 `events.js drawLoot / getLoot / takeAll / takeEverything / drawDrop`：
- 赢后逐项按 chance roll 出现，可**逐个 take / take all（本项）/ take everything（全部）**。
- 受背包剩余空间约束：`take all` 数量 = `min(floor(freeSpace/weight), numLeft)`；空间不足时弹 `drop:` 菜单让玩家丢别的物品腾位（`drawDrop` / `dropStuff`）。
- "take everything and leave" 复合钮：能全拿下时一次拿完并离开。

### 4.8 逃跑规则（重要区分）

- **随机遭遇（Encounters）没有逃跑**：`startCombat` 只画攻击钮 + 治疗钮，**没有 leave/flee 按钮**。一旦开打只能打赢或打死——移植时别自己加"逃跑"。
- **地标 setpiece 的分阶段战斗有 `run` / `leave` 钮**：如硫磺矿/煤矿在每个战斗阶段的胜利界面提供 `run`（`nextScene:'end'` 直接退出）或 `continue`（进下一阶段）。这是 setpiece 剧情钮，不是通用战斗逃跑。

---

## §5 地标 setpieces 摘要

出处 `setpieces.js`（`Events.Setpieces`）、`executioner.js`（`Events.Executioner`）。所有 setpiece 结构：`scenes` 里 `start` 起，`buttons` 用 `nextScene` 跳转（`nextScene: {概率阈值: 场景}` 用 `Math.random()` 选分支；`'end'` 收尾）。战斗场景内联自带 `health/damage/hit/attackDelay/loot`（**不复用 §4.2 敌人表**，`enemy` 字段只是内部 id，`chara` 是显示字符，enemy 名不显示）。

`clearDungeon()`（cave/town/city 的结算钮 `onLoad` 调用）：把当前格变 OUTPOST + 画路；矿/ship 用 `drawRoad()` + 置对应 `World.state.*` 标志。

| 地标 | scene key | 玩法概要 | 奖励要点 | 解锁村庄内容 |
|---|---|---|---|---|
| **前哨 outpost** | `outpost` | 安全点，一段文字即走 | 掉 cured meat 5–10；`useOutpost()` 把水灌满；**每个前哨仅能用一次**（按坐标标记 used，再踩无效果）| 无（由清地牢生成）|
| **铁矿 iron mine** | `ironmine` | `go inside` **需消耗 1 torch** → 打 beastly matriarch(HP10) → cleared | 掉 teeth/scales/cloth；`state.ironmine=true` | 回村 → `buildings["iron mine"]` → **iron miner 职业** |
| **煤矿 coal mine** | `coalmine` | 3 阶段战：man(HP10)×2 → chief(HP20) | 掉 cured meat/cloth/iron | 回村 → `buildings["coal mine"]` → **coal miner 职业** |
| **硫磺矿 sulphur mine** | `sulphurmine` | 军事据点，最难：soldier(HP50,远程)×2 → veteran(HP65) | 掉 cured meat/bullets/rifle/bayonet | 回村 → `buildings["sulphur mine"]` → **sulphur miner 职业** |
| **旧房子 house** | `house` | `go inside` 三分支：25% medicine（药 2–5）/25% supplies（灌水+杂物）/50% occupied（打 squatter HP10）| medicine / cured meat / leather / cloth | 无 |
| **洞穴 cave** | `cave` | 分支战斗地牢（beast/cave lizard）→ 3 种终点箱 | end3 出 **steel sword**；end2 出 steel/bolas/medicine；沿途掉 torch/iron sword 等 | 清完 → OUTPOST + 路 |
| **废弃小镇 town** | `town` | 更大战斗地牢（thug/scavenger/vigilante/madman/beast）| medicine / steel sword / rifle / bolas | 清完 → OUTPOST + 路 |
| **被毁城市 city** | `city` | 最大地牢（含大量分支，60+ 场景）| energy cell / laser rifle / grenade / alien alloy | 清完 → OUTPOST + 路 |
| **巨坑 borehole** | `borehole` | 一段文字直接捡 | **alien alloy 1–3@1** | 无 |
| **战场 battlefield** | `battlefield` | 一段文字直接捡 | rifle/bullets/**laser rifle**/energy cell/grenade/alien alloy | 无 |
| **沼泽 swamp** | `swamp` | `enter`→`cabin`→`talk`（**消耗 1 charm**）| **授予 `gastronome` perk**（吃肉回血 ×2）| perk（影响 World 生存）|
| **坠毁星舰 ship** | `ship` | `salvage` 一步 | `state.ship=true` | 回村 → `Ship.init()` = **Phase 3 飞船**（本期 defer）|
| **处刑者战舰 executioner** | `executioner-*`（executioner.js）| 多机翼大地牢（Engineering/Martial/Medical Wing + Command Deck）| 掉 **蓝图**：hypo / kinetic armour / plasma rifle / disruptor / glowstone / stim | 回村 → `Fabricator.init()` = **Phase 3**（本期 defer）|
| **被毁村庄 cache** | `cache` | prestige 专属，`Prestige.collectStores()` | 上一周目的库存 | 仅 prestige（本期 defer）|

**Phase 2 必做的经济相关 setpiece**：三矿（解锁 miner）、outpost（补给点）、house/cave/town/city/borehole/battlefield/swamp（战利品与 gastronome perk）。ship/executioner/cache 属 Phase 3，可先占位或不生成对应事件（但地图仍会布点 W/X/U）。

---

## §6 与村庄经济的接口

出处对照：上游 `outside.js._INCOME` / `outside.js`（`_BUILDINGS` job 映射，第 483-486 行）与本仓库 `src/game_data.h`。

### 6.1 现有 `game_data.h` 已就位的部分

Phase 1 的数据表已**预留了 Phase 2 的经济接口**：
- `Res` 枚举已含 `R_IRON / R_COAL / R_SULPHUR / R_COMPASS / R_BULLETS / R_MEDICINE / R_ENERGY_CELL / R_ALIEN_ALLOY`。
- `TRADE` 表已含 `compass`（fur400/scales20/teeth10，max1）。
- `INCOME` 表已含 `iron miner / coal miner / sulphur miner / steelworker / armourer`，且矿工产出与上游一致：
  - iron miner: `cured meat −1, iron +1`（delay 10）
  - coal miner: `cured meat −1, coal +1`
  - sulphur miner: `cured meat −1, sulphur +1`
  - steelworker: `iron −1, coal −1, steel +1`
  - armourer: `steel −1, sulphur −1, bullets +1`
- job→building 映射（`JobBuilding`）里三个 miner 现为 `BLD_NONE` 占位，注释已标 "mine, P2"。
- Item 表已含 `bone spear / iron sword / steel sword / rifle` 与三档护甲 `l/i/s armour`、`wagon/convoy/rucksack`、`torch/waterskin/cask/water tank`。

### 6.2 Phase 2 需要扩的部分

1. **矿山"building"标志**：上游把 `iron mine / coal mine / sulphur mine` 存在 `game.buildings[...]` 里，由 World 清矿后 `goHome` 置位；`outside.js._BUILDINGS`（`{'iron mine':['iron miner'], ...}`）据此解锁 miner 职业。本仓库需要新增一类"由 World 授予、非 craftable"的 building/flag（或扩 `Bld` 枚举 + 一个"不可建造"标记），并把 `JobBuilding` 里三个 miner 的 `BLD_NONE` 替换为对应矿山。**这是打通"探索→经济"闭环的核心改动**。
2. **World 携带用武器/道具进 Item 或独立表**：`bayonet / laser rifle / grenade / bolas` 是 Path 可携带的战斗物但 Phase 1 Item 表未含；需要为它们补上（含 `Path.Weight`：bone spear2/iron sword3/steel sword5/rifle5/laser rifle5/bolas0.5/bullets0.1/energy cell0.2）。`energy cell` 已在 `Res`。
3. **Perk 系统**：World 生存/战斗大量依赖 perk（gastronome/scout/slow metabolism/desert rat/precise/evasive/stealthy/boxer/martial artist/unarmed master/barbarian）。需确认 Phase 1 是否已有 `character.perks` 容器；至少 `gastronome`（沼泽）在 Phase 2 内可获得。可先实现少数几个，其余 perk 挂空。
4. **World 易失状态存储**（§0 难点 2）：`map`(61×61 char) / `mask`(61×61 bool) / `curPos` / `water` / `health` / `outfit` / `usedOutposts` / 各矿 `state.*` 标志。需与 `$SM` 持久层分离，`goHome` 才提交、`die` 直接丢。
5. **HP/水上限的高档位**（kinetic armour/fluid recycler/cargo drone）属 Fabricator（Phase 3），可先不接。
6. **compass → Path 解锁**的一行触发（§1.6），接进现有 Room 买入流程。

### 6.3 物品回流路径小结

- **compass**（交易站买）→ 开 Path。
- **矿石解锁**：清三矿 → 回村置矿山 building → miner 职业可雇 → 消耗 cured meat 产 iron/coal/sulphur → 喂 steelworks/armoury（Phase 1 已有建筑）产 steel/bullets → 造更强武器/护甲/子弹。
- **武器/材料**：地标/战斗掉落进 outfit → 回村入库（原料留家、武器留背包）。
- **gastronome perk**（沼泽 charm 换）→ 永久提升 World 续航。

---

## §7 文案清单与 zh_cn 覆盖度

### 7.1 官方 zh_cn 翻译存在且覆盖近乎完整

上游 `lang/zh_cn/strings.po` 是 gettext 文件，**793 条 msgid，仅 1 条空 msgstr（那条是 PO header 的 `msgid ""`，非漏译）**。抽检 Phase 2 全部关键串（Path/World/战斗/setpiece 文案、地形转换、罗盘 8 向、敌人遭遇/死亡文案、地标标题、UI 标签）**均已翻译**。key = 英文字面量（与代码 `_('...')` 一致），本仓库的 `strings_zh.h` + 字体 glyph 闭包可直接以这些 en→zh 对照为输入。

代表性对照（可直接用）：

| 英文 key | zh_cn |
|---|---|
| A Dusty Path | 漫漫尘途 |
| A Barren World | 荒芜世界 |
| embark | 出发 |
| supplies: | 供应: |
| free {0}/{1} | 背包剩余空间: {0}/{1} |
| hp: {0}/{1} | 生命: {0}/{1} |
| water:{0} | 水:{0} |
| pockets / rucksack | 背包 / 双肩包 |
| armour / water | 护甲 / 水 |
| none/leather/iron/steel/kinetic | none 有译；kinetic 未译（P3）|
| eat meat / use meds | 吃肉 / 服药 |
| leave / continue / run | 离开 / 继续 / （run 未单独抽查，见下）|
| take everything / take all you can | 拿走一切 / 拿走可带走的一切 |
| miss / stunned | 失手 / 惊吓 |
| nothing to take | 一无所获 |
| the compass points north…southeast | 罗盘指向北方…东南（8 向全译）|
| Wanderer / The Village | 漫游者 / 村庄 |
| The Iron/Coal/Sulphur Mine | 铁矿 / 煤矿 / 硫磺矿 |
| the mine is now safe for workers. | 矿井现在安全了 |
| the world fades | 眼前的世界烟消云散 |
| 武器 verb：punch/stab/swing/slash/thrust/shoot/blast/lob/tangle | 挥拳/戳刺/挥斩/劈砍/扎刺/开火/引爆/投掷/牵绊 |
| 敌人名 snarling beast/gaunt man/strange bird/shivering man/man-eater/scavenger/lizard/feral terror/soldier/sniper | 咆哮的野兽/憔悴的男子/怪鸟/颤抖的男子/食人怪/拾荒者/巨蜥/凶怪/士兵/狙击手 |
| 地标 label An Old House / A Damp Cave / An Abandoned Town / A Ruined City / A Crashed Starship / A Borehole / A Battlefield / A Murky Swamp | 旧房子/潮湿洞穴/废弃小镇/被毁的城市/坠毁星舰/巨坑/战场/迷雾沼泽 |

### 7.2 需要人工补译的缺口（少量）

抽检发现以下**属 Phase 2 但 zh_cn 未收**（推测为上游后加内容），实现时需补译并纳入 glyph 闭包：

- **Two-Headed Creature 遭遇**的 3 条串：标题 `A Two-Headed Creature`、通知 `a two-headed creature appears, the smaller head trembling`、死亡 `the two creatures are dead`（`grep two-headed` 在 po 中 0 命中）。这是唯一未译的 Phase 2 遭遇。

以下未译但**属 Phase 3（Fabricator/prestige），Phase 2 可忽略**：`boost`、`shield`、`use hypo`、`disintegrate`、`kinetic`、`A Ravaged Battleship`（处刑者）等。

### 7.3 固定 UI 串清单（供 glyph/排版排期）

除上表叙事文案外，Path/World/战斗界面的固定 chrome 串（英文 key，均已在 zh_cn）：

- **Path**：`A Dusty Path`、`supplies:`、`embark`、`perks`、`armour`、`water`、`none`、`leather`、`iron`、`steel`、`restores`、`hp`、`use with rifle`、`emits a soft red glow`、`free {0}/{1}`、`damage`、`weight`、`available`、`the compass points <dir>`（×8）。
- **World**：`A Barren World`、`{0}:{1}`、`water:{0}`、`pockets`、`rucksack`、`free {0}/{1}`、`hp: {0}/{1}`、`Wanderer`、`The Village` + 全部 landmark label；6 条地形转换叙事（`the trees yield to dry grass…` 等）；`dangerous to be this far from the village without proper protection` / `safer here`；`the meat has run out` / `starvation sets in` / `there is no more water` / `the thirst becomes unbearable`；`the world fades`；`water replenished`。
- **战斗**：12 个武器 verb；`eat meat` / `use meds`（+P3 的 `use hypo`/`boost`/`shield`）；`miss` / `stunned`；`leave` / `continue` / `run`；`take` / `all` / `take everything` / `take all you can` / ` and ` / `nothing` / `drop:` / `take:` / `nothing to take`；`*** EVENT ***`。
- **setpiece 剧情**：每个地标的 start/中间/cleared 文案与 button 文本（`go inside`/`enter`/`salvage`/`attack`/`talk`/`take` 等），均在 zh_cn（除 §7.2 缺口）。

---

## 附：抓取到的源文件行数（便于回溯）

`path.js` 341 · `world.js` 1109 · `events.js` 1487 · `encounters.js` 437 · `setpieces.js` 3587 · `executioner.js` 2343 · `outside.js` 665 · `room.js` 1259 · `zh_cn/strings.po` 3337。

引用格式示例：`world.js World.getMaxHealth()`、`path.js Path.getCapacity()`、`encounters.js Events.Encounters[Sniper]`、`setpieces.js Events.Setpieces.sulphurmine`。
