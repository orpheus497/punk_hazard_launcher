-- theme.lua -- named colour schemes.
--
-- Colours are 0xRRGGBBAA.  Lua 5.4 has real 64-bit integers, so a literal
-- like 0x1affaaff keeps every bit; under 5.1/5.2 it would have arrived as
-- a float and lost the low byte.
--
-- The six float fields drive the CRT fragment shader in src/gfx.c:
--   scanline    darkening of alternate rows, and the RGB triad mask
--   curvature   barrel distortion of the sample coordinate
--   vignette    corner falloff
--   aberration  R/B channel separation, in pixels
--   glow        phosphor bleed (a 4-tap blur added back)
--   noise       animated grain
-- All are 0 = off. Set them all to 0 for a flat, modern look while
-- leaving crt = true.

local M = {}

-- The default. Toxic green on near-black with a hazard-magenta accent.
M.hazard = {
    name        = "HAZARD",
    bg          = 0x07090bff,
    panel       = 0x0e1318ff,
    panel_alt   = 0x141b22ff,
    frame       = 0x1f2b33ff,
    text        = 0xb9c8d2ff,
    text_dim    = 0x5d6f7cff,
    text_bright = 0xeafff7ff,
    accent      = 0x1affaaff,
    accent2     = 0xff2e88ff,
    danger      = 0xff3b3bff,
    ok          = 0x51ff9bff,
    sel_bg      = 0x1affaa2a,
    sel_fg      = 0xffffffff,
    shadow      = 0x000000aa,
    scanline    = 0.30, curvature = 0.030, vignette = 0.35,
    aberration  = 0.60, glow      = 0.28,  noise     = 0.035,
}

-- Amber phosphor, as in a DEC VT220.
M.amber = {
    name        = "AMBER",
    bg          = 0x0b0803ff,
    panel       = 0x14100aff,
    panel_alt   = 0x1c1710ff,
    frame       = 0x33291aff,
    text        = 0xd9a441ff,
    text_dim    = 0x7a5c25ff,
    text_bright = 0xffd98cff,
    accent      = 0xffb000ff,
    accent2     = 0xff6a00ff,
    danger      = 0xff4422ff,
    ok          = 0xffc855ff,
    sel_bg      = 0xffb00030,
    sel_fg      = 0xfff2d0ff,
    shadow      = 0x000000aa,
    scanline    = 0.38, curvature = 0.045, vignette = 0.45,
    aberration  = 0.30, glow      = 0.40,  noise     = 0.05,
}

-- P1 green phosphor, monochrome and heavy.
M.phosphor = {
    name        = "PHOSPHOR",
    bg          = 0x030803ff,
    panel       = 0x081208ff,
    panel_alt   = 0x0c1a0cff,
    frame       = 0x1c331cff,
    text        = 0x66dd66ff,
    text_dim    = 0x2f7a2fff,
    text_bright = 0xccffccff,
    accent      = 0x33ff33ff,
    accent2     = 0x88ff88ff,
    danger      = 0xffdd33ff,
    ok          = 0x33ff33ff,
    sel_bg      = 0x33ff3330,
    sel_fg      = 0xeaffeaff,
    shadow      = 0x000000aa,
    scanline    = 0.45, curvature = 0.055, vignette = 0.50,
    aberration  = 0.15, glow      = 0.50,  noise     = 0.06,
}

-- Cold and clean. Minimal CRT, for a modern flat panel.
M.ice = {
    name        = "ICE",
    bg          = 0x080b10ff,
    panel       = 0x101620ff,
    panel_alt   = 0x161e2aff,
    frame       = 0x243244ff,
    text        = 0xc2d4e8ff,
    text_dim    = 0x64788fff,
    text_bright = 0xf0f7ffff,
    accent      = 0x4cc9f0ff,
    accent2     = 0x7b61ffff,
    danger      = 0xff5c7aff,
    ok          = 0x4cf0b0ff,
    sel_bg      = 0x4cc9f028,
    sel_fg      = 0xffffffff,
    shadow      = 0x000000aa,
    scanline    = 0.10, curvature = 0.010, vignette = 0.22,
    aberration  = 0.20, glow      = 0.15,  noise     = 0.015,
}

-- Hot magenta and orange.
M.blood = {
    name        = "BLOOD",
    bg          = 0x0c0508ff,
    panel       = 0x150a10ff,
    panel_alt   = 0x1d0f16ff,
    frame       = 0x3a1a28ff,
    text        = 0xe8c2d0ff,
    text_dim    = 0x8a5a6dff,
    text_bright = 0xfff0f5ff,
    accent      = 0xff2e63ff,
    accent2     = 0xff9f1cff,
    danger      = 0xff2e63ff,
    ok          = 0xffb703ff,
    sel_bg      = 0xff2e6330,
    sel_fg      = 0xffffffff,
    shadow      = 0x000000aa,
    scanline    = 0.32, curvature = 0.035, vignette = 0.40,
    aberration  = 0.80, glow      = 0.30,  noise     = 0.04,
}

return M
