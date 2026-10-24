#!/usr/bin/env python3
# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

"""
The Game Boy Color's build of the portable core (core/rules.c): its code is
bigger than a ROM bank (16 KB), so this splits it into three files for
three banks (core_p0.c .. core_p2.c, N_PARTS), from the preprocessed source
(sdcc -E), without changing what it does:

  - every function becomes __banked (callable from any bank) and loses
    "static"; the file's static names get the prefix rc_ (no clashes);
  - the variables are defined in the first file and declared extern in the
    others; static constant tables are copied into each (each bank reads its
    own);
  - the functions are shared out in order, about a third of the source
    text each.

Constant tables the rest of the game reads go to core_const.c (bank 0).
It also writes core_api.h: core/rules.h with the core's functions __banked
(the game's other code includes it instead of rules.h).

  split_core.py rules_pp.c core/rules.h OUT_DIR

The parsing is deliberately simple (top-level items split at braces and
semicolons, names found with regular expressions); bankify.py and
view_ram.py import its helpers. Environment: CORE_TRACE=1 makes every
function record its index and stack address in cartridge RAM (debugging).
Licence: GPL-2.0-or-later.
"""
import re
import sys
from pathlib import Path


def items_of(src):
    """the top-level items: (text, kind) with kind 'func' or 'decl'.
    Strings and character constants are skipped over; preprocessor lines
    (# at a line's start: sdcc -E's line markers) are dropped; a braced
    item whose header ends with ')' is a function, anything ending in ';'
    at depth 0 a declaration."""
    out, i, n, start, depth = [], 0, len(src), 0, 0
    first_brace_hdr = None
    while i < n:
        c = src[i]
        if c in "\"'":
            q = c
            i += 1
            while i < n and src[i] != q:
                i += 2 if src[i] == "\\" else 1
            i += 1
            continue
        if c == "#" and (i == 0 or src[i - 1] == "\n") and depth == 0:
            j = src.find("\n", i)
            j = n if j < 0 else j
            i = j + 1
            start = i
            continue
        if c == "{":
            if depth == 0:
                first_brace_hdr = src[start:i].strip()
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and first_brace_hdr is not None and first_brace_hdr.endswith(")"):
                out.append((src[start:i + 1].strip(), "func"))
                start = i + 1
                first_brace_hdr = None
        elif c == ";" and depth == 0:
            t = src[start:i + 1].strip()
            if t:
                out.append((t, "decl"))
            start = i + 1
            first_brace_hdr = None
        i += 1
    return out


def split_top(s, sep):
    """s split at sep outside brackets"""
    parts, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    parts.append(cur)
    return parts


def func_name(hdr):
    """the name before the first '(' of a function header or prototype"""
    m = re.search(r"(\w+)\s*\(", hdr)
    return m.group(1)


def add_banked(hdr):
    """__banked after the parameter list's closing parenthesis"""
    depth, end = 0, None
    for k, ch in enumerate(hdr):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                end = k
                break
    return hdr[:end + 1] + " __banked" + hdr[end + 1:]


# the words that can only be part of a declaration's type
TYPE_WORDS = {"static", "const", "volatile", "extern", "unsigned", "signed", "char", "int", "short", "long",
              "void", "struct", "u8", "s8", "u16", "s16", "u32", "s32"}


def var_names(decl):
    """the names a declaration ('static u8 a, b[4] = {..};') declares"""
    body = decl.rstrip(";")
    segs = split_top(body, ",")
    names = []
    for k, seg in enumerate(segs):
        seg = split_top(seg, "=")[0]
        seg = re.sub(r"\[[^\]]*\]", "", seg)
        ids = re.findall(r"[A-Za-z_]\w*", seg)
        ids = [x for x in ids if x not in TYPE_WORDS] if k == 0 else ids
        if ids:
            names.append(ids[-1])
    return names


