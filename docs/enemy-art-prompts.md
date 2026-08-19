# 战斗插画（敌人肖像）生成规格

`tools/gen_enemy_art.py` 把源图烘进 `src/enemy_art_data.h`。源图本身**不进仓库**
（`docs/enemy-art/` 已 gitignore），这份文档才是"图是怎么来的"的记录。

## 规格

| 项 | 值 |
|---|---|
| 目标画幅 | **492 × 420**（1.17:1，略高于正方形） |
| 建议源图 | **1024 × 874** 或更大，同比例 |
| 色彩 | 灰度（生成时按彩色出也行，管线会转 L 通道） |
| 存放 | `docs/enemy-art/<key>.png` |

## 构图铁律

**主体垂直居中，上下边缘留作可裁余量。**

战斗面板的插画高度会随玩家携带的武器数量收缩：常态给满 420px，但满装备
（10–12 个按钮）时会裁到 428px 以下，**从上下两侧对称裁掉**。所以：

- 敌人的头部和躯干必须落在**画面正中**
- 地面、天空、背景元素放在上下三分之一，被裁掉不影响识别
- 不要把关键特征（眼睛、武器、标志性部位）放在画面顶部或底部

## 画风：极简线条 + 线条权重层级（定稿）

- **线条少**，但每根线要有**粗细变化** —— 这是"好看"与"廉价"的分界
  - 粗线：外轮廓、剪影、暗部
  - 发丝细线：内部必要细节
  - **中间粗细一律不要**
- 收笔**渐变（taper）**，像毛笔，不要等宽的矢量描边
- **不要排线（crosshatching）**、不要毛发质感、不要肌肉起伏、不要灰调
- 纯白背景铺满整张画布
- 无边框、无文字、无签名

### 为什么强调"粗细层级"

第一版极简稿被判"太丑"，症结是**线条粗细均匀** —— 每根线一样粗，读起来就是矢量
贴纸。制版行话说得很准：均匀线宽的稿子 *"looks either dull or busy"*（要么呆板，
要么杂乱）。

写 prompt 时**不能只说 "bold confident outlines"** —— 模型会理解成"整体加粗"，
结果正是那种廉价描边。必须明确要求**同一张画里的权重对比**：

> a bold, confident, TAPERING outer contour carries the silhouette and the
> shadow side, while the few interior marks are HAIRLINE THIN — a clear weight
> hierarchy, never one uniform stroke width

剪影优先还有个实际好处：**缩到 492×420 仍然认得出**。内部细节在这个尺寸本来就看
不清，靠轮廓和姿态传达信息才是对的。

### SUBJECT 里一个材质词都不能出现

比 STYLE 的禁令更强的是 SUBJECT 里的**材质词**。给食人怪补描述时写了
`shaggy` / `matted fur`，出来的熊就是几千根毛丝画成的 —— 尽管同一个 prompt 里
STYLE 明写着 `no fur texture`。**主体行说什么，模型就画什么；风格行的否定压不住
主体行的肯定。**

已定稿的那几张，SUBJECT 里恰好一个材质词都没有。所以规矩是：

> SUBJECT 只写**是什么** + **什么姿态** + **几条肢**，绝不写表面材质
> （fur / muscles / scaled / shaggy / hide / skin / texture）。

体积感靠**剪影和实心黑**表达，不靠画表面。

**但不要靠加禁令去挡。** 试过把禁项逐个点名（`no muscle striation`、
`no feather detail`、`no cast shadow on the ground`），结果更糟：模型照画不误。
给 `glyph_t` 写的 `bulk conveyed by silhouette, absolutely NOT by drawing muscle
striation` 出来是一头满身肌肉纹的怪兽 —— 跟当年 `NOT Japanese, NOT samurai,
NOT katana` 照样出武士是同一回事：

> **否定压不过提及（negation does not beat mention）。有效的是只字不提
> （omission）。**

所以 STYLE 保持已验证的那版，**不再往里加禁令**；SUBJECT 里也不写"不要画肌肉"
这类话 —— 连"肌肉"两个字都不许出现。

### 两条从定稿图上读出来的硬规矩

