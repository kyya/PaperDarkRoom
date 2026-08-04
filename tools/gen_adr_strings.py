#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# gen_adr_strings.py — A Dark Room official translation table -> C header.
#
# Reads the upstream official Simplified-Chinese table
# lang/zh_cn/strings.js (a single `_.setTranslation({en: zh, ...})` flat map,
# 786 entries, 100% coverage) and emits src/strings_zh.h: a PROGMEM
# array of {en_key, zh} pairs SORTED BY en_key so the firmware can binary-
# search it, plus a `const char* tr(const char*)` declaration. Code refers to
# strings by their English key (e.g. tr("light fire")); zh values are stored
# UTF-8 and rendered by the CJK sparse-bitmap font (see gen_cjk_font.py).
#
# The whole game text corpus lives in this one table on purpose (research.md
# §8.3 "铁律"): the CJK glyph closure is extracted from these values, so any
# hard-coded Chinese elsewhere would silently drop glyphs (tofu boxes).
#
# lang/zh_cn/strings.js is NOT part of this repo — it comes from a local
# clone of the upstream game (doublespeakgames/adarkroom); point --in at it.
#
# Usage:
#   gen_adr_strings.py --in path/to/adarkroom/lang/zh_cn/strings.js --out src/strings_zh.h
from __future__ import annotations

import argparse
import json
import os
import re


def parse_strings_js(path: str) -> dict[str, str]:
    """Extract the single _.setTranslation({...}) object as a Python dict."""
    text = open(path, encoding="utf-8").read().strip()
    m = re.search(r"setTranslation\(\s*(\{.*\})\s*\)\s*;?\s*$", text, re.S)
    if not m:
        raise SystemExit(f"could not find _.setTranslation({{...}}) in {path}")
    return json.loads(m.group(1))


# ---- §8.3 glyph-closure overrides -----------------------------------------
# A handful of upstream zh values use characters the 12px source font has NO
# glyph for (Fusion Pixel AND its OFL sibling Ark Pixel both genuinely lack them
# at 12px, in every variant / the latest release — verified). Rendered on-device
# they would ship as .notdef "tofu" boxes, breaking the §8.3 glyph-closure iron
# law. Since no OFL 12px Simplified-Chinese pixel font carries these glyphs, the
# affected official translations are MINIMALLY reworded to an equivalent phrasing
# whose every character IS in the font — done HERE at the pipeline source so
# strings_zh.h stays a pure generated artifact (never hand-edited). Keyed by
# en_key so an override tracks its upstream row; a key that no longer exists
# upstream hard-errors (guards against silent drift). Keep this list as small as
# possible and rerun gen_cjk_font.py afterward — it fail-closes on any remaining
# tofu, proving the closure is whole.
#   辘 (U+8F98) in 饥肠辘辘  ·  藓 (U+85D3) in 苔藓
STRING_OVERRIDES = {
    # "she looks hungry." — 饥肠辘辘 -> 很饿 (also closer to the literal English)
    "builder finishes the smokehouse. she looks hungry.":
        "建造者造好了熏肉房。她看起来很饿。",
    # moss = 苔藓 -> 青苔 (an exact synonym, both chars in the font)
    "deep in the swamp is a moss-covered cabin.":
        "沼泽深处现出一栋覆满青苔的小屋",
    "the walls are moist and moss-covered":
        "岩壁潮湿，覆盖着青苔",
}


