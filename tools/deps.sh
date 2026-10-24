# Deimos - Doom RPG on retro consoles
# Copyright (C) 2026 unfnknblvbl and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
# Free software under the GNU General Public License, version 2 or (at your
# option) any later version, with NO WARRANTY: see the LICENSE file.

# deps.sh - finds, and with your permission installs, what the build scripts
# need. Every build script in Deimos sources this file and then calls
#
#     deps_require ITEM ...
#
# with the items it uses (the list is in deps_check below: python3, pillow,
# cc65, wla, m68k-elf, gbdk, ...). For each missing item it works out how to
# get it on this Linux distribution:
#
#   - from the distribution's own packages, with its package manager
#     (pacman on Arch / CachyOS / Manjaro / EndeavourOS, apt on Debian /
#     Ubuntu / Mint / Pop!_OS, dnf on Fedora, zypper on openSUSE); or
#   - for tools no distribution packages (the 68000 and SH-2 cross compilers,
#     WLA-DX, ZX0, GBDK-2020, cc65 where it is not packaged), by downloading
#     and building them from source into one folder, $DEIMOS_TOOLS
#     (default ~/.local/share/deimos-tools), without needing root.
#
# It shows the whole plan and asks once before changing anything. Nothing is
# installed without a yes.
#
# Environment:
#   DEIMOS_TOOLS=DIR     where the tools built from source go
#   DEIMOS_YES=1         install without asking (for unattended builds)
#   DEIMOS_NO_INSTALL=1  never install: only report what is missing
#
# Every build script in the repository sources this one file.

DEIMOS_TOOLS=${DEIMOS_TOOLS:-$HOME/.local/share/deimos-tools}

# The versions built from source: the ones the reference ROMs were made with,
# so that a build here gives the same bytes.
DEPS_BINUTILS=binutils-2.43.1
DEPS_GCC=gcc-13.3.0
DEPS_GNU_MIRROR=${DEPS_GNU_MIRROR:-https://ftpmirror.gnu.org/gnu}
DEPS_WLA_REPO=https://github.com/vhelin/wla-dx.git
DEPS_WLA_COMMIT=7ae11df             # WLA-DX 10.8a
DEPS_CC65_REPO=https://github.com/cc65/cc65.git
DEPS_CC65_COMMIT=8d86ca7            # cc65 2.19 (git)
DEPS_ZX0_REPO=https://github.com/einar-saukas/ZX0.git
DEPS_ZX0_COMMIT=ecde3a2             # ZX0 2.2
DEPS_GBDK_URL=https://github.com/gbdk-2020/gbdk-2020/releases/download/4.5.0/gbdk-linux64.tar.gz

# deps_path: puts the tools already built in $DEIMOS_TOOLS on PATH (and GBDK
# in $GBDK), so the checks and the builds find them. Runs when this file is
# sourced.
deps_path() {
    local d
    for d in wla-dx cc65 zx0 m68k-elf sh-elf arm-none-eabi; do
        if [ -d "$DEIMOS_TOOLS/$d/bin" ]; then
            case ":$PATH:" in
                *":$DEIMOS_TOOLS/$d/bin:"*) ;;                 # already there
                *) PATH="$DEIMOS_TOOLS/$d/bin:$PATH" ;;
            esac
        fi
    done
    export PATH
    if [ -z "${GBDK:-}" ] && [ -x "$DEIMOS_TOOLS/gbdk/bin/lcc" ]; then
        export GBDK="$DEIMOS_TOOLS/gbdk"
    fi
}

# deps_family: prints which family of distribution this is (arch, debian,
# fedora, suse) from /etc/os-release's ID and ID_LIKE, or "unknown".
deps_family() {
    local id="" like=""
    if [ -r /etc/os-release ]; then
        id=$(. /etc/os-release; echo "${ID:-}")
        like=$(. /etc/os-release; echo "${ID_LIKE:-}")
    fi
    case " $id $like " in
        *" arch "* | *" cachyos "* | *" manjaro "* | *" endeavouros "*) echo arch ;;
        *" debian "* | *" ubuntu "*) echo debian ;;
        *" fedora "* | *" rhel "* | *" centos "*) echo fedora ;;
        *" suse "* | *" opensuse "* | *" opensuse-tumbleweed "* | *" opensuse-leap "*) echo suse ;;
        *) echo unknown ;;
    esac
}

