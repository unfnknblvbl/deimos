// Deimos - Doom RPG on retro consoles
// Copyright (C) 2026 unfnknblvbl and contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Free software under the GNU General Public License, version 2 or (at your
// option) any later version, with NO WARRANTY: see the LICENSE file.

// Licence: GPL-2.0-or-later.
//
// The phone's keys in the texts (see keytext.h): a line-for-line copy of
// tools/key_text.py, which the Python builds use; any change there must
// be made here too (the editor's ROMs must stay byte-identical with them).
#include "keytext.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace keytext {
namespace {

// the phone's keys as the texts write them, and the token of what each does
// (the jar's key codes: * previous weapon, 7 next, # automap, 0 menu, 9 pass)
struct Key {
    const char *word;
    unsigned char token;
};
const Key KEYS[] = {{"OK", KEY_USE}, {"*", KEY_PREV}, {"7", KEY_NEXT}, {"#", KEY_NOTES}, {"0", KEY_MENU}, {"9", KEY_PASS}};
// the word after the key and what it becomes, longest first (tried in this order)
const char *const NOUNS[4][2] = {{"buttons", "buttons"}, {"button", "button"}, {"keys", "buttons"}, {"key", "button"}};

const int BOX_COLS = 21;        // a box line's columns from the text's column (3) to its edge (24)
const int WIDEST_ACTION = 9;    // the longest button name an action can have ("SEL+RIGHT", "SEL+START")
const int WIDEST_MENU = 5;      // the pause menu's ("START")
const int SMS_COLS = 21, GG_COLS = 19;   // the Master System's and Game Gear's boxes

// each token's name in the Text tab, by token (empty: not a token)
const char *markup_name(unsigned char t)
{
    switch (t) {
    case KEY_USE: return "USE";
    case KEY_PASS: return "PASS TURN";
    case KEY_MEDKIT: return "MEDKIT";
    case KEY_PREV: return "PREV WEAPON";
    case KEY_NEXT: return "NEXT WEAPON";
    case KEY_NOTES: return "NOTEBOOK";
    case KEY_DETAIL: return "FULL DETAIL";
    case KEY_MENU: return "PAUSE MENU";
    default: return "";
    }
}

// the Master System's fixed buttons (game/ui.asm), by token; gg: the Game Gear's
const char *sms_name(unsigned char t, bool gg)
{
    switch (t) {
    case KEY_USE: return "1";
    case KEY_PASS: return "2+DOWN";
    case KEY_MEDKIT: return "2+UP";
    case KEY_PREV: return "2+LEFT";
    case KEY_NEXT: return "2+RIGHT";
    case KEY_NOTES: return "2+1";
    case KEY_MENU: return gg ? "START" : "PAUSE";
    default: return "";   // (KEY_DETAIL: the Master System has none)
    }
}

// is s[i] a space or '|' (what separates the words of a phrase)?
bool sep(const std::string &s, size_t i) { return i < s.size() && (s[i] == ' ' || s[i] == '|'); }

// does s hold `w` at i?
bool starts(const std::string &s, size_t i, const char *w) { return s.compare(i, std::char_traits<char>::length(w), w) == 0; }

// the index just past the word starting at s[i] (a word ends at a space, a '|' or the end)
size_t word_end(const std::string &s, size_t i)
{
    while (i < s.size() && !sep(s, i)) i++;
    return i;
}

// if one of NOUNS starts at s[i] as a whole word (not followed by an ASCII
// letter or a '-', as in "key-|card"): its index in NOUNS, else -1
int noun_at(const std::string &s, size_t i)
{
    for (int k = 0; k < 4; k++) {
        size_t j = i + std::char_traits<char>::length(NOUNS[k][0]);
        if (!starts(s, i, NOUNS[k][0])) continue;
        if (j == s.size()) return k;
        unsigned char c = (unsigned char)s[j];
        if (!(((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) || c == '-')) return k;
    }
    return -1;
}

// key_text.py _match: at s[i] (the word "the"), a key phrase ("the K NOUN"
// or "the K and K NOUN", the words separated by one space or '|', kept as
// they were): true with out = what replaces it and end = the index past it
bool match(const std::string &s, size_t i, std::string &out, size_t &end)
{
    if (!(starts(s, i, "the") && (i == 0 || sep(s, i - 1)) && i + 4 < s.size() && sep(s, i + 3))) return false;
    out = "the";
    out += s[i + 3];
    i += 4;
    for (int n_keys = 1; n_keys <= 2; n_keys++) {   // the first key, then perhaps "and" and a second
        size_t e = word_end(s, i);
        std::string w = s.substr(i, e - i);
        int tok = 0;
        for (auto &k : KEYS)
            if (w == k.word) tok = k.token;
        if (!tok || e >= s.size()) return false;
        out += (char)tok;   // the key's token and the separator after it
        out += s[e];
        i = e + 1;
        int n = noun_at(s, i);
        if (n >= 0) {
            out += NOUNS[n][1];
            end = i + std::char_traits<char>::length(NOUNS[n][0]);
            return true;
        }
        if (n_keys == 2 || !(starts(s, i, "and") && i + 4 < s.size() && sep(s, i + 3))) return false;
        out += "and";
        out += s[i + 3];
        i += 4;
    }
    return false;
}

// does s hold a token?
bool has_token(const std::string &s)
{
    for (char c : s)
        if (is_token((unsigned char)c)) return true;
    return false;
}

// a character's columns at most in a Mega Drive or SNES-family box: a
// token's widest button name, else 1 (key_text.py md_width)
int md_width(unsigned char c) { return c == KEY_MENU ? WIDEST_MENU : is_token(c) ? WIDEST_ACTION : 1; }

// key_text.py rewrap: the lines ('|') wider than cols columns (width(c)
// each character's) broken at their last space that leaves the part
// before it within cols; the rest goes to the front of the next line
// (joined with a space, or with nothing after a '-'), or onto a new line
// before an empty line or at the end; then the next line is looked at. A
// line with no such space is left as it is (the box cuts it).
template <class W> std::string rewrap(const std::string &s, W width, int cols)
{
    std::vector<std::string> lines;
    size_t a = 0;
    for (;;) {   // split at '|' (as Python's split: n '|' -> n + 1 lines)
        size_t b = s.find('|', a);
        lines.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string ln = lines[i];
        int w = 0;          // the columns of ln[0..p)
        long cut = -1;
        for (size_t p = 0; p < ln.size(); p++) {
            if (ln[p] == ' ' && p > 0 && w <= cols) cut = (long)p;
            w += width((unsigned char)ln[p]);
        }
        if (w > cols && cut > 0) {
            std::string head = ln.substr(0, cut), rest = ln.substr(cut + 1);
            lines[i] = head;
            if (!rest.empty()) {
                if (i + 1 < lines.size() && !lines[i + 1].empty())
                    lines[i + 1] = rest + (rest.back() == '-' ? "" : " ") + lines[i + 1];
                else
                    lines.insert(lines.begin() + i + 1, rest);
            }
        }
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) out += (i ? "|" : "") + lines[i];
    return out;
}

// key_text.py's fixed-button form (sms_text): s tokenised, each token
// written as name(token), re-wrapped to cols; s unchanged without keys
template <class N> std::string fixed_text(const std::string &s, N name, int cols)
{
    std::string t = tokenise(s);
    if (!has_token(t)) return s;
    std::string out;
    for (char c : t)
        if (is_token((unsigned char)c)) out += name((unsigned char)c);
        else out += c;
    return rewrap(out, [](unsigned char) { return 1; }, cols);
}
}  // namespace

// see keytext.h
bool is_token(unsigned char c) { return (c >= KEY_USE && c <= KEY_DETAIL) || c == KEY_MENU; }

// see keytext.h (key_text.py tokenise)
std::string tokenise(const std::string &s)
{
    std::string out, rep;
    size_t i = 0, end = 0;
    while (i < s.size()) {
        if (match(s, i, rep, end)) {
            out += rep;
            i = end;
        } else
            out += s[i++];
    }
    return out;
}

// see keytext.h (key_text.py md_text)
std::string md_text(const std::string &s)
{
    std::string t = tokenise(s);
    return has_token(t) ? rewrap(t, md_width, BOX_COLS) : s;
}

// see keytext.h (key_text.py sms_text)
std::string sms_text(const std::string &s, bool gg)
{
    return fixed_text(s, [gg](unsigned char t) { return sms_name(t, gg); }, gg ? GG_COLS : SMS_COLS);
}

// see keytext.h
std::string key_markup(const std::string &s)
{
    std::string out;
    for (char c : tokenise(s))
        if (is_token((unsigned char)c)) out += std::string("{") + markup_name((unsigned char)c) + "}";
        else out += c;
    return out;
}

// see keytext.h: a '{' starts a name only if a '}' follows and what is
// between them is a token's name (in any case); otherwise it stays
std::string key_unmarkup(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        size_t e = s[i] == '{' ? s.find('}', i) : std::string::npos;
        unsigned char tok = 0;
        if (e != std::string::npos) {
            std::string n = s.substr(i + 1, e - i - 1);
            for (auto &c : n) c = (char)toupper((unsigned char)c);
            for (unsigned char t = 1; t <= KEY_MENU; t++)
                if (is_token(t) && n == markup_name(t)) tok = t;
        }
        if (tok) {
            out += (char)tok;
            i = e;
        } else
            out += s[i];
    }
    return out;
}

// see keytext.h (UTF-8: a character's continuation bytes are not counted)
int widest_line(const std::string &s)
{
    int longest = 0, cur = 0;
    bool tok = false;   // (this line has a token: the builders re-wrap it)
    for (size_t i = 0; i <= s.size(); i++) {
        unsigned char c = i < s.size() ? (unsigned char)s[i] : '|';
        if (c == '|' || c == '\n') {   // a line's end
            if (!tok) longest = std::max(longest, cur);
            cur = 0;
            tok = false;
        } else if (is_token(c))
            tok = true;
        else if ((c & 0xC0) != 0x80)
            cur++;
    }
    return longest;
}

}  // namespace keytext
