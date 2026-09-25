-- layout.lua -- UI metrics, in pixels at 1x.
--
-- These are logical sizes; app.c scales them by the window's DPI factor so
-- the same numbers hold on a 4K panel.
--
-- The screen is four regions:
--
--   +-------------------------------------------------+  top_height
--   |  wordmark        selected game, in detail        |
--   +----------------------------+--------------------+
--   |                            |  details           |
--   |   game icon grid           |  ----------        |  split
--   |   (split of the width)     |  options           |
--   +----------------------------+--------------------+
--   |  cpu   memory   load   uptime   host            |  bottom_height
--   +-------------------------------------------------+

return {
    pad              = 20,

    top_height       = 116,  -- wordmark + expanded detail for the selection
    bottom_height    = 96,   -- system monitor
    split            = 0.70, -- grid takes this fraction, panel takes the rest
                             -- clamped to [0.35, 0.85] in script.c

    -- Grid tiles. The column count is NOT fixed: it is derived from the
    -- available width and this target width, so the grid reflows when the
    -- window resizes instead of clipping.
    tile_width       = 196,
    tile_gap         = 18,
    tile_aspect      = 1.34,  -- cover height / width; 1.34 is 3:4 box art

    font_size        = 20,
    font_size_title  = 34,
    font_size_small  = 15,

    -- Selection easing, in "units per second" of exponential approach.
    -- Higher is snappier; 0 disables the animation entirely.
    anim_speed       = 16.0,
}
