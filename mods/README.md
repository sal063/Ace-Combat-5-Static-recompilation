# Mods

Drop a mod in here as a directory. It is picked up the next time the game
starts — nothing to rebuild, nothing to repack.

```
mods/
  my_mod/
    mod.toml                          optional
    files/
      select/noise.gim                every copy of a file inside DATA.PAC
      BIN/DATA.PAC/0011/map/m09a/m09a.wad   one member's copy
      BIN/US/BGM.PAC                  a whole file on the disc
      my_mod/table.bin                a new file, for scripts to read
    script.lua                        Lua, run at startup
    lib/helpers.lua                   a module, for require("lib.helpers")
    params.txt                        the game's named values, changed by name
    widescreen.pnach                  PCSX2 patches (data patches work)
    symbols.txt                       names for functions
    module.dll                        native code, only with native = true
```

## What a mod can change

| Layer | How | Example |
| --- | --- | --- |
| Files | `files/` | textures, music, maps, text |
| Named values | `params.txt`, `ac5.param` | `.enemyjet.distSound = 1.5` |
| Memory | `.pnach`, `ac5.patch` | PCSX2's widescreen patch |
| Functions | `ac5.hook_before/after/replace`, `ac5.redirect` | new weapon behaviour |
| Every frame | `ac5.on_frame`, `ac5.on_scene`, timers | a HUD counter, a game mode |
| The controller | `ac5.on_input`, `ac5.pad` | remapping, macros |
| Between runs | `ac5.store`, `ac5.stored` | unlocks, statistics |

## The filesystem the game reads from

The game does not read the disc directly. Every read — the CD library, Namco's
file service, the music streams, and the loader that unpacks `DATA.PAC` — goes
through one filesystem, and mods are layers in it. Nothing is repacked: when a
mod changes a file inside `DATA.PAC`, the game's own loader fills that member
from the filesystem instead of decompressing it from the disc. This works the
same on a `.iso` and on an extracted disc directory.

A file under `files/` is named the way the filesystem names it:

- **A whole file on the disc**, by its path: `BIN/US/BGM.PAC`. The size may
  change.
- **A file inside `DATA.PAC`, every copy**, by its name from
  `config/pac_names.txt`: `select/noise.gim`. Many names repeat across members,
  and a plain name replaces every copy the disc has.
- **One copy**, by member: `BIN/DATA.PAC/0011/map/m09a/m09a.wad`, or by
  position where there is no name: `BIN/DATA.PAC/0958/#4`. This form can also
  add a file to a slot the member leaves empty.
- **Anything else** is a new file, which scripts can read. The log says so,
  because it is usually a typo.

Names are matched without regard to case. The name table comes from a Japanese
build's index and is not right everywhere, so a plain name skips places the
disc leaves empty, and the log warns when the copies it replaces differ in
size. When a warning names a member you did not mean, give that member its own
file.

```
python -m modkit list    --disc DIR --names datapack.bin
python -m modkit extract --disc DIR --names datapack.bin --out DIR --member 11
```

`extract` writes the paths the filesystem uses, so a file can go straight into
a mod. `--ref-tree` cuts each file to its true length; without it a file keeps
up to 15 bytes of padding.

## Named values: params.txt

Much of the game's tuning is looked up by name at run time (the game's
`CTxtio` lists). An override applies to whatever the game asks for under that
name, converted to the type it asks for:

```
.enemyjet.distSound = 1.5
.enemyjet.enable    = 0
some_string_value   = "text"
```

To find the names, run with `PS2_PARAM_TRACE=1`. The log lists every distinct
name the game looks up, with its type and value:

```
param: model_name (string) = "Tcam__rel__cameraMain"
param: motion_no (u16) = 1
```

## Memory patches: .pnach

PCSX2 `.pnach` files work as they do in PCSX2 for patches that change data:
`patch=place,EE,address,type,value`, where place 0 means once at start, 1 means
every field and 2 means both. Two kinds are refused, each with a message:
- **Patches into the game's code.** The code was recompiled, so they would
  change nothing; use a hook instead.
- **Conditional extended codes.** The rest of that section is skipped, because
  the lines after a conditional depend on it.

Two mods patching the same bytes: the higher priority keeps them.

