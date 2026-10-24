/* Deimos - Doom RPG on retro consoles
 * Copyright (C) 2026 unfnknblvbl and contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Free software under the GNU General Public License, version 2 or (at your
 * option) any later version, with NO WARRANTY: see the LICENSE file.
 */

/*
 * Doom RPG for the 32X: the 68000's part. The game runs on the SH-2s
 * (sh2/); the 68000 sets up the Mega Drive's own picture (nothing: the
 * 32X's picture is in front of it), lets the SH-2s go, and then reads the
 * pad each frame for them and keeps the saved games (the cartridge's
 * battery RAM, which only the 68000 reaches).
 *
 * The COMM registers ($A15120..):
 *   +0  start-up: "SH" from the master SH-2 once it has the 32X's picture
 *   +2  the pad (6 buttons: the SNES pad's bits, sh2/plat.c)
 *   +4  the frame count (the game's clock)
 *   +8  the battery RAM: 1 read, 2 write (the SH-2's request; 0 done),
 *       +10 the byte's index, +12 its value
 *   +14 a test's result (the 68000 copies it to work RAM $FF0000)
 * (The offsets are in bytes; each COMM register is 16 bits. Those not
 * listed are not used by this file.)
 *
 * Called from m68k/boot.s (jmp main) after the 32X adapter is on; main()
 * never returns. Nothing here uses RAM variables: the code is linked into
 * the cartridge (m68k/m68k.ld), so all state lives in locals (registers or
 * the stack in work RAM).
 * Licence: GPL-2.0-or-later.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;

/* REG8/REG16: a byte or word hardware register at address a */
#define REG8(a) (*(volatile u8 *)(a))
#define REG16(a) (*(volatile u16 *)(a))
#define VDP_DATA REG16(0xC00000)        /* the Mega Drive VDP's data port */
#define VDP_CTRL REG16(0xC00004)        /* its control port; reads give the status */
#define A15100 REG16(0xA15100)          /* 32X adapter control (68000 side) */
#define COMM(n) REG16(0xA15120 + (n))   /* the COMM register at byte offset n (see above) */
#define PAD_DATA REG8(0xA10003)         /* controller port 1: data */
#define PAD_CTRL REG8(0xA10009)         /* controller port 1: direction (1 bits are outputs) */

/* vdp_reg: write value v to VDP register r (a control-port word $8000 |
 * r << 8 | v). Called by vdp_init. */
static void vdp_reg(u8 r, u8 v) { VDP_CTRL = (u16)(0x8000 | (r << 8) | v); }

/*
 * vdp_init: set the Mega Drive VDP to a blank H40 picture and clear all of
 * its video RAM and colour RAM, so that only the 32X's picture shows.
 * Inputs: none. Output: the VDP's registers, VRAM and CRAM. Called once by
 * main.
 */
static void vdp_init(void)
{
    int i;
    (void)VDP_CTRL;             /* a status read resets the control port's write state */
    vdp_reg(0, 0x04);           /* H-interrupts off */
    vdp_reg(1, 0x04);           /* display off, Mega Drive mode */
    vdp_reg(2, 0x30);           /* plane A table at $C000 */
    vdp_reg(3, 0x2C);           /* window table at $B000 */
    vdp_reg(4, 0x07);           /* plane B table at $E000 */
    vdp_reg(5, 0x54);           /* sprite table at $A800 */
    vdp_reg(7, 0x00);           /* backdrop: colour 0 */
    vdp_reg(10, 0xFF);          /* H-interrupt counter (H-interrupts are off anyway) */
    vdp_reg(11, 0x00);          /* full-screen scrolling, no external interrupt */
    vdp_reg(12, 0x81);          /* H40 (the 32X's picture is 320 wide) */
    vdp_reg(13, 0x2B);          /* horizontal scroll table at $AC00 */
    vdp_reg(15, 0x02);          /* auto-increment: 2 bytes per data-port access */
    vdp_reg(16, 0x01);          /* planes 64 x 32 cells */
    /* VRAM and CRAM cleared: nothing of the Mega Drive's own shows */
    VDP_CTRL = 0x4000;          /* command: VRAM write from address 0 */
    VDP_CTRL = 0x0000;
    for (i = 0; i < 32768; i++) VDP_DATA = 0;   /* all 64 KB of VRAM, a word at a time */
    VDP_CTRL = 0xC000;          /* command: CRAM write from address 0 */
    VDP_CTRL = 0x0000;
    for (i = 0; i < 64; i++) VDP_DATA = 0;      /* all 64 colours black */
    vdp_reg(1, 0x44);           /* display on */
}

/*
 * wait_vblank: wait for the start of the next vertical blank. Status bit 3
 * is the VDP's "in vertical blank" flag: first wait while it is set (so a
 * call made during a blank waits for the next one), then until it is set.
 * Called once a frame by main.
 */
static void wait_vblank(void)
{
    while (VDP_CTRL & 8) {}
    while (!(VDP_CTRL & 8)) {}
}

/*
 * pad_read: read controller 1 (3- or 6-button) and return its state as
 * the SNES pad's bits (the game's buttons: sh2/plat.c), 1 = pressed:
 *   $8000 B   $4000 Y   $2000 Select   $1000 Start
 *   $0800 up  $0400 down  $0200 left  $0100 right
 *   $0080 A   $0040 X   $0020 L   $0010 R
 * The pad is read by toggling its TH line (bit 6 of the data port): each
 * TH change selects a different set of buttons, and on a 6-button pad the
 * third low phase reads 0 in the direction bits and the following high
 * phase gives X, Y, Z and Mode. The raw bits are active low. The bit
 * patterns in the comments name each bit from bit 7 down to bit 0.
 * Called once a frame by main (the 6-button pad's TH sequence restarts
 * after a pause, so one read per frame always starts from its first phase).
 */