翻车批和定稿批摆在一起看，差别不在画风参数，在**主体本身**：

**一、人物必须从头裹到脚。** 定稿的三个人（拾荒者、狙击手、士兵）全都埋在
大衣、斗篷、缠布里。翻车的憔悴男、颤抖的男子、双头怪都被我写成了裸露上身 ——
**露出皮肤就是在邀请模型画肌肉**，出来全是健美先生，跟"憔悴""虚弱"完全相反。
衣服盖住 = 没有肌肉可画 = 剪影自然干净。`scavenger` 那 105 词全是穿戴物件，
这才是它又长又干净的原因。

**二、动物必须锚定到真实物种。** 定稿的是狼、科莫多龙 —— 有参考可依。翻车的
`glyph_r`/`glyph_t` 我写的是 `predator`、`creature` 这种抽象词，模型直接掉进
**奇幻怪兽先验**：肌肉虬结、龙一样的背脊、灰调明暗。写 `hyena`、`monitor
lizard`、`brown bear`，它就去查参考而不是编怪物。

### 减墨要正面要"留白"，不要反面禁"排线"

食人怪一度是全场最脏的一张（43KB、纯白 65.3%，同为四足的巨蜥只要 19KB / 88.3%），
墨全花在背上那片毛发排线。禁令（`no fur texture`）拦不住 —— 但**正面描述想要的
留白**一次就成：

> drawn with VERY FEW strokes — its bulk carried by the **bold outline alone**,
> the body left **open and white inside**

结果 **43KB → 36KB、纯白 65.3% → 77.2%**，而且更像定稿那批（粗笔触 + 几块实心黑
压重量）。同一句话可以用在任何墨量超标的图上。

### 比喻会把参照物的解剖一起搬进来

双头怪的第二颗头一开始从**背上**长出来。为了修着生位置用了比喻，连翻两次车：

| 加的比喻 | 修好了 | 顺带毁了 |
|---|---|---|
| `like a two-headed snake` | 颈根移到肩前 ✓ | 脖子变成细长带环纹的**蛇颈** |
| `like a two-headed calf` | 脖子变粗 ✓ | 小头长出**牛耳朵和牛脸** |

**比喻是整包搬运的** —— 模型不会只取你想要的那一条属性。要控形态就**只写几何
关系**（"两条脖子从胸前并排长出、基部相接、短而粗"），再把物种钉死（"两颗都是
熊头，形状相同，一大一小"）。

### 结构画不出来时，先怀疑物种选错了

双头怪的第二颗头**连续九轮**长在躯干上（背、肩胛、腰），换遍了措辞、视角、
比喻都没解决：

- 加"分叉""对称""基部相接" → 依旧接在肩后
- 换正面视角 → 颈根终于对了，但姿态呆板；再配长颈就长出**第三个头**
- 长颈 V 形（好看）→ 放大看，小头的根还是在背上

根因不在 prompt，在**物种**：熊的脖子又短又埋在肩里，胸前根本没有可供分叉的
位置，模型只能往躯干上安。改成**爬行类**后一次就成 —— 长颈从胸前伸出是蜥蜴/龙的
正常解剖，V 形分叉不用强求，自己就出来了。

> 一个结构反复画不对，多半不是描述不够狠，而是**你要求的解剖在这个物种身上
> 本来就不存在**。换物种比加形容词有效。

（代价：上游给它掉 `FUR`，爬行类掉毛皮说不通 —— 但它是变异生物，且这个反差
远不如"第二颗头长在背上"扎眼。）

### 改一个毛病会挤掉上一个

同一张图连改四轮，每轮都是"修好新的、退回旧的"：加长脖子 → 表情反差没了；
换比喻 → 着生点好了但脖子成了蛇；改粗脖子 → 颈根又爬回背上。

原因是**每轮只写新要求，旧要求就被挤出去了**。已经调对的约束必须每次**原样带上**，
prompt 只增不减 —— 它不是对话，模型不记得上一版做对了什么。

### 长度不是问题（别乱归因）