From a script, `ac5.patch(addr, value, width, original, when)` does the same.
With `original`, the patch is only written over that value, which protects
against patching memory that holds something else.

## Scripts

Every `.lua` file in a mod's directory runs at startup, in name order. Put
modules for `require` in a subdirectory. A script sees one table, `ac5`, which
is `runtime/include/ac5mod.h` call for call:

```lua
-- functions: by address, or by name (config/game_symbols.txt, symbols.txt)
ac5.hook_before(0x0011DB30, function(ctx) count = count + 1 end)
ac5.hook_after("CTxtio::_GetFloat", function(ctx)
    ac5.set_return_float(ctx, ac5.return_float(ctx) * 2)
end)
local h = ac5.hook_replace(0x00123456, function(ctx)
    if not wanted then return false end    -- the game's own function runs
    ac5.set_return(ctx, 1)
end)
ac5.unhook(h)

-- the game's loop: a safe place to call the game's own functions
ac5.on_frame(function(ctx) ac5.call_guest(ctx, "sceGsGetGParam") end)
ac5.on_scene(function(ctx, machine, major, minor, from_major, from_minor) end)
local t = ac5.every(600, function() ac5.log("ten seconds") end)
ac5.after(60, function() ac5.cancel(t) end)

-- the controller, before the game sees it
ac5.on_input(function(pads)
    if pads[1].buttons & ac5.BTN.l3 ~= 0 then
        pads[1].buttons = pads[1].buttons | ac5.BTN.cross
    end
end)

ac5.param(".enemyjet.distSound", 1.5)
ac5.store("best_score", 12345)          -- saves/mods/<id>.txt
local helpers = require("lib.helpers")
```

Hooks and callbacks return a handle. `unhook` detaches hooks; `cancel` stops
callbacks and timers.

Each mod has its own globals. `io`, `os`, `dofile`, `loadfile` and
`collectgarbage` are not there, and `load` takes source text only. A callback
that raises an error is reported for its first three failures and switched off
after ten; a switched-off replacement lets the game's own function run. The
`ctx` a callback is given is valid only while that callback runs.

A function with an after-hook is called rather than jumped to. If a guest loop
runs through it by tail jumps more than 256 calls deep, the deeper calls skip
their after-hooks rather than growing the stack; the run summary counts it.

## Native modules

A DLL exporting `int ac5_mod_main(const ac5_api *api)` loads only when the
mod's manifest says `native = true`. It is ordinary code in the game's process,
so only enable it for mods you trust. Check `api->abi_version >=
AC5_ABI_VERSION`: fields are only ever added. See
`tests/mods/native_test/native_test.c`.

## mod.toml

Every field is optional.

```toml
name     = "Custom Music"
version  = "1.0"
author   = "someone"
priority = 10            # higher wins every conflict; default 0
enabled  = false         # switch it off without deleting it
native   = true          # allow the DLLs in this directory to load
requires = "base_pack >= 1.2, helpers"
abi      = 3             # the mod ABI it needs, at least
game     = "SLUS-20851"

[config]
volume   = "0.8"         # read back with ac5.config("volume")
```

A mod whose requirements are not met is switched off, and the log and the run
summary say why. That covers a missing or switched-off mod, a version outside
the range, a newer ABI, a different game, or a requirement cycle. Mods load
after the mods they require.

## When two mods want the same thing

Priority decides everywhere: the same file, hooked function, indirect-call
target, memory bytes or named value. For files at equal priority, a
member-scoped path beats a plain name, and after that the mod whose directory
name sorts later wins. Every conflict is in the log with both sides named.

## What it costs

A file changed inside `DATA.PAC` changes how much memory the game allocates for
that member, and the machine has 32 MB. The run summary says what each mod
added. Whole-file replacements stream from your file and cost no memory. A
changed member is ready sooner than one the game decompresses, so a load that
includes one can finish a little early.

## When something goes wrong

- `PS2_NO_MODS=1` turns the whole layer off. Always the first thing to try.
- `PS2_MOD_DIR=path` loads from somewhere else.
- The run summary lists every mod, what it supplied, why any were switched
  off, every conflict, hook, patch and parameter override, and every script
  error.
