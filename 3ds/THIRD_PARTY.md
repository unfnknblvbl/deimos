# Third-party code in the 3DS game

The `.3dsx` this folder builds (and the one the Deimos Editor makes, from the
same code) contains, besides Deimos itself (GPL-2.0-or-later, `../LICENSE`):

| Library | What it does here | Licence |
|---|---|---|
| libctru (devkitPro) | the 3DS's system calls: screens, pad, files, sound | zlib: `licences/libctru.txt` |
| citro3d (devkitPro) | the GPU: the 3D view and the pictures over it | zlib: `licences/citro3d.txt` |
| newlib | the C library (memory, strings, files, maths) | BSD-style and similar notices: `licences/newlib.txt` |

When you pass on a `.3dsx`, pass these notices on with it (the `licences/`
folder). The Deimos Editor carries them in its `docs/` folder.
