/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

// Options and results shared by the console builders.
// Licence: GPL-2.0-or-later.
//
// The builders (sms_build, md_build, engine's new-ROM makers) take a
// BuildOptions from the caller (the GUI's build thread or the command-line
// tool) to report progress and to be cancelled, and fill a BuildStats with
// what the ROM used. parallel_for spreads the slow per-cell work (the
// visibility of each cell) over the computer's cores.
#pragma once
#include "common.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// how a build reports back; every member may be left empty
struct BuildOptions {
    int threads = 0;             // 0 = all cores
    std::function<void(const std::string &)> log;   // receives one line of build log
    std::function<void(float)> progress;   // 0..1
    std::atomic<bool> *cancel = nullptr;   // set to true by the caller to stop the build
    // the console being built for (project.h's CONSOLE_KEYS name): the
    // builders use that console's own pictures and texts (Project::
    // for_console); "" = the shared ones only
    std::string console;
    // leave out the ROM's editable copy of the level (a smaller ROM, more
    // room for maps; the editor can't open it afterwards)
    bool locked = false;
    // log one line (nothing when no log is set)
    void say(const std::string &s) const
    {
        if (log) log(s);
    }
    // report progress f, 0..1 (nothing when no callback is set)
    void at(float f) const
    {
        if (progress) progress(f);
    }
    // true once the caller has asked to cancel; builders poll it between steps
    bool cancelled() const { return cancel && *cancel; }
    // worker threads to use: threads, or one per hardware thread (at least 1)
    int n_threads() const { return threads > 0 ? threads : (int)std::max(1u, std::thread::hardware_concurrency()); }
};

// what a build used, for the Build tab and the command-line tool's output
struct BuildStats {
    int rom_kb = 0;                  // the finished ROM's size in KB
    int used_kb = 0, free_kb = 0;    // of the space the level may use
    std::vector<std::string> notes;  // warnings and remarks to show the user
};

// Calls f(i) for every i in 0..n-1 on `threads` worker threads and
// returns when all are done. The threads take the next index from a shared
// atomic counter, so the order is not fixed: f must be safe to run
// concurrently (each call writes only its own results).
template <class F>
void parallel_for(int n, int threads, F f)
{
    std::atomic<int> next(0);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++)
        pool.emplace_back([&]() {
            for (;;) {
                int i = next++;
                if (i >= n) break;
                f(i);
            }
        });
    for (auto &th : pool) th.join();
}
