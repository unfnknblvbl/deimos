# The 3DS toolchain

`build.sh` uses a toolchain built from source (the build machine had no
devkitPro packages): `DKP=<that folder>` holds

- `bin/arm-none-eabi-*`: wrappers around clang 18 (`--target=armv6k-none-eabihf
  -mcpu=mpcore -mfpu=vfpv2 -mfloat-abi=hard -mtp=soft`) and ld.lld;
- `sysroot/`: devkitPro's newlib (its `devkitPro` branch) with libsysbase,
  and compiler-rt's builtins (`libclang_rt.builtins-arm.a`);
- `libctru/`, `citro3d/`: built from their GitHub sources;
- `tools/bin/`: picasso, bin2s, 3dsxtool, smdhtool (3dstools, picasso);
- `crt/`: 3dsx_crt0.o, crti.o, crtn.o and `3dsx.ld` (with a `PT_TLS` program
  header for lld).

On a normal machine use devkitPro: `build.sh` also understands its layout
(devkitARM's gcc in `$DEVKITPRO/devkitARM/bin`, libctru and citro3d in
`$DEVKITPRO/libctru`, the tools in `$DEVKITPRO/tools/bin`), and when the
3DS tools are missing it offers to install devkitPro's package manager and
its `3ds-dev` group (`../tools/deps.sh`; on Arch this adds devkitPro's
repositories to `/etc/pacman.conf`, on Debian / Ubuntu it runs devkitPro's
installer). The devkitPro build has not been tried here: this machine could
not reach devkitPro's servers, so the reference .3dsx is the clang build.