# ---- LOCAL append table (keys upstream never had) -------------------------
# DISTINCT from STRING_OVERRIDES above (which reword an EXISTING upstream row):
# these keys do NOT exist in the upstream flat translation map at all, so there is
# no official translation to inherit. This is the ONE sanctioned deviation from
# the "official-translation only" principle — a small set of Phase-2 strings the
# firmware needs that upstream keys differently (or never surfaced as flat _()
# msgids). Each entry is asserted ABSENT from upstream at generation time (below),
# so if a future upstream sync adds an official translation for one, the build
# HARD-ERRORS here — forcing us to drop the local and adopt the official wording
# (guards against silently shadowing an upstream string). Keep this list minimal.
#
#   1) The 8 World.LANDMARKS[].label map tooltips — upstream joins them with
#      &nbsp; ("Iron&nbsp;Mine"), so the bare labels the port renders as the
#      map/HUD hint never entered the zh_cn flat map. Wording aligned with the
#      matching setpiece TITLE already in zh_cn (The Iron Mine -> 铁矿, etc.).
#   2) The Two-Headed Creature encounter's 3 strings — the sole Phase-2 random
#      enemy absent from the official zh_cn set (research-phase2.md §7.2).
LOCAL_STRINGS = {
    "Iron Mine":            "铁矿",
    "Coal Mine":            "煤矿",
    "Sulphur Mine":         "硫磺矿",
    "An Abandoned Town":    "废弃小镇",
    "A Crashed Starship":   "坠毁星舰",
    "A Borehole":           "巨坑",
    "A Battlefield":        "战场",
    "A Ravaged Battleship": "被摧毁的战舰",
    "two-headed creature":  "双头怪",
    "a two-headed creature appears, the smaller head trembling":
        "一只双头怪出现了，较小的那颗头在颤抖",
    "the two creatures are dead": "两只怪物都倒下了",

    # ======================================================================
    #   3) Phase 3c (Executioner + Fabricator) — 233 keys.
    #      The upstream zh_cn table predates the Executioner expansion, so its
    #      188 executioner.js strings, fabricator.js's 23 and space.js's `wait`
    #      have NO official translation to inherit; the rest are keys this port
    #      needs that upstream never routed through _() at all (enemy display
    #      names, the blueprint item names, world.js's two un-wrapped goHome
    #      notifications, and the combat buttons 3c-1 added).
    #      Wording is the docs/i18n-phase3c-draft.md v1 ruling: OFFICIAL WINS —
    #      every term that has an upstream Chinese counterpart uses it verbatim
    #      (工坊 / 病房 / 武装力量 / 凶怪 / 巢穴 / 静谧 / 空空如也 …), no new
    #      coinage competes with one, and only genuinely new nouns are invented.
    #      House style follows the official corpus: no sentence-final 。
    #      Three keys are byte-for-byte upstream typos and MUST stay that way —
    #      `ship\u2019s` (a curly apostrophe), `what appears the be`, `somtimes`.
    # ======================================================================

    # -- Executioner shared enemy table (executioner.js:1-115) --
    "tripped a motion sensor.": "触发了移动感应器",
    "mechanical guard": "机械卫兵",
    "a mobile defence platform trundles around the corner.": "一台移动防御平台从拐角转了出来",
    "mechanical quadruped": "机械四足",
    "a medical drone wheels out of control.": "一台医疗机械失控地冲了过来",
    "broken medic": "损毁医疗机",
    "one of the defence turrets still works.": "有一座防御炮塔还能用",
    "defence turret": "防御炮塔",

    # -- executioner-intro — the prologue (14 scenes) --
    "the remains of a huge ship are embedded in the earth.": "一艘巨舰的残骸嵌在地里",
    "the remains of a massive battleship lie here, like a silent sealed city.":
        "一艘庞大战舰的残骸躺在这里，像一座沉静的封闭城市",
    "it lists to the side in a deep crevasse, cut when it fell from the sky.":
        "它斜插在一道深沟里，那是它坠落时切开的",
    "the hatches are all sealed, but the hull is blown out just above the dirt, providing an entrance.":
        "舱门全都封死了，但贴近地面的船身被炸开一个口子，可以进去",
    "the interior of the ship is cold and dark. what little light there is only accentuates its harsh angles.":
        "船内又冷又暗。仅有的一点光只让那些冷硬的边角更分明",
    "the walls hum faintly.": "墙壁发出轻微的嗡鸣",
    "thick, sticky webbing covers the walls of the corridor.": "又厚又黏的蛛网覆满走廊的墙壁",
    "deeper into the ship, the darkness seems almost to writhe.": "再往船内深处，黑暗几乎在蠕动",
    "a small knapsack hangs from a cluster of webs, a few feet from the floor.":
        "一只小背包挂在一片蛛网上，离地不高",
    "a huge arthropod lunges from the shadows, its mandibles thrashing.":
        "一只巨大的节肢生物从阴影中扑出，口器开合不停",
    "the webs part, and a grotesque insect lurches forward.": "蛛网分开，一只怪异的巨虫摇晃着扑来",
    "an operative waits in ambush around the corner.": "一名特工在拐角处埋伏",
    "the military has set up a small camp just inside the ship.": "武装力量在船内不远处扎了个小营地",
    "crude attempts have been made to cut into the walls.": "有人粗暴地试着切开墙壁",
    "scraps of copper wire litter the floor.": "地上散落着铜线碎片",
    "two bedrolls are wedged into a corner.": "两个睡袋嵌在角落里",
    "a dusty researcher clumsily hides in the shadows.": "一名满身尘土的研究员笨拙地躲在阴影里",
    "debris is stacked in the corridor, forming a low barricade.": "残骸堆在走廊里，垒成一道矮路障",
    "the walls are scorched and melted.": "墙壁被烧焦、烧化了",
    "behind the barricade, a few weapons lay abandoned.": "路障后面丢着几件武器",
    "the partially devoured remains of several wanderers are piled before a dark corridor.":
        "几具被啃食过半的流浪者尸骸堆在一条黑暗走廊前",
    "shuffling noises can be heard from within.": "嘈杂声从里面传来",
    "ancient beast": "远古巨兽",
    "an ancient beast has made these ruins its home.": "一头远古巨兽把这片废墟当成了巢穴",
    "a maintenance panel is embedded in the wall next to a large sealed door.":
        "一扇密封大门旁的墙上嵌着一块检修面板",
    "perhaps the ship’s systems are still operational.": "也许这艘船的系统还能运作",
    "power cycle": "重启电源",
    "as the lights come online, so too do the defence systems.": "灯亮起来的同时，防御系统也醒了",
    "automated turret": "自动炮塔",
    "beyond the bulkhead is a small antechamber, seemingly untouched by scavengers.":
        "隔舱后面是一间小前厅，看来拾荒者没来过",
    "a large hatch grinds open, and the wind rushes in.": "一扇大舱门吱嘎打开，风灌了进来",
    "a strange device sits on the floor. looks important.": "地上放着一台古怪的装置。看上去很重要",
    "take device and leave": "取走装置离开",

    # -- executioner-antechamber — the elevator hall --
    "a large hatch opens into a wide corridor.": "一扇大舱门通向一条宽走廊",
    "the corridor leads to a bank of elevators, which appear to be functional.":
        "走廊尽头是一排电梯，看起来还能用",
    "engineering": "工程区",
    "medical": "医疗区",
    "martial": "军事区",
    "command deck": "指挥甲板",

    # -- executioner-engineering — Engineering Wing (21 scenes) --
    "Engineering Wing": "工程区",
    "elevator doors open to a blasted corridor. debris covers the floor, piled into makeshift defences.":
        "电梯门打开，外面是一条被炸毁的走廊。地上满是残骸，堆成临时工事",
    "emergency lighting flickers.": "备用灯忽明忽暗",
    "an automated assembly line performs its empty routines, long since deprived of materials.":
        "一条自动装配线空转着重复流程，原料早已断供",
    "its final works lie forgotten, covered by a thin layer of dust.": "它最后的成品被遗忘在那里，覆满一层薄灰",
    "assembly arms spin wildly out of control.": "装配臂失控地疯狂挥转",
    "unruly welder": "失控焊机",
    "assembly arms spark and jitter.": "装配臂打着火花抽动",
    "a cacophony of decrepit machinery fills the room.": "破旧机械的嘈杂声充满整个房间",
    "must have been the engine room, once. the massive machines now stand inert, twisted and scorched by explosions.":
        "这里想必曾是引擎室。巨大的机器如今一动不动，被爆炸扭曲烧焦",
    "the destruction is uniform and precise.": "破坏得齐整而精准",
    "bits of them can be scavenged.": "还能从里面捡些零件",
    "none of the ship's engines escaped the destruction.": "这艘船的引擎无一幸免",
    "it's no mystery why she no longer flies.": "它飞不起来了，不难理解",
    "sparks cascade from a reactivated power junction, and catch.": "重新通电的接线盒溅下火花，烧了起来",
    "the flames fill the corridor.": "火焰塞满了走廊",
    "extinguish": "灭火",
    "rush through": "冲过去",
    "rows of inert security robots hang suspended from the ceiling.": "成排的保安机器人静止地挂在天花板上",
    "wires run overhead, corroded and useless.": "头顶的线缆生锈了，早已无用",
    "more signs of past combat down the hall. guard post is ransacked.": "走廊那头还有交火的痕迹。哨位被翻遍了",
    "still, some things can be found.": "不过仍然能找到些东西",
    "marks on the door read 'research and development.' everything seems mostly untouched, but dead.":
        "门上标着“研发”。里面几乎没被动过，但已经死透了",
    "one machine thrums with power, and might still work.": "有一台机器还在通电嗡响，也许还能用",
    "use machine": "使用机器",
    "step inside, and the machine whirs. muscle and bone reknit. good as new.":
        "走进去，机器嗡嗡运转。血肉与骨骼重新接合。像新的一样",
    "the machines here look unfinished, abandoned by their creator. wires and other scrap are scattered about the work benches.":
        "这里的机器都没完工，被造它们的人丢下了。工作台上散落着线缆和废料",
    "experimental plans cover one wall, held by an unseen force.": "一整面墙贴着实验图样，被某种看不见的力量固定着",
    "this one looks useful.": "这一张看起来有用",
    "clattering metal and old servos. something is coming...": "金属碰撞，旧机件嘎吱作响。有东西过来了……",
    "fight": "战斗",
    "an unfinished automaton whirs to life.": "一台未完成的机械体嗡地启动",
    "unstable prototype": "不稳定原型机",
    "at the back of the workshop, elevator doors twitch and buzz.": "工坊后面，电梯门抽动着嗡响",
    "looks like a way out of here.": "看起来像是条出路",

    # -- executioner-martial — Martial Wing (27 scenes) --
    "Martial Wing": "军事区",
    "metal grinds, and the elevator doors open halfway. beyond is a brightly lit battlefield. remains litter the corridor, undisturbed by scavengers.":
        "金属刮响，电梯门只开了一半。外面是一片灯火通明的战场。走廊里到处是遗骸，拾荒者没有动过",
    "looks like they tried to barricade the elevators.": "看来他们试过用路障堵住电梯",
    "further along, the corridor branches.": "再往前，走廊分成两条",
    "the door to the left is sealed and refuses to open.": "左边的门封死了，打不开",
    "blow it down": "炸开它",
    "continue right": "走右边",
    "the blast throws the door inwards.": "爆炸把门掀进屋里",
    "through the bulkhead is a large room, walls lined with weapon racks. fighting seems to have passed it by.":
        "隔舱后面是一间大屋，墙上排满武器架。战火似乎绕开了这里",
    "another door at the end of the hall, sealed from this side.": "走廊尽头还有一扇门，从这边封着",
    "should be able to open it.": "应该能打开",
    "the corridor is eerily silent.": "走廊静谧得反常",
    "crew cabins flank the hall, devoid of life.": "走廊两侧是船员舱，没有活物",
    "a few useful items can be scavenged.": "能捡到几件有用的东西",
    "ruined defence turrets flank the corridor.": "走廊两侧是损毁的防御炮塔",
    "could put the scrap to good use.": "这些废料还能用上",
    "small sensors in the walls still look to be operational.": "墙里的小型感应器看来还在工作",
    "easily avoided.": "很好躲开",
    "large barricades bisect the corridor, scorched by weapons fire.": "巨大的路障把走廊拦成两段，被火力烧焦",
    "bodies litter the ground on either side.": "两侧的尸体散落在地上",
    "documents are scattered down the hall, most charred and curled.": "文件散落一路，多数烧焦卷曲",
    "this one looks interesting.": "这一份看着有意思",
    "the next door leads to a ransacked planning room.": "下一扇门通向一间被翻乱的作战室",
    "maps of the surface can still be found amongst the debris.": "残骸里还能翻出地面的地图",
    "scavenge maps": "搜集地图",
    "drew some attention with all that noise.": "那阵动静引来了注意",
    "slipped past an automated sentry.": "绕过了一台自动哨兵",
    "if only they'd been destroyed along with everything else.": "要是它们也跟着一起毁了就好了",
    "ran straight into another one.": "又撞上一台",
    "the corridor passes through a security checkpoint. the defences are blown apart, ragged edges scorched by laser fire.":
        "走廊穿过一处安检站。防御工事被炸得粉碎，参差的断口被激光烧焦",
    "past the checkpoint, banks of containment cells can be seen.": "安检站后面能看到成排的收容室",
    "the cells are all empty.": "收容室空空如也",
    "power cables running across the ceiling are split in several places, sparking occasionally.":
        "横过天花板的电缆有几处断裂，不时冒出火花",
    "the guards died at their posts, shot through with superheated plasma.": "卫兵死在原位上，被高热等离子打穿",
    "their weapons lie on the floor beside them.": "他们的武器就落在身旁的地上",
    "the corridor opens onto a vast training complex, obstacles and features blackened by real combat.":
        "走廊通向一座巨大的操练场，场上的设施都被实战熏黑",
    "a regenerative machine hums uncannily by one of the courses.": "一台再生机器在其中一条场道旁怪异地嗡响",
    "motion from the centre of the yard.": "场地中央有动静",
    "a sparring automaton, still fully function and crusted with timeworn blood, lunges forward.":
        "一台陪练机械体扑了上来，功能完好，结着经年的血迹",
    "engage": "接战",
    "the machine attacks, blades whirling.": "机器发起攻击，刀刃飞旋",
    "murderous robot": "杀戮机器人",
    "the ruins of the sparring machine clatter to the ground.": "陪练机械的残骸散落一地",
    "picked this deck clean.": "这一层已经清扫干净了",

    # -- executioner-medical — Medical Wing (31 scenes) --
    "Medical Wing": "医疗区",
    "elevator doors open to an empty corridor.": "电梯门打开，外面是一条空走廊",
    "a few dusty corpses can be seen further down, but this deck appears to have been spared most of the combat.":
        "远处有几具积灰的尸体，但这一层似乎躲过了大部分战斗",
    "past the checkpoint, the corridor is undamaged save for sporadic graffiti.":
        "过了检查站，走廊完好，只有零星涂鸦",
    "there was no fighting here.": "这里没有交火",
    "automated guardians still stalk the halls, unaware that their masters have long gone.":
        "自动守卫仍在走廊里游荡，不知道主人早已离开",
    "clumsy machines, and easily avoided.": "笨拙的机器，很好躲开",
    "medical gurneys are fixed to grooves running down the corridor walls.": "医疗推车嵌在走廊墙上的滑槽里",
    "the automated patient transport system now sits motionless.": "自动转运系统如今一动不动",
    "it had friends.": "它不止一台",
    "more medical robots stand frozen, attached by a network of wires.": "更多医疗机器人僵在原地，被一张线网连着",
    "they take no notice of the intrusion.": "它们没有理会闯入者",
    "weapons are strewn about the medical dispatch bay. must have been used as a muster point.":
        "武器散落在医疗调度舱里。这里想必被当成过集结点",
    "more strange graffiti adorns the walls.": "墙上还有更多古怪的涂鸦",
    "this ward has been converted to a makeshift strategy room, maps scrawled hastily on any flat surface.":
        "这间病房被当成临时作战室，所有平面上都草草涂着地图",
    "a secure locker is set into one wall.": "一面墙里嵌着一个保险柜",
    "force locker": "破开柜子",
    "hinges rusted through. no challenge.": "锁扣早锈穿了。不费力",
    "the noise draws attention.": "声响引来了注意",
    "better to move without drawing attention.": "最好别引起注意",
    "noises can be heard from the corridor outside.": "外面走廊传来声响",
    "something's wrong with this robot.": "这台机器人不太对劲",
    "unstable automaton": "不稳定机械体",
    "another checkpoint ahead, fitted with heavy doors.": "前面又是一处检查站，装着厚重的门",
    "security is even tighter here.": "这里的防守更严",
    "slipped through unnoticed.": "没被发现就过去了",
    "air whistles as the doors open. this section must have lower pressure than the rest of the ship.":
        "门打开时空气呼啸。这一段的气压想必比船上别处低",
    "the air is cooler here. low cabinets ring the room, doors dusted with frost.":
        "这里的空气更冷。矮柜绕墙排了一周，柜门覆着一层霜",
    "samples of something biological inside.": "里面是某种生物样本",
    "security drones still patrol the hallways.": "保安机仍在走廊里巡逻",
    "predictable paths.": "路线固定",
    "surgical tools are scattered on the floor, near what appears the be the remains of a fire.":
        "手术器械散落在地上，旁边像是一堆烧过的余烬",
    "strange.": "古怪",
    "the air in this room has a metallic tinge. floor is covered in dark powder.":
        "这间屋里的空气带着金属味。地上铺着一层黑色的粉",
    "some completed explosives in the corner.": "角落里有些做好的炸药",
    "containment cells arranged at the back of the room, all open.": "房间后侧排着收容室，全都开着",
    "something moving up ahead.": "前面有东西在动",
    "a mutated beast leaps from its cell.": "一头变异的野兽从收容室里蹿了出来",
    "malformed experiment": "畸形实验体",
    "the creature's tortured breathing ceases.": "那生物痛苦的呼吸停了",
    "nothing more here.": "这里没别的了",

    # -- executioner-command — Command Deck (9 scenes, the boss) --
    "Command Deck": "指挥甲板",
    "the path to the command bridge is wide, walls adorned with decorative shields.":
        "通往指挥台的路很宽，墙上挂着一排排的盾",
    "fighting hadn't reached here, it seems.": "看来战火没烧到这里",
    "detour through the officer's lounge.": "绕道穿过军官休息室",
    "might be something useful here.": "这里也许有有用的东西",
    "small weapons cache in a cabinet.": "柜子里有一小堆武器",
    "lucky.": "运气不错",
    "found some medical supplies in a discarded bag.": "在一个丢弃的包里找到些医疗补给品",
    "the command deck is empty, save for a squat figure sitting motionless in the centre of the room.":
        "指挥甲板空无一人，只有一个矮小的身影一动不动地坐在房间中央",
    "in a flash, the figure is standing.": "一转眼，那身影已经站起",
    "approach": "靠近",
    "wanderer form, but not quite flesh. not quite metal either. a crystal set into its chest pulses with light.":
        "流浪者的形体，却不太像血肉。也不太像金属。胸口嵌着的一块水晶随光脉动",
    "it says it saw the rebellion coming. said it made arrangements.": "它说它早看见叛乱要来。说它做了安排",
    "says it can't die.": "说它不会死",
    "observe": "察看",
    "the immortal wanderer attacks.": "不死流浪者发起攻击",
    "immortal wanderer": "不死流浪者",
    "the crystal pulses brightly, then goes dark. the assailant shimmers as its shape becomes less defined.":
        "水晶亮起，随即熄灭。那袭击者闪烁着，形体渐渐模糊",
    "then it is gone.": "然后它就消失了",
    "time to get out of here.": "是时候离开这里了",

    # -- Fabricator (fabricator.js) — the 3c-3 page, translated up front --
    "Fabricator": "制造机",
    "A Whirring Fabricator": "嗡鸣的制造机",
    "fabricate:": "制造:",
    "blueprints": "蓝图",
    "energy blade": "能量刃",
    "the blade hums, charged particles sparking and fizzing.": "刀刃嗡鸣，带电的火星不停爆开",
    "fluid recycler": "流体回收器",
    "water out, water in. waste not, want not.": "水出去，水回来。不浪费，就不会缺",
    "cargo drone": "货运无人机",
    "the workhorse of the wanderer fleet.": "流浪者舰队的苦力",
    "kinetic armour": "动能护甲",
    "wanderer soldiers succeed by subverting the enemy's rage.": "流浪者士兵靠反转敌人的怒火取胜",
    "disruptor": "干扰器",
    "somtimes it is best not to fight.": "有时候最好别打",
    "hypo": "针剂",
    "a handful of hypos. life in a vial.": "一把针剂。一小管里装着命",
    "stim": "兴奋剂",
    "sometimes it is best to fight without restraint.": "有时候最好毫无保留地打",
    "plasma rifle": "等离子步枪",
    "the peak of wanderer weapons technology, sleek and deadly.": "流浪者武器技术的顶点，光滑而致命",
    "the familiar hum of wanderer machinery coming to life. finally, real tools.":
        "流浪者机械醒来时熟悉的嗡鸣。终于有真正的工具了",

    # -- space.js gap --
    "wait": "等待",

    # -- port-only keys: upstream never routed these through _() --
    "chitinous horror": "甲壳凶怪",
    "chitinous queen": "甲壳母虫",
    "operative": "特工",
    "researcher": "研究员",
    "blueprints feed into the fabricator data port. possibilities grow.":
        "蓝图接入制造机的数据口。能做的东西变多了",
    "builder knows the strange device when she sees it. takes it for herself real quick. doesn't ask where it came from.":
        "建造者一眼就认出了那台古怪的装置，很快收下，不管它从何而来",
    "fleet beacon": "舰队信标",
    "hypo blueprint": "针剂蓝图",
    "kinetic armour blueprint": "动能护甲蓝图",
    "plasma rifle blueprint": "等离子步枪蓝图",
    "disruptor blueprint": "干扰器蓝图",
    "stim blueprint": "兴奋剂蓝图",

    # -- combat UI added by 3c-1 (world.js World.Weapons / events.js buttons) --
    "disintegrate": "瓦解",
    "slice": "切割",
    "stun": "电击",
    "use hypo": "注射",
    "use stim": "亢奋",
    "boost": "亢奋",
    "shield": "护盾",
    "kinetic": "动能",

    # -- the expansion ending subtitles (space.js showExpansionEnding, 3d) --
    "the beacon pulses gently as the ship glides through space.<br>coordinates are locked. nothing to do but wait.":
        "信标缓缓脉动，飞船滑过太空。<br>坐标已经锁定。除了等待无事可做",
    "the beacon glows a solid blue, and then goes dim. the ship slows.<br>gradually, the vast wanderer homefleet comes into view.<br>massive worldships drift unnaturally through clouds of debris, scarred and dead.":
        "信标转为沉稳的蓝光，随后暗下。飞船减速。<br>流浪者的庞大母舰队渐渐进入视野。<br>巨大的世界舰在碎片云中不自然地漂着，伤痕累累，死寂无声",
    "the air is running out.": "空气就快用光了",
    "the capsule is cold.": "舱室很冷",
}