# deps_check ITEM: succeeds when ITEM is already available. The items:
#   python3 pillow numpy   Python 3 and its imaging and array modules
#   cc make unzip curl git cmake cxx pkgconf   general build tools
#   mingw                  the Windows cross compiler (the editor's Windows build)
#   sdl-deps               the X11/Wayland/audio headers SDL2 is built against
#   wla-z80 wla-spc700 wla-superfx   WLA-DX's assemblers (and wlalink) for the
#                          Z80, the SNES sound CPU and the Super FX
#   cc65                   cc65, ca65, ld65 (the SNES's C compiler and assembler)
#   zx0                    the ZX0 compressor
#   m68k-elf sh-elf        the 68000 and SH-2 cross compilers (Mega Drive, 32X)
#   arm-none-eabi          the ARM cross compiler (Game Boy Advance)
#   gbdk                   GBDK-2020 (Game Boy Color)
#   devkitpro-3ds          devkitPro's 3DS libraries and tools
deps_check() {
    case $1 in
        python3) command -v python3 >/dev/null ;;
        pillow) python3 -c "import PIL" 2>/dev/null ;;
        numpy) python3 -c "import numpy" 2>/dev/null ;;
        cc) command -v cc >/dev/null || command -v gcc >/dev/null ;;
        cxx) command -v c++ >/dev/null || command -v g++ >/dev/null ;;
        make | unzip | curl | git | cmake) command -v "$1" >/dev/null ;;
        pkgconf) command -v pkg-config >/dev/null ;;
        mingw) command -v x86_64-w64-mingw32-g++ >/dev/null ;;
        sdl-deps) command -v pkg-config >/dev/null && pkg-config --exists x11 2>/dev/null ;;   # (X11 is the minimum SDL2 needs; the rest are extras)
        wla-z80 | wla-spc700 | wla-superfx)
             command -v "$1" >/dev/null && command -v "${WLALINK:-wlalink}" >/dev/null ;;
        cc65) command -v cc65 >/dev/null && command -v ca65 >/dev/null && command -v ld65 >/dev/null ;;
        zx0) command -v "${ZX0:-zx0}" >/dev/null ;;
        m68k-elf) command -v "${CROSS:-m68k-elf-}gcc" >/dev/null || command -v "${M68K:-m68k-elf}-gcc" >/dev/null ;;
        sh-elf) command -v "${SH2:-sh-elf}-gcc" >/dev/null ;;
        arm-none-eabi) command -v "${ARM:-arm-none-eabi}-gcc" >/dev/null ;;
        gbdk) [ -x "${GBDK:-/opt/gbdk}/bin/lcc" ] ;;
        devkitpro-3ds) local d=${DKP:-${DEVKITPRO:-/opt/devkitpro}}
                       [ -x "$d/tools/bin/3dsxtool" ] && [ -d "$d/libctru" ] ;;
        *) echo "deps.sh: unknown item $1" >&2; return 1 ;;
    esac
}

# deps_packages ITEM FAMILY: prints the distribution packages that provide
# ITEM on FAMILY, or "@source" when it is built from source here instead, or
# "@devkitpro" for devkitPro's own package manager, or nothing when this
# script does not know (the item is then reported for you to install).
deps_packages() {
    local item=$1 fam=$2
    case $item:$fam in
        python3:arch) echo python ;;            python3:*) echo python3 ;;
        pillow:arch) echo python-pillow ;;      pillow:debian) echo python3-pil ;;
        pillow:fedora) echo python3-pillow ;;   pillow:suse) echo python3-Pillow ;;
        numpy:arch) echo python-numpy ;;        numpy:*) echo python3-numpy ;;
        cc:arch) echo base-devel ;;             cc:debian) echo build-essential ;;
        cc:*) echo gcc make ;;
        cxx:arch) echo gcc ;;                   cxx:debian) echo g++ ;;
        cxx:*) echo gcc-c++ ;;
        make:* | unzip:* | curl:* | git:* | cmake:*) echo "$item" ;;
        pkgconf:arch) echo pkgconf ;;           pkgconf:debian) echo pkg-config ;;
        pkgconf:fedora) echo pkgconf-pkg-config ;; pkgconf:suse) echo pkg-config ;;
        mingw:arch) echo mingw-w64-gcc ;;       mingw:debian) echo g++-mingw-w64-x86-64-posix ;;
        mingw:fedora) echo mingw64-gcc-c++ ;;   mingw:suse) echo mingw64-cross-gcc-c++ ;;
        sdl-deps:arch) echo libx11 libxext libxrandr libxcursor libxi libxss libxkbcommon wayland mesa alsa-lib libpulse ;;
        sdl-deps:debian) echo libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
                              libxkbcommon-dev libwayland-dev libegl-dev libgl-dev libasound2-dev libpulse-dev ;;
        sdl-deps:fedora) echo libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel \
                              libXScrnSaver-devel libxkbcommon-devel wayland-devel mesa-libEGL-devel \
                              mesa-libGL-devel alsa-lib-devel pulseaudio-libs-devel ;;
        sdl-deps:suse) echo libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel \
                            libXss-devel libxkbcommon-devel wayland-devel Mesa-libEGL-devel Mesa-libGL-devel \
                            alsa-devel libpulse-devel ;;
        cc65:debian | cc65:fedora) echo cc65 ;;
        arm-none-eabi:arch) echo arm-none-eabi-gcc arm-none-eabi-binutils ;;
        arm-none-eabi:debian) echo gcc-arm-none-eabi binutils-arm-none-eabi ;;
        arm-none-eabi:fedora) echo arm-none-eabi-gcc-cs arm-none-eabi-binutils-cs ;;
        cc65:* | wla-*:* | zx0:* | m68k-elf:* | sh-elf:* | arm-none-eabi:* | gbdk:*) echo @source ;;
        devkitpro-3ds:arch | devkitpro-3ds:debian) echo @devkitpro ;;
    esac
}