中途一度以为"描述越长画得越满"，想把所有 SUBJECT 砍到 30 词。数一下就知道不对：
已定稿的 `scavenger` 是 **105 词**、`sniper` 75 词、`feral_terror` 54 词，都比翻车的
`glyph_t`（70 词）长。`scavenger` 那 105 词全是**穿戴物件**（油布斗篷、焊接面罩、
废料袋），画出来仍是干净的极简线条。

**决定画面繁简的是词的种类，不是词的数量** —— 物件可以尽情写，材质一个字都不能写。

### 为什么不能用排线

插画以 4bpp + RLE 存进固件，**体积由"有多少像素保持纯白"决定**。排线和满幅背景是
RLE 的天敌：

| 画风 | 单张 | 压缩率 | 纯白占比 |
|---|---|---|---|
| 满幅排线（第一版） | 89KB | 1.17x | ~27% |
| 要求纯白底 | 52KB | 1.99x | 69.0% |
| **极简线条（定稿）** | **29KB** | **3.38x** | **83.7%** |
| *事件插图 `ev_nomad` 参考* | *9KB* | *7.30x* | *91.2%* |

16 张合计约 **480KB**。仍高于事件插图那批（210KB），差额来自构图取向 —— 事件插图是
"小小的人 + 大片荒原"，这批是**满幅肖像**，墨量注定更高。这是刻意的取舍。

## Prompt 模板（定稿）

```
Minimalist black-and-white line illustration of {SUBJECT}. Drawn with FEW,
clean, confident strokes — an economical sketch, not a detailed rendering.
No fur texture, no muscle definition, no crosshatching, no stippling, no
grey tones. Solid pure-white background, completely flat and uniform,
filling the whole canvas; no paper texture, no scan, no photograph, no
sheet, no frame, no border, no shadow. The subject alone, centred, facing
the viewer, occupying the middle of the frame. Sparse woodblock-print
simplicity. No text, no signature.
```

那一长串否定项是有来历的：模型很爱把画**当成一张放在桌上的纸**来渲染（连纸边和投影
一起画出来），那圈灰地板既压不动又会在墨水屏上糊成一片。

## 白点归一（管线自动处理）

生成的图是"一张画的照片"，白底其实是 246–252 之间跳动的噪点。量化后这些噪点在
level 14/15 之间反复横跳，RLE 一个字节都压不掉 —— 因为游程只能覆盖**相等**的字节。

`gen_enemy_art.py` 的 `WHITE_POINT = 225` 在量化前把近白钳到纯白，这一步把参考图从
27% 纯白拉到 84%、体积从 96KB 降到 30KB，**肉眼看不出任何差别**（被它移动的像素在
16 阶屏上本来就显示为白）。

出图时不必自己处理，管线会做。

## 主体清单

野外遭遇 11 张（有名字，玩家最常遇到）：

| 文件名 | 敌人 | SUBJECT |
|---|---|---|
| `snarling_beast.png` | 咆哮的野兽 | a snarling feral beast lunging from undergrowth |
| `gaunt_man.png` | 憔悴的男子 | a gaunt ragged man with a crazed stare |
| `strange_bird.png` | 怪鸟 | a strange large flightless bird, sharp beak |
| `two_headed.png` | 双头怪 | a two-headed **beast**（熊形，非犬科）—— 见下 |
| `shivering_man.png` | 颤抖的男子 | a shivering man attacking with surprising strength |
| `man_eater.png` | 食人怪 | a large man-eating creature, claws freshly bloodied |
| `scavenger.png` | 拾荒者 | a scavenger in scrap armour, crouched to strike |
| `lizard.png` | 巨蜥 | a giant lizard, scaled hide, low stance |
| `feral_terror.png` | 凶怪 | a feral terror bursting from the trees, all teeth |
| `soldier.png` | 士兵 | a soldier in tattered military gear, rifle raised |
| `sniper.png` | 狙击手 | a sniper taking aim from cover |

### 双头怪画兽，不画人

定为**兽形**（熊形，避开犬科）。两条依据：

- **上游数据说它是野兽** —— `combat_data.h` 里它掉 `FUR` / `TEETH` / `MEAT`，
  跟咆哮的野兽、食人怪同一类掉落，人形敌人掉的是 `CLOTH` / `LEATHER`。