def apply_locals(pairs: dict[str, str]) -> int:
    """Append local-only keys; hard-error if any now exists upstream (drifted)."""
    for en in LOCAL_STRINGS:
        if en in pairs:
            raise SystemExit(
                f"local en_key now EXISTS upstream (adopt the official one, drop "
                f"the local): {en!r}")
    pairs.update(LOCAL_STRINGS)
    return len(LOCAL_STRINGS)


def apply_overrides(pairs: dict[str, str]) -> int:
    """Replace glyph-closure-adapted zh values in place; return count applied."""
    for en in STRING_OVERRIDES:
        if en not in pairs:
            raise SystemExit(
                f"override en_key absent from upstream table (drifted?): {en!r}")
    for en, zh in STRING_OVERRIDES.items():
        pairs[en] = zh
    return len(STRING_OVERRIDES)


def c_escape(s: str) -> str:
    """Escape a UTF-8 string as a C string body (keeps multibyte bytes raw)."""
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 0x20:
            out.append(f"\\x{ord(ch):02x}")
        else:
            out.append(ch)          # printable ASCII or UTF-8 CJK — emit raw
    return "".join(out)


HEADER_NOTE = """\
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room official Simplified-Chinese string table (Doublespeak Games,
// MPL-2.0). GENERATED by tools/gen_adr_strings.py from lang/zh_cn/strings.js
// — do not edit by hand; rerun the tool. Entries are sorted by en_key for the
// binary search in tr(). zh values are UTF-8, rendered by the CJK font
// (src/cjk_font12.h). Include this header from exactly ONE .cpp
// (cjk_text.cpp) — it carries the whole table.
"""


