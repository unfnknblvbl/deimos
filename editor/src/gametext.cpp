// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// The game's texts from the jar (see gametext.h): a port of
// tools/game_text.py. A text's "recipe" is a list of parts joined, then
// upper-cased: a part is a string of the jar's base.str, entities.str or
// items.str (or its piece between the "%1"-style placeholders the phone
// game fills in), changed by an op, or a plain string of Deimos's own.
#include "gametext.h"
#include "common.h"
#include "jar.h"
#include "keytext.h"

namespace gametext {

namespace {

enum Table { BASE, ENTS, ITEMS };              // base.str, entities.str, items.str
enum Op { AS_IS, NO_COLON, NO_BAR, COUNT, WORD };   // tools/game_text.py's "", "colon", "bar", "count", "word"
const int WHOLE = -1;                          // a part's piece: the whole string

// one part of a recipe: lit (Deimos's own text) when not null, else
// string `index` of `table`, its piece `piece` (WHOLE: all of it), op applied
struct Part {
    const char *lit;
    int table, index, piece, op;
};
// a part from the jar, and one of Deimos's own
constexpr Part J(int table, int index, int piece = WHOLE, int op = AS_IS) { return {nullptr, table, index, piece, op}; }
constexpr Part L(const char *s) { return {s, 0, 0, WHOLE, AS_IS}; }

// one text: up to three parts (n of them)
struct Recipe {
    int n;
    Part p[3];
};
// recipes of 0, 1, 2 and 3 parts
constexpr Recipe R() { return {0, {}}; }
constexpr Recipe R(Part a) { return {1, {a}}; }
constexpr Recipe R(Part a, Part b) { return {2, {a, b}}; }
constexpr Recipe R(Part a, Part b, Part c) { return {3, {a, b, c}}; }

// the texts table, in tools/game_text.py's TEXTS order (the names there:
// TX_GAINED is the first, and so on)
const Recipe TEXTS[] = {
    // ---- the rules' messages
    R(J(BASE, 69, 0)), R(J(BASE, 69, 1)), R(J(BASE, 68, 1)), R(J(BASE, 84, 1)),        // GAINED .. XP_GAINED
    R(J(BASE, 78), L(" "), J(BASE, 76, 0, NO_COLON)),                                    // LEVEL_UP
    R(J(BASE, 0, 1)), R(J(BASE, 31)), R(J(BASE, 30)), R(J(BASE, 32)),                  // DAMAGE .. ARMOR_GONE
    R(J(BASE, 29, 0)), R(J(BASE, 28)), R(J(BASE, 16, 2)), R(J(BASE, 1, 1)),            // DOG_TOOK .. ATTACKS
    R(J(BASE, 5, 1)), R(J(BASE, 7, 1)), R(J(BASE, 6)), R(J(BASE, 4)),                  // CASTS_RAISE .. DODGED
    R(J(BASE, 33)), R(J(BASE, 35)), R(J(BASE, 79)), R(J(BASE, 26)),                    // FIRE_CLEARED .. PAIN
    R(J(BASE, 88)), R(J(BASE, 90)), R(J(BASE, 87)), R(J(BASE, 89)),                    // NEED_GREEN .. NEED_RED
    R(J(BASE, 82)), R(J(BASE, 80)), R(J(BASE, 81)), R(J(BASE, 83)),                    // LOW_HP .. LOW_XP
    R(J(BASE, 242)), R(J(BASE, 241)), R(J(BASE, 34)), R(J(BASE, 25)),                  // CODE_BAD .. NO_AMMO
    R(J(BASE, 23)), R(J(BASE, 24)), R(J(BASE, 37)), R(J(BASE, 39)),                    // MISSED .. HEALTH_MAX
    R(J(BASE, 40, 0)), R(J(BASE, 36)), R(J(BASE, 50)), R(J(BASE, 51)),                 // GOT .. BURNS_MORE
    R(J(BASE, 66)), R(J(BASE, 67)), R(J(BASE, 91, 0)), R(J(BASE, 91, 1)),              // NOTHING .. PORTAL_END
    R(J(BASE, 85)), R(J(BASE, 86)), R(J(BASE, 12)), R(J(BASE, 10)),                    // BERSERK_ON .. BAD_TARGET
    R(J(BASE, 13, 0)),                                                                   // WONT_FIT
    // ---- the menus and boxes
    R(J(BASE, 162, WHOLE, NO_COLON)), R(J(BASE, 162, WHOLE, NO_COLON), L(" ")),          // CREDITS, CREDITS_SP
    R(L("  "), J(BASE, 162, WHOLE, NO_COLON), L(" ")),                                   // CREDITS_MID
    R(J(BASE, 117)), R(J(BASE, 148)), R(J(BASE, 114)), R(J(BASE, 147)),                // NOTEBOOK .. NEW_GAME
    R(J(BASE, 154)), R(J(BASE, 120)), R(J(BASE, 115)), R(J(BASE, 238)),                // SAVE_GAME .. NO_CREDITS
    R(J(BASE, 217), L(" "), J(BASE, 218, WHOLE, WORD)),                                  // TOO_MANY
    R(J(ENTS, 73)), R(J(ENTS, 74)), R(J(BASE, 11, 0)), R(J(BASE, 11, 1, NO_BAR)),      // BERSERKER .. CAPTURED_END
    // ---- the vendor's items, by the rules' item tag (STORE_0 .. STORE_16)
    R(), R(), R(J(BASE, 182)), R(J(BASE, 181)), R(J(BASE, 183)), R(J(BASE, 180)), R(),
    R(J(BASE, 177, WHOLE, COUNT)), R(J(BASE, 175, WHOLE, COUNT)), R(J(BASE, 179, WHOLE, COUNT)),
    R(J(BASE, 176, WHOLE, COUNT)), R(J(BASE, 178, WHOLE, COUNT)), R(),
    R(J(BASE, 184)), R(J(BASE, 185)), R(J(BASE, 187)), R(J(BASE, 186)),
    // ---- the weapons' names (WPN_0 .. WPN_18)
    R(J(ENTS, 0)), R(J(ENTS, 1)), R(J(ENTS, 8)), R(J(ENTS, 2)), R(J(ENTS, 4)), R(L("SUPER SHOTGUN")),
    R(J(ENTS, 6)), R(J(ENTS, 5)), R(J(ENTS, 7)), R(L("HELL HOUND")), R(J(ENTS, 10)), R(J(ENTS, 11)),
    R(L("MELEE 1")), R(L("MELEE 2")), R(L("MELEE 3")), R(L("MISSILE")), R(L("BOSS MISSILE")),
    R(L("ROCKET MISSILE")), R(L("RAISE")),
};

// a .str table (u16 count, then per string a u16 length and its bytes,
// little-endian) as Latin-1 strings; what is cut short is left out
std::vector<std::string> table(const Jar &jar, int t)
{
    static const char *const FILES[] = {"base.str", "entities.str", "items.str"};
    std::vector<std::string> out;
    const Bytes *b = jar.get(FILES[t]);
    if (!b || b->size() < 2) return out;
    const Bytes &d = *b;
    unsigned n = d[0] | (d[1] << 8);
    size_t p = 2;
    for (unsigned i = 0; i < n && p + 2 <= d.size(); i++) {
        unsigned len = d[p] | (d[p + 1] << 8);
        p += 2;
        if (p + len > d.size()) break;
        out.emplace_back((const char *)&d[p], len);
        p += len;
    }
    return out;
}

// one part's text (tools/game_text.py part)
std::string part(const std::vector<std::string> tables[3], const Part &pt)
{
    if (pt.lit) return pt.lit;
    const auto &rows = tables[pt.table];
    std::string s = pt.index < (int)rows.size() ? rows[pt.index] : "";
    if (pt.piece != WHOLE) {   // the piece between placeholders ("%" and a digit)
        std::vector<std::string> pieces(1);
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '%' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
                pieces.emplace_back();
                i++;
            } else
                pieces.back() += s[i];
        }
        s = pt.piece < (int)pieces.size() ? pieces[pt.piece] : "";
    }
    if (pt.op == NO_COLON) {
        std::string t;
        for (char c : s)
            if (c != ':') t += c;
        s = t;
    } else if (pt.op == NO_BAR) {
        while (!s.empty() && s.back() == '|') s.pop_back();
    } else if (pt.op == COUNT) {   // "10x Shells" -> "10 Shells"
        size_t k = 0;
        while (k < s.size() && s[k] >= '0' && s[k] <= '9') k++;
        if (k > 0 && k + 1 < s.size() && s[k] == 'x' && s[k + 1] == ' ') s.erase(k, 1);
    } else if (pt.op == WORD) {
        s = s.substr(0, s.find(' '));
    }
    return s;
}