- **人形画不出来** —— `TWO HEADS on one pair of shoulders` 连试六张，每张都渲染成
  **两个并排的人**，humanoid 先验压不住。改成"右肩上长出第二颗头"才画对，但方向
  本来就错了。

兽形反而一次成：双头动物（双头蛇、双头牛）有大量真实参考可依。

setpiece 兜底 **4** 张（按 `Combat.enemyChara` 索引，无名字）：

| 文件名 | 字形 | 上游台词（`setpieces_data.h`） | SUBJECT |
|---|---|---|---|
| `glyph_r.png` | R | *a startled beast defends its home* / *a large beast charges out of the dark* | 护巢的穴居兽，四足低伏前扑 |
| `glyph_e.png` | E | *a man charges down the hall, a rusty blade in his hand* | 举锈刀冲锋的褴褛男子 |
| `glyph_t.png` | T | *a large creature lunges* / *a giant lizard shambles forward* | 沉重的大型猛兽扑击 |
| `glyph_d.png` | D | *a grizzled soldier attacks, waving a bayonet* | 持刺刀步枪的老兵 |

**没有 `glyph_k`。** 带 `K` 的敌人只有双头怪，那是随机遭遇，走 `ENEMY_ART[id]`
拿专属图，永远到不了兜底表 —— 画了也是死图，白占 30KB。

### 兜底图必须照台词画

第一版这 4 张（外加当时那张 K）被判"太丑"，症结不在画风参数，而在 SUBJECT 是我
按字形**猜**的（"a misshapen aberrant creature"、"a hulking brute"）。描述一笼统，
模型就退回它最熟的**通用奇幻怪物库存图**：肌肉块、灰调渐变、张臂站桩、脚下投影 ——
全是 STYLE 明令禁止的东西，跟已定稿的野外那批完全不是一路。

`setpieces_data.h` 里每个字形都带着上游原文，照着写就具体了，不必猜。

## 自检：每张都要放大数一遍

模型在**数量**上不可靠，而且错法不止"多一条腿"：

| 实际翻车 | 表现 |
|---|---|
| 多出整个人 | `gaunt_man` 画了两个男人；`glyph_e` 主体肩后多出一张脸 |
| 该有的没有 | `two_headed` 画成两个人重叠，不是一身两头 |
| 不该有的长出来 | `glyph_t` 巨蜥长了两个头 |
| 肢体数错 | 早期的 5 条腿食人怪、3 条手颤抖男 |

对策是**在 SUBJECT 里正面写死数量**（`Exactly ONE person, TWO arms, TWO legs,
ONE head`；一身两头则写 `ONE body, ONE torso, ONE pair of shoulders, from which
TWO NECKS rise`），而不是事后挑。

**动态姿态词会长肢体。** 给憔悴的男子写 `LONG LUNGING STRIDE, body low, coat
tails flying` 之后，连出三张**三条腿**（配三只靴子）—— 飘起的衣摆在大跨步姿态下
被补全成了一条腿。同一个人物写朴素的 `sprinting` 从不出这问题。所以：

- 动势优先靠**视角**（`THREE-QUARTER VIEW`）拿，别堆姿态副词
- 数量约束要连**鞋/靴**一起数（`TWO legs, TWO boots`）—— 错误就是以"第三只靴子"
  的形式出现的，只数腿数不出来

**验收必须看原图，不能只看缩略图。** `glyph_e` 肩后那张脸在 246px 的对照表里
完全看不出来，放大才发现 —— 缩略图只够筛画风，数数得看原图。每个主体一次出
2–3 张候选，逐张数完再选。

## 流程

```bash
# 图放进 docs/enemy-art/ 之后
python tools/gen_enemy_art.py          # 有源图的替换掉 placeholder，缺的继续占位
python tools/gen_enemy_art.py --preview-only   # 只看效果，不写 header
```

预览（16 灰阶量化后的实际上屏效果）写到 `docs/enemy-art/final/`。
**可以一张张来**，不必等 16 张齐了再跑。
