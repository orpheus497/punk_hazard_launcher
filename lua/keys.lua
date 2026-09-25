-- keys.lua -- bindings.
--
-- The left-hand side is the name SDL gives the input, verbatim:
--
--   keyboard  SDL_GetKeyName().  Letter keys are reported in UPPER CASE
--             ("Unaccented letter keys on latin keyboards are normally
--             labeled in upper case" -- SDL_keyboard.c), and the named keys
--             are spelled exactly "Up", "Down", "Return", "Escape",
--             "PageUp", "PageDown", "Home", "End", "Tab", "Backspace",
--             "Space", "Delete", "F1".."F12".
--
--   pad       SDL_GameControllerGetStringForButton(): "a", "b", "x", "y",
--             "back", "guide", "start", "leftstick", "rightstick",
--             "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft",
--             "dpright".
--
-- The right-hand side is one of the semantic actions in enum ph_action:
--   up down left right page_up page_down home end
--   launch back quit fullscreen search details favorite sort crt theme
--   reload help
--
-- Binding several keys to one action is normal and expected; the table is
-- a plain lookup, so there is no cost to it.

return {
    keyboard = {
        ["Up"]        = "up",
        ["Down"]      = "down",
        ["Left"]      = "left",
        ["Right"]     = "right",

        -- vi keys, because this is a BSD program
        ["K"]         = "up",
        ["J"]         = "down",
        ["H"]         = "left",
        ["L"]         = "right",

        ["PageUp"]    = "page_up",
        ["PageDown"]  = "page_down",
        ["Home"]      = "home",
        ["End"]       = "end",

        ["Return"]    = "launch",
        ["Space"]     = "launch",
        ["Escape"]    = "back",
        ["Backspace"] = "back",
        ["Q"]         = "quit",

        ["F11"]       = "fullscreen",
        ["F"]         = "fullscreen",
        ["/"]         = "search",
        ["I"]         = "details",
        ["Tab"]       = "details",
        ["*"]         = "favorite",
        ["B"]         = "favorite",
        ["S"]         = "sort",
        ["C"]         = "crt",
        ["T"]         = "theme",
        ["R"]         = "reload",
        ["F1"]        = "help",
        ["?"]         = "help",
    },

    pad = {
        ["dpup"]          = "up",
        ["dpdown"]        = "down",
        ["dpleft"]        = "left",
        ["dpright"]       = "right",
        ["a"]             = "launch",
        ["b"]             = "back",
        ["x"]             = "details",
        ["y"]             = "favorite",
        ["leftshoulder"]  = "page_up",
        ["rightshoulder"] = "page_down",
        ["start"]         = "launch",
        ["back"]          = "sort",
        ["guide"]         = "quit",
    },
}
