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