// the three tables of the jar
struct Tables {
    std::vector<std::string> t[3];
    explicit Tables(const Jar &jar)
    {
        for (int i = 0; i < 3; i++) t[i] = table(jar, i);
    }
};

}  // namespace

// see gametext.h: each recipe's parts joined, a..z made capitals
std::vector<std::string> texts(const Jar &jar)
{
    Tables tb(jar);
    std::vector<std::string> out;
    for (const Recipe &r : TEXTS) {
        std::string s;
        for (int i = 0; i < r.n; i++) s += part(tb.t, r.p[i]);
        for (auto &c : s)
            if (c >= 'a' && c <= 'z') c -= 32;
        out.push_back(s);
    }
    return out;
}

// see gametext.h (tools/game_text.py md_blob)
Bytes md_blob(const Jar &jar)
{
    std::vector<std::string> ts = texts(jar);
    Bytes out(2 * ts.size(), 0);
    for (size_t i = 0; i < ts.size(); i++) {
        size_t at = out.size();
        out[2 * i] = (u8)(at >> 8);
        out[2 * i + 1] = (u8)at;
        std::string t = keytext::md_text(ts[i]);
        out.insert(out.end(), t.begin(), t.end());
        out.push_back(0);
    }
    return out;
}

// see gametext.h (tools/game_text.py sms_blob, SMS_ADDR $8000)
Bytes sms_blob(const Jar &jar, bool gg)
{
    std::vector<std::string> ts = texts(jar);
    Bytes out(2 * ts.size(), 0);
    for (size_t i = 0; i < ts.size(); i++) {
        size_t at = 0x8000 + out.size();
        out[2 * i] = (u8)at;
        out[2 * i + 1] = (u8)(at >> 8);
        std::string t = keytext::sms_text(ts[i], gg);
        out.insert(out.end(), t.begin(), t.end());
        out.push_back(0);
    }
    return out;
}

// see gametext.h (tools/game_text.py MAP_TITLE)
std::string map_title(const Jar &jar, const std::string &map)
{
    Tables tb(jar);
    Part pt = L("");
    if (map == "intro") pt = L("UAC Base");
    else if (map == "junction" || map == "junction_destroyed") pt = J(BASE, 57);
    else if (map == "reactor") pt = J(BASE, 58);
    else if (map == "endgame") pt = L("Ending");
    else if (map == "items") pt = J(ITEMS, 0);
    else if (map.size() == 7 && map.rfind("level0", 0) == 0 && map[6] >= '1' && map[6] <= '7')
        pt = J(BASE, 58 + (map[6] - '0'));
    return latin1_to_utf8(part(tb.t, pt));
}

}  // namespace gametext
