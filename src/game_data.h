// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room (Doublespeak Games, MPL-2.0) — Phase 1 numeric data tables.
// Every constant here is transcribed from the upstream script/ sources
// (room.js, outside.js, state_manager.js) — the values ARE the port, so this
// file is a derivative of the MPL game and carries the MPL header. Pure data,
// no Arduino/M5 dependency, so game_state's logic can be host-compiled for the
// smoke test. All en_key strings double as tr() keys (strings_zh.h) AND as the
// game's own store/craftable identifiers, exactly as upstream keys them.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

namespace adr {

// ---- Fixed point ---------------------------------------------------------
// Stores are integers × FP so the 0.5/tick hunter income never drifts the way
// a float would across thousands of offline steps (research.md §5.2).
constexpr int32_t FP = 100;

// ---- Time constants (seconds) — room.js / outside.js ---------------------
constexpr int INCOME_TICK_S   = 10;      // _INCOME delay (every worker source)
constexpr int FIRE_COOL_S     = 300;     // _FIRE_COOL_DELAY 5min (awake only)
constexpr int ROOM_WARM_S     = 30;      // _ROOM_WARM_DELAY
constexpr int BUILDER_STATE_S = 30;      // _BUILDER_STATE_DELAY
constexpr int NEED_WOOD_S     = 15;      // _NEED_WOOD_DELAY (stranger -> forest)
constexpr int STOKE_COOLDOWN_S = 10;     // _STOKE_COOLDOWN (light + stoke)
constexpr int GATHER_DELAY_S  = 60;      // _GATHER_DELAY
constexpr int TRAPS_DELAY_S   = 90;      // _TRAPS_DELAY
// _POP_DELAY [0.5, 3] min. floor(rand*(3-0.5))+0.5 -> {0.5,1.5,2.5} min; the
// deterministic offline estimate uses the mean, 1.5 min = 90s (research §5.2).
constexpr int POP_DELAY_MIN_S = 90;

constexpr int SETTLE_MAX_S = 24 * 3600;  // single-settle cap (town_page parity)

// Button wood costs (room.js).
constexpr int LIGHT_FIRE_WOOD = 5;
constexpr int STOKE_FIRE_WOOD = 1;

constexpr int HUT_ROOM = 4;              // _HUT_ROOM: villagers per hut

// ---- Fire / Temperature state machines (room.js FireEnum/TempEnum) --------
enum Fire : uint8_t { FIRE_DEAD = 0, FIRE_SMOLDERING, FIRE_FLICKERING,
                      FIRE_BURNING, FIRE_ROARING };
enum Temp : uint8_t { TEMP_FREEZING = 0, TEMP_COLD, TEMP_MILD,
                      TEMP_WARM, TEMP_HOT };
static const char* const FIRE_TEXT[5] = {
    "dead", "smoldering", "flickering", "burning", "roaring" };
static const char* const TEMP_TEXT[5] = {
    "freezing", "cold", "mild", "warm", "hot" };

// ---- Resources (stores) — the P1-reachable economic set ------------------
// wood..charm from gather/traps/income; iron..compass reachable via trading
// post (buy) + steelworks/armoury income. Crafted upgrades/weapons/buildings
// are counted separately (Item / Bld), not here.
enum Res : uint8_t {
    R_WOOD = 0, R_FUR, R_MEAT, R_BAIT, R_LEATHER, R_CURED_MEAT,
    R_IRON, R_COAL, R_SULPHUR, R_STEEL, R_TEETH, R_SCALES, R_CLOTH, R_CHARM,
    R_BULLETS, R_MEDICINE, R_ENERGY_CELL, R_ALIEN_ALLOY, R_COMPASS,
    RES_COUNT
};
static const char* const RES_KEY[RES_COUNT] = {
    "wood", "fur", "meat", "bait", "leather", "cured meat",
    "iron", "coal", "sulphur", "steel", "teeth", "scales", "cloth", "charm",
    "bullets", "medicine", "energy cell", "alien alloy", "compass" };

// ---- Buildings (game.buildings) — P1 craftables + P2 World mines ----------
enum Bld : uint8_t {
    B_TRAP = 0, B_CART, B_HUT, B_LODGE, B_TRADING_POST, B_TANNERY,
    B_SMOKEHOUSE, B_WORKSHOP, B_STEELWORKS, B_ARMOURY,
    // -- Phase 2 World mines. NOT craftable: World.goHome() sets each to 1 the
    // first time the matching mine setpiece is cleared (world.js goHome), which
    // unlocks the corresponding miner job (JOB_REQ_BLD below). They sit AFTER
    // the 10 craftable buildings so craft ids 0..9 still map 1:1 to Bld 0..9.
    B_IRON_MINE, B_COAL_MINE, B_SULPHUR_MINE,
    BLD_COUNT
};
constexpr uint8_t BLD_NONE = 0xFF;
// Count of CRAFTABLE buildings (Craft ids 0..9 == Bld 0..9). Distinct from
// BLD_COUNT now that the non-craftable World mines extend the Bld enum — the
// craft<->building alignment (craftIsBuilding / craftSlot) keys off THIS, not
// BLD_COUNT, so appending mines can't misclassify a tool craft id as a building.
constexpr uint8_t CRAFT_BLD_COUNT = B_IRON_MINE;   // = 10
static const char* const BLD_KEY[BLD_COUNT] = {
    "trap", "cart", "hut", "lodge", "trading post", "tannery",
    "smokehouse", "workshop", "steelworks", "armoury",
    "iron mine", "coal mine", "sulphur mine" };

// ---- Craftable items (stores, non-building) -------------------------------
enum Item : uint8_t {
    I_TORCH = 0, I_WATERSKIN, I_CASK, I_WATER_TANK, I_BONE_SPEAR, I_RUCKSACK,
    I_WAGON, I_CONVOY, I_L_ARMOUR, I_I_ARMOUR, I_S_ARMOUR, I_IRON_SWORD,
    I_STEEL_SWORD, I_RIFLE,
    // -- Phase 2 World carryable weapons (path.js carryable / World.Weapons).
    // Loot from battlefield / city / mines, NOT P1-craftable, so they carry no
    // Craft id — they extend Item past the 14 craftables purely as store slots a
    // World expedition can carry and bank on goHome. Their Path.Weight (>1, see
    // world_data.h WEIGHTS) is why they get dedicated slots rather than a Res row.
    I_BAYONET, I_LASER_RIFLE, I_GRENADE, I_BOLAS,
    ITEM_COUNT
};
static const char* const ITEM_KEY[ITEM_COUNT] = {
    "torch", "waterskin", "cask", "water tank", "bone spear", "rucksack",
    "wagon", "convoy", "l armour", "i armour", "s armour", "iron sword",
    "steel sword", "rifle",
    "bayonet", "laser rifle", "grenade", "bolas" };

// ---- Unified craftable table (room.js Room.Craftables) --------------------
// Craft ids 0..9 are buildings (aligned 1:1 with Bld); 10..23 are items
// (aligned 1:1 with Item, id-10). This alignment lets build()/craft() map a
// craft id straight to its count slot without a lookup table.
enum Craft : uint8_t {
    C_TRAP = 0, C_CART, C_HUT, C_LODGE, C_TRADING_POST, C_TANNERY,
    C_SMOKEHOUSE, C_WORKSHOP, C_STEELWORKS, C_ARMOURY,
    C_TORCH, C_WATERSKIN, C_CASK, C_WATER_TANK, C_BONE_SPEAR, C_RUCKSACK,
    C_WAGON, C_CONVOY, C_L_ARMOUR, C_I_ARMOUR, C_S_ARMOUR, C_IRON_SWORD,
    C_STEEL_SWORD, C_RIFLE,
    CRAFT_COUNT
};
enum CraftType : uint8_t { CT_BUILDING = 0, CT_TOOL, CT_UPGRADE, CT_WEAPON };

struct ResAmt { uint8_t res; int32_t amt; };  // amt in WHOLE units
constexpr uint8_t RA_END = 0xFF;              // cost-list terminator

struct Craftable {
    const char*  key;            // en_key (tr + identity)
    const char*  buildMsg;       // en_key logged on successful build
    const char*  availableMsg;   // en_key pushed ONCE when first offerable (room.js
                                 // craftUnlocked); nullptr = none (upstream: only
                                 // buildings carry availableMsg, not tools/weapons)
    uint8_t      type;           // CraftType
    int16_t      maximum;        // -1 = unlimited
    int32_t      woodIncrPerN;   // extra wood per existing count (trap 10/hut 50)
    ResAmt       cost[3];        // base cost, RA_END-terminated
};

// Buildings first (ids 0..9), then tools/upgrades/weapons (ids 10..23).
static const Craftable CRAFT[CRAFT_COUNT] = {
    // -- buildings (each carries a room.js availableMsg, pushed once on unlock) --
    { "trap", "more traps to catch more creatures",
      "builder says she can make traps to catch any creatures might still be alive out there",
      CT_BUILDING, 10, 10, { {R_WOOD,10}, {RA_END,0}, {RA_END,0} } },
    { "cart", "the rickety cart will carry more wood from the forest",
      "builder says she can make a cart for carrying wood",
      CT_BUILDING, 1, 0, { {R_WOOD,30}, {RA_END,0}, {RA_END,0} } },
    { "hut", "builder puts up a hut, out in the forest. says word will get around.",
      "builder says there are more wanderers. says they'll work, too.",
      CT_BUILDING, 20, 50, { {R_WOOD,100}, {RA_END,0}, {RA_END,0} } },
    { "lodge", "the hunting lodge stands in the forest, a ways out of town",
      "villagers could help hunt, given the means",
      CT_BUILDING, 1, 0, { {R_WOOD,200}, {R_FUR,10}, {R_MEAT,5} } },
    { "trading post",
      "now the nomads have a place to set up shop, they might stick around a while",
      "a trading post would make commerce easier",
      CT_BUILDING, 1, 0, { {R_WOOD,400}, {R_FUR,100}, {RA_END,0} } },
    { "tannery", "tannery goes up quick, on the edge of the village",
      "builder says leather could be useful. says the villagers could make it.",
      CT_BUILDING, 1, 0, { {R_WOOD,500}, {R_FUR,50}, {RA_END,0} } },
    { "smokehouse", "builder finishes the smokehouse. she looks hungry.",
      "should cure the meat, or it'll spoil. builder says she can fix something up.",
      CT_BUILDING, 1, 0, { {R_WOOD,600}, {R_MEAT,50}, {RA_END,0} } },
    { "workshop", "workshop's finally ready. builder's excited to get to it",
      "builder says she could make finer things, if she had the tools",
      CT_BUILDING, 1, 0, { {R_WOOD,800}, {R_LEATHER,100}, {R_SCALES,10} } },
    { "steelworks", "a haze falls over the village as the steelworks fires up",
      "builder says the villagers could make steel, given the tools",
      CT_BUILDING, 1, 0, { {R_WOOD,1500}, {R_IRON,100}, {R_COAL,100} } },
    { "armoury", "armoury's done, welcoming back the weapons of the past.",
      "builder says it'd be useful to have a steady source of bullets",
      CT_BUILDING, 1, 0, { {R_WOOD,3000}, {R_STEEL,100}, {R_SULPHUR,50} } },
    // -- tools / upgrades / weapons (need workshop; no upstream availableMsg) --
    { "torch", "a torch to keep the dark away", nullptr, CT_TOOL, -1, 0,
      { {R_WOOD,1}, {R_CLOTH,1}, {RA_END,0} } },
    { "waterskin", "this waterskin'll hold a bit of water, at least", nullptr,
      CT_UPGRADE, 1, 0, { {R_LEATHER,50}, {RA_END,0}, {RA_END,0} } },
    { "cask", "the cask holds enough water for longer expeditions", nullptr,
      CT_UPGRADE, 1, 0, { {R_LEATHER,100}, {R_IRON,20}, {RA_END,0} } },
    { "water tank", "never go thirsty again", nullptr, CT_UPGRADE, 1, 0,
      { {R_IRON,100}, {R_STEEL,50}, {RA_END,0} } },
    { "bone spear", "this spear's not elegant, but it's pretty good at stabbing",
      nullptr, CT_WEAPON, -1, 0, { {R_WOOD,100}, {R_TEETH,5}, {RA_END,0} } },
    { "rucksack", "carrying more means longer expeditions to the wilds", nullptr,
      CT_UPGRADE, 1, 0, { {R_LEATHER,200}, {RA_END,0}, {RA_END,0} } },
    { "wagon", "the wagon can carry a lot of supplies", nullptr, CT_UPGRADE, 1, 0,
      { {R_WOOD,500}, {R_IRON,100}, {RA_END,0} } },
    { "convoy", "the convoy can haul mostly everything", nullptr, CT_UPGRADE, 1, 0,
      { {R_WOOD,1000}, {R_IRON,200}, {R_STEEL,100} } },
    { "l armour", "leather's not strong. better than rags, though.",
      nullptr, CT_UPGRADE, 1, 0, { {R_LEATHER,200}, {R_SCALES,20}, {RA_END,0} } },
    { "i armour", "iron's stronger than leather", nullptr, CT_UPGRADE, 1, 0,
      { {R_LEATHER,200}, {R_IRON,100}, {RA_END,0} } },
    { "s armour", "steel's stronger than iron", nullptr, CT_UPGRADE, 1, 0,
      { {R_LEATHER,200}, {R_STEEL,100}, {RA_END,0} } },
    { "iron sword", "sword is sharp. good protection out in the wilds.",
      nullptr, CT_WEAPON, -1, 0, { {R_WOOD,200}, {R_LEATHER,50}, {R_IRON,20} } },
    { "steel sword", "the steel is strong, and the blade true.",
      nullptr, CT_WEAPON, -1, 0, { {R_WOOD,500}, {R_LEATHER,100}, {R_STEEL,20} } },
    { "rifle", "black powder and bullets, like the old days.",
      nullptr, CT_WEAPON, -1, 0, { {R_WOOD,200}, {R_STEEL,50}, {R_SULPHUR,50} } },
};

// Map a building/item count slot from a craft id.
inline bool craftIsBuilding(uint8_t c) { return c < CRAFT_BLD_COUNT; }
inline uint8_t craftSlot(uint8_t c) {
    return craftIsBuilding(c) ? c : (uint8_t)(c - CRAFT_BLD_COUNT);
}
inline bool craftNeedsWorkshop(uint8_t type) { return type != CT_BUILDING; }

// ---- Trade goods (room.js Room.TradeGoods) — unlocked by trading post -----
// P1 subset: the resource-producing goods + compass (all costs & products land
// in the Res enum). P2 combat weapons (bolas/grenade/bayonet) are omitted here.
enum Trade : uint8_t {
    T_SCALES = 0, T_TEETH, T_IRON, T_COAL, T_STEEL, T_MEDICINE, T_BULLETS,
    T_ENERGY_CELL, T_ALIEN_ALLOY, T_COMPASS,
    TRADE_COUNT
};
struct TradeGood {
    uint8_t product;    // Res added (+1) on buy
    int16_t maximum;    // -1 = unlimited
    ResAmt  cost[3];    // RA_END-terminated
};
static const TradeGood TRADE[TRADE_COUNT] = {
    { R_SCALES,       -1, { {R_FUR,150}, {RA_END,0}, {RA_END,0} } },
    { R_TEETH,        -1, { {R_FUR,300}, {RA_END,0}, {RA_END,0} } },
    { R_IRON,         -1, { {R_FUR,150}, {R_SCALES,50}, {RA_END,0} } },
    { R_COAL,         -1, { {R_FUR,200}, {R_TEETH,50}, {RA_END,0} } },
    { R_STEEL,        -1, { {R_FUR,300}, {R_SCALES,50}, {R_TEETH,50} } },
    { R_MEDICINE,     -1, { {R_SCALES,50}, {R_TEETH,30}, {RA_END,0} } },
    { R_BULLETS,      -1, { {R_SCALES,10}, {RA_END,0}, {RA_END,0} } },
    { R_ENERGY_CELL,  -1, { {R_SCALES,10}, {R_TEETH,10}, {RA_END,0} } },
    { R_ALIEN_ALLOY,  -1, { {R_FUR,1500}, {R_SCALES,750}, {R_TEETH,300} } },
    { R_COMPASS,       1, { {R_FUR,400}, {R_SCALES,20}, {R_TEETH,10} } },
};

// ---- Worker income (outside.js _INCOME) -----------------------------------
// 10 jobs. Values are PER WORKER PER 10s tick, in fixed point (×FP). gatherer's
// count is derived (population − assigned); the rest are the assignable jobs in
// game.workers. Per source, only as many workers as the inputs on hand can feed
// are settled — the rest idle (see game_state applyIncomeSource).
enum Job : uint8_t {
    J_GATHERER = 0, J_HUNTER, J_TRAPPER, J_TANNER, J_CHARCUTIER,
    J_IRON_MINER, J_COAL_MINER, J_SULPHUR_MINER, J_STEELWORKER, J_ARMOURER,
    JOB_COUNT
};
static const char* const JOB_KEY[JOB_COUNT] = {
    "gatherer", "hunter", "trapper", "tanner", "charcutier",
    "iron miner", "coal miner", "sulphur miner", "steelworker", "armourer" };

struct IncomeItem { uint8_t res; int32_t dfp; };  // per worker, fixed point
struct IncomeDef  { IncomeItem items[3]; uint8_t n; };
static const IncomeDef INCOME[JOB_COUNT] = {
    /* gatherer     */ { { {R_WOOD, +1*FP}, {}, {} }, 1 },
    /* hunter       */ { { {R_FUR, +FP/2}, {R_MEAT, +FP/2}, {} }, 2 },
    /* trapper      */ { { {R_MEAT, -1*FP}, {R_BAIT, +1*FP}, {} }, 2 },
    /* tanner       */ { { {R_FUR, -5*FP}, {R_LEATHER, +1*FP}, {} }, 2 },
    /* charcutier   */ { { {R_MEAT, -5*FP}, {R_WOOD, -5*FP}, {R_CURED_MEAT, +1*FP} }, 3 },
    /* iron miner   */ { { {R_CURED_MEAT, -1*FP}, {R_IRON, +1*FP}, {} }, 2 },
    /* coal miner   */ { { {R_CURED_MEAT, -1*FP}, {R_COAL, +1*FP}, {} }, 2 },
    /* sulphur miner*/ { { {R_CURED_MEAT, -1*FP}, {R_SULPHUR, +1*FP}, {} }, 2 },
    /* steelworker  */ { { {R_IRON, -1*FP}, {R_COAL, -1*FP}, {R_STEEL, +1*FP} }, 3 },
    /* armourer     */ { { {R_STEEL, -1*FP}, {R_SULPHUR, -1*FP}, {R_BULLETS, +1*FP} }, 3 },
};

// Building each job requires before it can be staffed (outside.js checkWorker /
// jobMap). Miners require World mines (P2) — no P1 building, so BLD_NONE.
static const uint8_t JOB_REQ_BLD[JOB_COUNT] = {
    BLD_NONE,        // gatherer (default)
    B_LODGE,         // hunter
    B_LODGE,         // trapper
    B_TANNERY,       // tanner
    B_SMOKEHOUSE,    // charcutier
    B_IRON_MINE,     // iron miner    (World mine, P2 — set by World.goHome)
    B_COAL_MINE,     // coal miner    (World mine, P2)
    B_SULPHUR_MINE,  // sulphur miner (World mine, P2)
    B_STEELWORKS,    // steelworker
    B_ARMOURY,       // armourer
};

// Builder wood income once Helping (level 4): +2 wood / 10s (room.js onArrival).
constexpr int32_t BUILDER_WOOD_DFP = 2 * FP;

// ---- Idle-crew notice: packed log arg ------------------------------------
// When a shortage truncates a crew (applyIncomeSource above), the log line has
// to name three things — WHICH job, WHICH input ran short, HOW MANY idled — but
// a LogEntry carries a single int32 arg. Pack all three into it and let the
// renderer (room_page logText) splice tr(JOB_KEY[job]) / tr(RES_KEY[res]) /
// the count into the template. Same "the arg is an index, not a number" trick
// "the fire is {0}" already uses for its Fire enum, just three fields wide.
// The recovery line only needs the job, so it packs res/idle as 0.
inline int32_t packIdleArg(uint8_t job, uint8_t res, int idle) {
    if (idle < 0) idle = 0;
    if (idle > 0xFFFF) idle = 0xFFFF;
    return (int32_t)job | ((int32_t)res << 8) | ((int32_t)idle << 16);
}
inline uint8_t idleArgJob(int32_t a)  { return (uint8_t)(a & 0xFF); }
inline uint8_t idleArgRes(int32_t a)  { return (uint8_t)((a >> 8) & 0xFF); }
inline int     idleArgIdle(int32_t a) { return (int)((a >> 16) & 0xFFFF); }

// ---- Compact quantity formatter (v0.3.3) ----------------------------------
// Real play stockpiles wood/fur into the thousands, and the Outside inventory
// box's 3-column grid + the Trade balance row have no room for a 5-6 digit run.
// Abbreviate above 1000, ASCII only (shares the existing mixed CJK/ASCII draw
// path): v<1000 verbatim; 1000..9999 -> "1.2K" (one decimal, TRUNCATED, never
// rounded up past a digit — 1999 stays "1.9K"); 10000..999999 -> "12K".."999K"
// (integer K); 1e6..9.99e6 -> "1.2M" (one decimal, truncated); >=1e7 -> "12M"
// (integer M). Output is at most 5 chars + NUL. Narrative log text and the
// population / building counts stay full-number (small values, never abbreviate).
inline void fmtAmount(int32_t v, char* out, size_t cap) {
    if (v < 0) v = 0;
    if (v < 1000)          { snprintf(out, cap, "%ld", (long)v); return; }
    if (v < 10000)         { snprintf(out, cap, "%ld.%ldK",
                                      (long)(v / 1000), (long)((v % 1000) / 100)); return; }
    if (v < 1000000)       { snprintf(out, cap, "%ldK", (long)(v / 1000)); return; }
    if (v < 10000000)      { snprintf(out, cap, "%ld.%ldM",
                                      (long)(v / 1000000), (long)((v % 1000000) / 100000)); return; }
    snprintf(out, cap, "%ldM", (long)(v / 1000000));
}

// ---- Trap drops (outside.js TrapDrops) ------------------------------------
struct TrapDrop { int32_t rollUnderMilli; uint8_t res; const char* msg; };
static const TrapDrop TRAP_DROPS[6] = {
    { 500,  R_FUR,    "scraps of fur" },
    { 750,  R_MEAT,   "bits of meat" },
    { 850,  R_SCALES, "strange scales" },
    { 930,  R_TEETH,  "scattered teeth" },
    { 995,  R_CLOTH,  "tattered cloth" },
    { 1000, R_CHARM,  "a crudely made charm" },
};

}  // namespace adr