# deps_source_needs ITEM: prints the other items a source build of ITEM needs
# first (compilers, downloaders, the GMP/MPFR/MPC libraries for GCC).
deps_source_needs() {
    case $1 in
        wla-*) echo cc cxx make cmake git ;;
        cc65 | zx0) echo cc make git ;;
        gbdk) echo curl ;;
        m68k-elf | sh-elf | arm-none-eabi) echo cc cxx make curl gcc-libs ;;
    esac
}

# deps_gcc_libs FAMILY: the packages GCC's own build needs (GMP, MPFR, MPC, xz).
deps_gcc_libs() {
    case $1 in
        arch) echo gmp mpfr libmpc xz ;;
        debian) echo libgmp-dev libmpfr-dev libmpc-dev xz-utils ;;
        fedora) echo gmp-devel mpfr-devel libmpc-devel xz ;;
        suse) echo gmp-devel mpfr-devel mpc-devel xz ;;
    esac
}

# deps_sudo: prints "sudo" unless we are already root.
deps_sudo() { [ "$(id -u)" = 0 ] || echo sudo; }

# deps_pm_install FAMILY PACKAGE...: installs packages with the family's
# package manager (only the ones not installed yet, where it can tell).
deps_pm_install() {
    local fam=$1; shift
    local s; s=$(deps_sudo)
    case $fam in
        arch) $s pacman -S --needed --noconfirm "$@" ;;
        debian) $s apt-get update && $s apt-get install -y "$@" ;;
        fedora) $s dnf install -y "$@" ;;
        suse) $s zypper --non-interactive install "$@" ;;
    esac
}