def main():
    """Write OUT_DIR/core_p0..2.c, core_const.c, core_api.h."""
    pp = Path(sys.argv[1]).read_text()
    rules_h = Path(sys.argv[2]).read_text()
    out = Path(sys.argv[3])
    items = items_of(pp)
    funcs = [t for t, k in items if k == "func"]
    defined = {func_name(t[:t.index("{")]) for t in funcs}
    # the static names: functions and variables
    statics = set()
    for t, k in items:
        if not t.startswith("static"):
            continue
        if k == "func":
            statics.add(func_name(t[:t.index("{")]))
        elif "(" in split_top(t, "=")[0] and not t.startswith("static const"):
            statics.add(func_name(t))                   # a prototype
        else:
            statics.update(var_names(t))

    def rename(text):
        """every static name (not after '.', '->' or a word character) prefixed rc_"""
        for nm in statics:
            text = re.sub(r"(?<![\.\w>])" + nm + r"\b", "rc_" + nm, text)
        return text

    # common: in every file (types, prototypes, externs); defs_a: part 0's
    # definitions; defs_b: the other parts' (externs, static tables copied);
    # defs_c: core_const.c's; protos: every function's banked prototype
    common, defs_a, defs_b, defs_c, protos = [], [], [], [], []
    for t, k in items:
        if k == "func":
            continue
        head = split_top(t, "=")[0]
        if t.startswith("typedef") or re.match(r"(struct|enum|union)\s+\w*\s*\{", t):
            common.append(t)
        elif "(" in head and "=" not in t.split("(")[0] and re.search(r"\w+\s*\([^)]*\)\s*;$", t):
            name = func_name(t)
            if name in defined:
                continue                                # (written again, banked)
            common.append(t)                            # the platform's hooks
        elif t.startswith("extern"):
            common.append(t)
        elif "const" in head.split("[")[0].split("*")[0] and "=" in t:
            if t.startswith("static"):
                defs_a.append(t)                        # constant tables: in both (each bank its own)
                defs_b.append(t)
            else:                                       # the core's own, others read: in bank 0
                defs_c.append(t)
                decl = split_top(t.rstrip(";"), "=")[0].strip()
                defs_a.append("extern " + decl + ";")
                defs_b.append("extern " + decl + ";")
        else:                                           # a variable: defined in part 0, extern elsewhere
            v = re.sub(r"^static\s+", "", t)
            defs_a.append(v)
            decl = ", ".join(split_top(x, "=")[0].strip() for x in split_top(v.rstrip(";"), ",")) + ";"
            defs_b.append("extern " + decl)
    for t in funcs:
        hdr = t[:t.index("{")].strip()
        protos.append(add_banked(re.sub(r"^static\s+", "", hdr)) + ";")
    # share out the functions (by source size: N_PARTS files)
    N_PARTS = 3
    total = sum(len(t) for t in funcs)
    parts, acc = [[] for _ in range(N_PARTS)], 0
    import os
    trace = os.environ.get("CORE_TRACE")
    for fi, t in enumerate(funcs):
        hdr = t[:t.index("{")].strip()
        body = t[t.index("{"):]
        if trace:                                   # (debugging: the last function entered, at $C092)
            body = "{" + f" {{ volatile unsigned char k = *(volatile unsigned char *)0xBFFF; *(volatile unsigned char *)(0xBE00 + k) = {fi}; *(volatile unsigned char *)(0xBD00 + k) = (unsigned char)(unsigned)&k; *(volatile unsigned char *)(0xBC00 + k) = (unsigned char)((unsigned)&k >> 8); *(volatile unsigned char *)0xBFFF = k + 1; }}" + body[1:]
        f = add_banked(re.sub(r"^static\s+", "", hdr)) + "\n" + body
        parts[min(N_PARTS - 1, int(acc * N_PARTS / total))].append(f)
        acc += len(t)
    pre = "/* generated by gbc/tools/split_core.py from core/rules.c: do not edit */\n"
    for k, fs in enumerate(parts):
        name, defs = f"core_p{k}.c", (defs_a if k == 0 else defs_b)
        text = pre + "\n".join(common) + "\n\n" + rename("\n".join(defs) + "\n\n" + "\n".join(protos) + "\n\n" +
                                                           "\n\n".join(fs)) + "\n"
        (out / name).write_text(text)
    part_a, part_b = parts[0], parts[1:]                # (not used)
    (out / "core_const.c").write_text(pre + "\n".join(common) + "\n\n" + "\n".join(defs_c) + "\n")
    # the API header
    api = []
    for line in rules_h.splitlines():
        line = re.sub(r'#include "([^"]+)"', r'#include "../../core/\1"', line)
        m = re.match(r"^([a-zA-Z].*?\b(\w+)\s*\(.*\))\s*;(.*)$", line)
        if m and m.group(2) in defined:
            line = m.group(1) + " __banked;" + m.group(3)
        api.append(line)
    (out / "core_api.h").write_text(pre + "\n".join(api) + "\n")
    print(f"{' + '.join(str(len(p)) for p in parts)} functions, {len(statics)} static names renamed")


if __name__ == "__main__":
    main()