def emit(pairs: dict[str, str]) -> str:
    items = sorted(pairs.items(), key=lambda kv: kv[0])
    total_zh_chars = sum(len(v) for v in pairs.values())
    utf8_bytes = sum(len(v.encode("utf-8")) for v in pairs.values())

    b = []
    b.append(HEADER_NOTE.rstrip("\n"))
    b.append(f"// {len(items)} entries · {total_zh_chars} zh chars · "
             f"{utf8_bytes} UTF-8 bytes of translation text.")
    b.append("#pragma once")
    b.append("#include <stddef.h>")
    b.append("")
    b.append("struct AdrString { const char* en_key; const char* zh; };")
    b.append("")
    b.append("// Sorted by en_key (ascending, byte order) for binary search.")
    b.append(f"static const AdrString ADR_STRINGS[] = {{")
    for en, zh in items:
        b.append(f'    {{ "{c_escape(en)}", "{c_escape(zh)}" }},')
    b.append("};")
    b.append(f"static const size_t ADR_STRINGS_COUNT = "
             f"sizeof(ADR_STRINGS) / sizeof(ADR_STRINGS[0]);")
    b.append("")
    b.append("// Binary-search the table; returns the zh translation, or the")
    b.append("// en_key itself as a fallback when the key is absent. Defined in")
    b.append("// cjk_text.cpp (the sole includer of this header).")
    b.append("const char* tr(const char* en_key);")
    b.append("")
    return "\n".join(b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    pairs = parse_strings_js(args.inp)
    n_over = apply_overrides(pairs)      # §8.3 glyph-closure adaptations
    n_local = apply_locals(pairs)        # local-only keys upstream never had
    header = emit(pairs)
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    total_zh = sum(len(v) for v in pairs.values())
    print(f"wrote {args.out}: {len(pairs)} entries, {total_zh} zh chars "
          f"({sum(len(v.encode('utf-8')) for v in pairs.values())} UTF-8 bytes); "
          f"{n_over} glyph-closure override(s), {n_local} local append(s) applied")


if __name__ == "__main__":
    main()