# deps_require ITEM...: makes sure every ITEM is available. Prints what is
# missing and the plan to get it, asks once (unless DEIMOS_YES=1), carries
# the plan out, then checks again. Returns non-zero (the calling build stops)
# when something is still missing.
deps_require() {
    deps_path
    local missing=() it
    for it in "$@"; do deps_check "$it" || missing+=("$it"); done
    [ ${#missing[@]} -eq 0 ] && return 0

    local fam pk pkgs=() src=() dkp=0 manual=() need
    fam=$(deps_family)
    for it in "${missing[@]}"; do
        pk=$(deps_packages "$it" "$fam")
        case $pk in
            "") manual+=("$it") ;;
            @devkitpro) dkp=1 ;;
            @source)
                case $it in wla-*) it=wla ;; esac                 # (one WLA-DX build has every assembler)
                case " ${src[*]:-} " in *" $it "*) continue ;; esac
                src+=("$it")
                for need in $(deps_source_needs "$it"); do     # what building it needs
                    if [ "$need" = gcc-libs ]; then
                        pkgs+=($(deps_gcc_libs "$fam"))
                    elif ! deps_check "$need"; then
                        pkgs+=($(deps_packages "$need" "$fam"))
                    fi
                done ;;
            *) pkgs+=($pk) ;;
        esac
    done
    # (no duplicates)
    [ ${#pkgs[@]} -gt 0 ] && mapfile -t pkgs < <(printf '%s\n' "${pkgs[@]}" | awk '!seen[$0]++')

    echo "Missing for this build: ${missing[*]}"
    [ ${#pkgs[@]} -gt 0 ] && [ "$fam" != unknown ] && \
        echo "  install with your package manager ($fam): ${pkgs[*]}"
    [ ${#src[@]} -gt 0 ] && echo "  download and build from source into $DEIMOS_TOOLS: ${src[*]}" \
        && case " ${src[*]} " in *" m68k-elf "* | *" sh-elf "* | *" arm-none-eabi "*)
            echo "    (each cross compiler takes about 10-30 minutes to build)" ;; esac
    [ $dkp = 1 ] && echo "  install devkitPro's package manager and its 3ds-dev group (devkitpro.org)"
    if [ "$fam" = unknown ] || [ ${#manual[@]} -gt 0 ]; then
        echo "  not known how to install on this system: ${manual[*]:-${pkgs[*]}}"
        echo "  (README.md, \"Toolchains\", says where each comes from)"
    fi
    if [ -n "${DEIMOS_NO_INSTALL:-}" ]; then return 1; fi
    if [ -z "${DEIMOS_YES:-}" ]; then
        if [ ! -t 0 ]; then
            echo "Run again with DEIMOS_YES=1 to install these, or install them yourself." >&2
            return 1
        fi
        local yn
        read -r -p "Go ahead? [Y/n] " yn
        case ${yn:-y} in y | Y | yes | Yes) ;; *) return 1 ;; esac
    fi

    if [ ${#pkgs[@]} -gt 0 ] && [ "$fam" != unknown ]; then
        deps_pm_install "$fam" "${pkgs[@]}" || return 1
    fi
    [ $dkp = 1 ] && { deps_devkitpro "$fam" || return 1; }
    for it in "${src[@]}"; do
        echo "== Building $it into $DEIMOS_TOOLS"
        case $it in
            wla) deps_build_wla ;;
            cc65) deps_build_cc65 ;;
            zx0) deps_build_zx0 ;;
            gbdk) deps_get_gbdk ;;
            m68k-elf) deps_build_cross m68k-elf --with-cpu=68000 --disable-multilib ;;
            sh-elf) deps_build_cross sh-elf --with-cpu=m2 --with-multilib-list=m2 ;;
            arm-none-eabi) deps_build_cross arm-none-eabi --with-cpu=arm7tdmi --with-float=soft --disable-multilib ;;
        esac || { echo "Building $it failed." >&2; return 1; }
    done

    deps_path
    local still=()
    for it in "${missing[@]}"; do deps_check "$it" || still+=("$it"); done
    if [ ${#still[@]} -gt 0 ]; then
        echo "Still missing: ${still[*]} (README.md, \"Toolchains\")" >&2
        return 1
    fi
    echo "All set."
}

# deps_git_at REPO COMMIT DIR: a checkout of REPO at COMMIT in DIR (cloned
# once, reused after).
deps_git_at() {
    [ -d "$3/.git" ] || git clone -q "$1" "$3"
    git -C "$3" checkout -q "$2"
}

# deps_build_wla: WLA-DX (all its assemblers and wlalink) into $DEIMOS_TOOLS/wla-dx.
deps_build_wla() {
    local w=$DEIMOS_TOOLS/src/wla-dx
    mkdir -p "$DEIMOS_TOOLS/src"
    deps_git_at $DEPS_WLA_REPO $DEPS_WLA_COMMIT "$w" &&
    cmake -S "$w" -B "$w/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEIMOS_TOOLS/wla-dx" >/dev/null &&
    cmake --build "$w/build" -j"$(nproc)" >/dev/null &&
    cmake --install "$w/build" >/dev/null
}

# deps_build_cc65: cc65, ca65, ld65 and their libraries into $DEIMOS_TOOLS/cc65.
deps_build_cc65() {
    local c=$DEIMOS_TOOLS/src/cc65
    mkdir -p "$DEIMOS_TOOLS/src"
    deps_git_at $DEPS_CC65_REPO $DEPS_CC65_COMMIT "$c" &&
    make -C "$c" -j"$(nproc)" >/dev/null &&
    make -C "$c" PREFIX="$DEIMOS_TOOLS/cc65" install >/dev/null
}

# deps_build_zx0: the zx0 compressor into $DEIMOS_TOOLS/zx0/bin.
deps_build_zx0() {
    local z=$DEIMOS_TOOLS/src/ZX0
    mkdir -p "$DEIMOS_TOOLS/src" "$DEIMOS_TOOLS/zx0/bin"
    deps_git_at $DEPS_ZX0_REPO $DEPS_ZX0_COMMIT "$z" &&
    cc -O2 -o "$DEIMOS_TOOLS/zx0/bin/zx0" "$z"/src/zx0.c "$z"/src/compress.c "$z"/src/optimize.c "$z"/src/memory.c
}

# deps_get_gbdk: GBDK-2020's ready-made Linux release into $DEIMOS_TOOLS/gbdk.
deps_get_gbdk() {
    mkdir -p "$DEIMOS_TOOLS"
    curl -fL --progress-bar "$DEPS_GBDK_URL" | tar xz -C "$DEIMOS_TOOLS"    # (unpacks as gbdk/)
}

# deps_build_cross TARGET CONFIGURE-OPTIONS...: GNU binutils and GCC (C only,
# no C library: the games bring their own) for TARGET, into
# $DEIMOS_TOOLS/TARGET. The sources are downloaded once to $DEIMOS_TOOLS/src.
deps_build_cross() {
    local t=$1; shift
    local s=$DEIMOS_TOOLS/src p=$DEIMOS_TOOLS/$t b=$DEIMOS_TOOLS/src/build-$t
    mkdir -p "$s" "$p" "$b"
    (
        set -e
        cd "$s"
        [ -f $DEPS_BINUTILS.tar.xz ] || curl -fLO "$DEPS_GNU_MIRROR/binutils/$DEPS_BINUTILS.tar.xz"
        [ -f $DEPS_GCC.tar.xz ] || curl -fLO "$DEPS_GNU_MIRROR/gcc/$DEPS_GCC/$DEPS_GCC.tar.xz"
        [ -d $DEPS_BINUTILS ] || tar xf $DEPS_BINUTILS.tar.xz
        [ -d $DEPS_GCC ] || tar xf $DEPS_GCC.tar.xz
        # binutils: the assembler and linker (MAKEINFO=true: no manuals, so no texinfo needed)
        mkdir -p "$b/binutils" && cd "$b/binutils"
        "$s/$DEPS_BINUTILS/configure" --target=$t --prefix="$p" --disable-nls --disable-werror \
            --disable-gdb --disable-gdbserver --disable-sim --disable-gprofng --without-zstd MAKEINFO=true >/dev/null
        make -j"$(nproc)" MAKEINFO=true >/dev/null
        make install MAKEINFO=true >/dev/null
        # gcc: the compiler and libgcc only, for a bare machine
        export PATH="$p/bin:$PATH"
        mkdir -p "$b/gcc" && cd "$b/gcc"
        "$s/$DEPS_GCC/configure" --target=$t --prefix="$p" --enable-languages=c --disable-nls \
            --disable-libssp --disable-threads --disable-shared --without-headers --with-newlib \
            --disable-libquadmath --disable-libgomp --disable-libatomic --disable-lto --without-zstd \
            "$@" MAKEINFO=true >/dev/null
        make -j"$(nproc)" all-gcc all-target-libgcc MAKEINFO=true >/dev/null
        make install-gcc install-target-libgcc MAKEINFO=true >/dev/null
    )
}

# deps_devkitpro FAMILY: devkitPro's package manager and its 3DS packages
# (3ds-dev: devkitARM, libctru, citro3d, the 3DS tools) in /opt/devkitpro.
# On Arch this adds devkitPro's signed repositories to /etc/pacman.conf; on
# Debian and Ubuntu it runs devkitPro's own installer for dkp-pacman.
deps_devkitpro() {
    local s; s=$(deps_sudo)
    case $1 in
        arch)
            # devkitPro's signing key and keyring, then its two repositories
            $s pacman-key --recv BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver keyserver.ubuntu.com &&
            $s pacman-key --lsign BC26F752D25B92CE272E0F44F7FD5492264BB9D0 &&
            $s pacman -U --noconfirm https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst || return 1
            if ! grep -q '^\[dkp-libs\]' /etc/pacman.conf; then
                printf '\n[dkp-libs]\nServer = https://pkg.devkitpro.org/packages\n\n[dkp-linux]\nServer = https://pkg.devkitpro.org/packages/linux/$arch/\n' \
                    | $s tee -a /etc/pacman.conf >/dev/null
            fi
            $s pacman -Sy --needed --noconfirm 3ds-dev ;;
        debian)
            local t; t=$(mktemp)
            curl -fsSL https://apt.devkitpro.org/install-devkitpro-pacman -o "$t" &&
            chmod +x "$t" && $s "$t" && rm -f "$t" &&
            $s dkp-pacman -S --needed --noconfirm 3ds-dev ;;
        *) return 1 ;;
    esac
    echo "devkitPro is in /opt/devkitpro (open a new terminal, or: export DEVKITPRO=/opt/devkitpro)"
    export DEVKITPRO=/opt/devkitpro
}

deps_path