static u16 pad_read(void)
{
    u8 a, b, c;
    u16 p = 0;
    PAD_DATA = 0x40;            /* TH high */
    __asm__ volatile("nop\n\tnop");   /* let the pad's outputs settle */
    a = PAD_DATA;               /* ?1CBRLDU */
    PAD_DATA = 0x00;            /* TH low */
    __asm__ volatile("nop\n\tnop");
    b = PAD_DATA;               /* ?0SA00DU */
    PAD_DATA = 0x40;            /* two more TH pulses: to the 6-button pad's */
    PAD_DATA = 0x00;            /* third low phase */
    PAD_DATA = 0x40;
    PAD_DATA = 0x00;
    __asm__ volatile("nop\n\tnop");
    c = PAD_DATA;               /* 6-button: ?0SA0000 */
    PAD_DATA = 0x40;
    __asm__ volatile("nop\n\tnop");
    {
        u8 x = PAD_DATA;        /* 6-button: ?1CBMXYZ */
        PAD_DATA = 0x00;        /* finish the sequence, leaving TH high */
        PAD_DATA = 0x40;
        a = (u8)~a;             /* active low -> 1 = pressed */
        b = (u8)~b;
        if (a & 0x01) p |= 0x0800;      /* up */
        if (a & 0x02) p |= 0x0400;      /* down */
        if (a & 0x04) p |= 0x0200;      /* left */
        if (a & 0x08) p |= 0x0100;      /* right */
        if (b & 0x10) p |= 0x0080;      /* A: SNES A (use) */
        if (a & 0x10) p |= 0x8000;      /* B: SNES B (pass) */
        if (a & 0x20) p |= 0x4000;      /* C: SNES Y (medkit) */
        if (b & 0x20) p |= 0x1000;      /* Start */
        if ((c & 0x0F) == 0) {          /* a 6-button pad */
            x = (u8)~x;
            if (x & 0x04) p |= 0x0020;  /* X: L (previous weapon) */
            if (x & 0x01) p |= 0x0010;  /* Z: R (next weapon) */
            if (x & 0x02) p |= 0x0040;  /* Y: SNES X (notebook) */
            if (x & 0x08) p |= 0x2000;  /* Mode: Select */
        }
    }
    return p;
}

/* SRAM(i): battery RAM byte i (0..8191). The battery RAM sits on the odd
 * bytes from $200001; the 32X bank register maps that 1 MB bank to the
 * window at $900000, so byte i is at $900001 + 2 * i. */
#define SRAM(i) REG8(0x900001 + 2 * (u32)(i))

/*
 * main: the 68000's whole job. Sets up the VDP and the pad port, releases
 * the SH-2s, hands the 32X's picture to them, waits for the master SH-2,
 * and then loops forever, once a frame: count the frame into COMM+4, put
 * the pad in COMM+2, serve any battery RAM requests in COMM+8..12, and
 * copy COMM+14 to work RAM $FF0000 (where a test harness reads it).
 * Called from m68k/boot.s; never returns.
 */
void main(void)
{
    u16 frames = 0;                     /* (no RAM variables: the 68000's code has none) */
    vdp_init();                         /* blank the Mega Drive's own picture */
    PAD_CTRL = 0x40;                    /* port 1: TH (bit 6) an output, the rest inputs */
    PAD_DATA = 0x40;                    /* TH high: the pad's idle state */
    REG16(0xA15104) = 2;                /* the bank window at $900000: the battery RAM ($200000) */
    REG8(0xA15101) = 3;                 /* the SH-2s out of reset */
    while (REG16(0xA15124) != 0x535F) {}        /* the slave's "S_OK" (the boot ROM's) */
    A15100 = 0x8003;                    /* the 32X's picture to the SH-2s */
    while (COMM(0) != 0x5348) {}        /* "SH": the master SH-2 is going */
    for (;;) {
        
        wait_vblank();                  /* once a frame */
        COMM(4) = ++frames;             /* the game's clock (wraps at 65536) */
        COMM(2) = pad_read();           /* the buttons, in SNES layout */
        if (COMM(8)) {                  /* the battery RAM, a byte at a time, while the SH-2 asks */
            u16 idle = 0, k = 0;        /* idle: empty looks in a row; k: bytes served */
            REG8(0xA130F1) = 3;                         /* the battery RAM on, writable */
            /* serve requests until 6000 bytes have gone or the SH-2 has
               asked for nothing for 400 looks (it has finished) */
            while (k < 6000 && idle < 400) {
                u16 c = COMM(8), i;
                if (!c) {
                    /* (a while between looks: emulators that see the 68000 poll a
                       COMM register stop it until the SH-2 writes there) */
                    u8 w;
                    for (w = 0; w < 4; w++) (void)REG16(0xC00008);   /* (the VDP's H/V counter, read only to pass time) */
                    idle++;
                    continue;
                }
                i = COMM(10) & 0x1FFF;          /* the byte's index, kept within 8 KB */
                if (c == 1) COMM(12) = SRAM(i); /* 1: read it back to the SH-2 */
                else SRAM(i) = (u8)COMM(12);    /* 2: write it */
                COMM(8) = 0;                    /* done: the SH-2 may send the next */
                idle = 0;
                k++;
            }
            REG8(0xA130F1) = 0;                         /* the battery RAM off (ROM back) */
        }
        *(volatile u16 *)0xFF0000 = COMM(14);   /* a test's result, for the harness */
    }
}
