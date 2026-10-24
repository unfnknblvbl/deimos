/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// The phone's keys in the game's texts, made into the console's buttons:
// the same as tools/key_text.py (in megadrive/ and in mastersystem/), so the
// texts the builders write are byte-for-byte those of the Python builds.
// Licence: GPL-2.0-or-later.
//
// The original's help texts name the phone's keys ("press the OK|button",
// "the * and 7 keys", "the # key", "the 0|key", "the 9 button"; '|' is a
// line break). A "token" is a byte below 32 standing for an action's
// button (KEY_USE.. KEY_DETAIL = 1 + the action's ACT_* number, KEY_MENU
// the pause menu's Start); tokenise() puts one where a text names a key,
// and "key(s)" becomes "button(s)". The Mega Drive and the SNES-family
// games write a token as the name of the button that does that action now
// (they can be moved on the options screen): md_text() makes the texts of
// their campaign block. The Master System and Game Gear have fixed buttons:
// sms_text() writes the names in.
//
// In the Text tab a token is shown and typed as its name in braces, e.g.
// {NEXT WEAPON} (key_markup / key_unmarkup).
#pragma once
#include <string>

namespace keytext {

// the tokens (tools/key_text.py KEY_*)
enum { KEY_USE = 1, KEY_PASS, KEY_MEDKIT, KEY_PREV, KEY_NEXT, KEY_NOTES, KEY_DETAIL, KEY_MENU = 16 };

// is byte c one of the tokens?
bool is_token(unsigned char c);
// s (Latin-1 or UTF-8: only ASCII is looked at) with each key phrase made
// into tokens; the rest unchanged
std::string tokenise(const std::string &s);
// a Latin-1 text as the Mega Drive's campaign block holds it: tokenised
// and, if it has a token, re-wrapped for the widest button names (21
// columns a line); a text without keys comes back unchanged
std::string md_text(const std::string &s);
// a Latin-1 text as the Master System (gg: the Game Gear) shows it: the
// tokens written as that console's buttons and, if it had any, re-wrapped
// to its box (21 columns; the Game Gear's 19); unchanged without keys
std::string sms_text(const std::string &s, bool gg);

// a text as the Text tab shows it: tokenised, each token as {NAME}
std::string key_markup(const std::string &s);
// and back: each {NAME} (any case) made into its token
std::string key_unmarkup(const std::string &s);
// the letters of the longest line of s ('|' or new line separated; UTF-8)
// that has no token (a line with one is re-wrapped by the builders to fit
// the console's box): for the Text tab's warning
int widest_line(const std::string &s);

}  // namespace keytext
