-- install.lua -- build recipes.
--
-- When a directory is imported, src/install.c walks this list in order and
-- takes the FIRST recipe whose `detect` list names a file that exists at
-- the top of the imported tree.  Its `steps` are then run in the tree with
-- fork(2)+execvp(3) -- there is no shell, so each step is an argv array,
-- not a command string.  Output goes to <game>/install.log.
--
-- Order matters: a project carrying both CMakeLists.txt and a convenience
-- Makefile should be built the way its author intended, so the more
-- specific build systems come first.
--
-- To add your own, insert a table anywhere in this list. To disable
-- building entirely, return {}.

return {
    {
        name   = "autotools",
        detect = { "configure" },
        steps  = {
            { "./configure" },
            { "make" },
        },
    },
    {
        name   = "cmake",
        detect = { "CMakeLists.txt" },
        steps  = {
            { "cmake", "-S", ".", "-B", "build", "-DCMAKE_BUILD_TYPE=Release" },
            { "cmake", "--build", "build", "--parallel" },
        },
    },
    {
        name   = "meson",
        detect = { "meson.build" },
        steps  = {
            { "meson", "setup", "build" },
            { "ninja", "-C", "build" },
        },
    },
    {
        -- FreeBSD's make(1) is bmake and reads BSDmakefile first, then
        -- makefile, then Makefile. A GNUmakefile in the same tree means
        -- the author expects GNU make, so it is checked separately below.
        name   = "bsdmake",
        detect = { "BSDmakefile", "Makefile", "makefile" },
        steps  = {
            { "make" },
        },
    },
    {
        name   = "gnumake",
        detect = { "GNUmakefile" },
        steps  = {
            { "gmake" },
        },
    },
    {
        name   = "cargo",
        detect = { "Cargo.toml" },
        steps  = {
            { "cargo", "build", "--release" },
        },
    },
    {
        name   = "go",
        detect = { "go.mod" },
        steps  = {
            { "go", "build", "./..." },
        },
    },
}
