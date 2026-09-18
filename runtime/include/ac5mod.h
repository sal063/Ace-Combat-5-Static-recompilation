/* ac5mod.h -- what a mod is written against.
 *
 * This is the whole surface.  A mod reaches the game through the calls below
 * in one of two ways:
 *
 *   Lua      a .lua file in the mod's directory.  The `ac5` table it sees is
 *            this header, call for call (runtime/src/ps2_lua.c).
 *   native   a DLL in the mod's directory exporting
 *                int ac5_mod_main(const ac5_api *api);
 *            loaded only when the mod's manifest says `native = true`.  Native
 *            code is arbitrary code in the game's process; the log says which
 *            mods ran some.
 *
 * THE MACHINE UNDERNEATH.  The game is not emulated here: its functions were
 * translated to C ahead of time and are ordinary native code in this process.
 * What a mod manipulates is the guest's *state* -- its 32 MB of memory and its
 * register file -- through a context pointer that every hook receives.
 *
 * ADDRESSES ARE STABLE.  A guest address means the same thing in every build:
 * the executable it came from is pinned by SHA-256, so 0x0011DB30 is the same
 * function next year as today.  config/game_symbols.txt names the functions
 * the game's own debug messages identify, and symbol() looks names up.
 *
 * WHICH THREAD.  Everything here except log() must happen on a thread that
 * holds the EE: in ac5_mod_main, a hook, or any callback below -- which is
 * everywhere a mod's code is called from.  A call from any other thread is
 * refused and counted.
 *
 * PRIORITY.  A mod's manifest priority settles every conflict: two mods
 * replacing one file, one function, one indirect-call target, one memory
 * patch, one named parameter -- the higher priority wins, both are logged.
 *
 * COMPATIBILITY.  Fields are only ever appended.  A module built against this
 * header runs on any runtime whose abi_version is at least AC5_ABI_VERSION, so
 * check `api->abi_version >= AC5_ABI_VERSION`, not equality.
 *
 * HANDLES.  Everything that attaches something returns a handle > 0, or -1
 * with the reason in the log: hooks go back through unhook(), callbacks and
 * timers through cancel(), patches through unpatch().
 */
#ifndef AC5MOD_H
#define AC5MOD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AC5_ABI_VERSION 3

typedef struct ps2_ctx ac5_ctx;         /* the guest's registers, opaque here */

typedef struct ac5_api ac5_api;

/* A native mod exports this.  It runs once, after the filesystem is open and
 * before the guest starts.  Return 0 to say it loaded; anything else is logged
 * as a failure (whatever it attached first stays attached). */
typedef int (*ac5_mod_main_fn)(const ac5_api *api);

/* ---- the controller ------------------------------------------------------ */
/* Bits of ac5_pad.buttons, set while held -- libpad2's button order. */
#define AC5_BTN_SELECT   (1u << 0)
#define AC5_BTN_L3       (1u << 1)
#define AC5_BTN_R3       (1u << 2)
#define AC5_BTN_START    (1u << 3)
#define AC5_BTN_UP       (1u << 4)
#define AC5_BTN_RIGHT    (1u << 5)
#define AC5_BTN_DOWN     (1u << 6)
#define AC5_BTN_LEFT     (1u << 7)
#define AC5_BTN_L2       (1u << 8)
#define AC5_BTN_R2       (1u << 9)
#define AC5_BTN_L1       (1u << 10)
#define AC5_BTN_R1       (1u << 11)
#define AC5_BTN_TRIANGLE (1u << 12)
#define AC5_BTN_CIRCLE   (1u << 13)
#define AC5_BTN_CROSS    (1u << 14)
#define AC5_BTN_SQUARE   (1u << 15)

#define AC5_PAD_PORTS 2

typedef struct ac5_pad {
    uint8_t  connected;
    uint16_t buttons;                   /* AC5_BTN_* */
    uint8_t  lx, ly, rx, ry;            /* 0..255, 127 at rest */
    uint8_t  l2, r2;                    /* trigger pressure, 0..255 */
} ac5_pad;

/* ---- callbacks a mod can register -------------------------------------- */

/* A hook.  For hook_replace, return non-zero when you handled the call and 0
 * to let the game's own function run after all.  Otherwise ignored. */
typedef int  (*ac5_hook_fn)(ac5_ctx *ctx, void *user);
typedef void (*ac5_field_fn)(void *user);
/* on_frame: the main thread's context, so call_guest works from here. */
typedef void (*ac5_frame_fn)(ac5_ctx *ctx, void *user);
/* on_scene: `machine` 0 is the top-level scene machine, 1 the screen's own. */
typedef void (*ac5_scene_fn)(ac5_ctx *ctx, int machine, int major, int minor,
                             int from_major, int from_minor, void *user);
/* on_input: both ports, as the game is about to see them; change them freely. */
typedef void (*ac5_input_fn)(ac5_pad *pads, int ports, void *user);

enum { AC5_PATCH_ONCE = 0, AC5_PATCH_ALWAYS = 1, AC5_PATCH_BOTH = 2 };

struct ac5_api {
    uint32_t abi_version;               /* AC5_ABI_VERSION                   */
    const char *game_id;                /* "SLUS-20851"                      */
    const char *mod_id;                 /* this mod's directory name         */
    int32_t priority;                   /* from its mod.toml                 */

    /* -- log -------------------------------------------------------------
     * Goes to the same log as everything else, prefixed with the mod's id.
     * The one call that is safe from any thread. */
    void (*log)(const char *fmt, ...);

    /* -- guest memory ----------------------------------------------------
     * Addresses are the game's own.  Only memory -- RAM, the scratchpad, VU
     * memory -- is reachable: an address with a hardware register behind it
     * reads 0 and ignores writes. */
    uint8_t  (*read8)(uint32_t addr);
    uint16_t (*read16)(uint32_t addr);
    uint32_t (*read32)(uint32_t addr);
    float    (*read_float)(uint32_t addr);
    void     (*write8)(uint32_t addr, uint8_t v);
    void     (*write16)(uint32_t addr, uint16_t v);
    void     (*write32)(uint32_t addr, uint32_t v);
    void     (*write_float)(uint32_t addr, float v);
    void     (*read_bytes)(void *dst, uint32_t addr, uint32_t n);
    void     (*write_bytes)(uint32_t addr, const void *src, uint32_t n);

    /* -- the guest's registers, inside a hook or frame callback ---------
     * arg(n): the n'th argument as the call passes it -- eight in registers,
     * the rest on the stack.  set_return: $v0 as the function will appear to
     * have returned it.  reg / set_reg: r0..r31. */
    uint32_t (*arg)(ac5_ctx *ctx, int n);
    void     (*set_return)(ac5_ctx *ctx, uint32_t value);
    uint32_t (*reg)(ac5_ctx *ctx, int n);
    void     (*set_reg)(ac5_ctx *ctx, int n, uint32_t v);

    /* -- hooks -----------------------------------------------------------
     * hook_before(): run fn ahead of the function; higher priority first.
     * hook_replace(): run fn instead; one per function, settled by priority.
     * call_original(): run the real function from inside a replacement.
     * redirect(): send indirect calls for one guest address to another. */
    int  (*hook_before)(uint32_t guest_addr, ac5_hook_fn fn, void *user,
                        int priority);
    int  (*hook_replace)(uint32_t guest_addr, ac5_hook_fn fn, void *user);
    void (*call_original)(uint32_t guest_addr, ac5_ctx *ctx);
    int  (*redirect)(uint32_t from_guest, uint32_t to_guest);

    /* -- calling into the game -------------------------------------------
     * Runs a guest function with up to eight arguments and returns $v0.  Pass
     * the context a hook or on_frame was given: a guest function runs on a
     * guest stack, and the stack pointer lives in the context. */
    uint32_t (*call_guest)(ac5_ctx *ctx, uint32_t guest_addr,
                           const uint32_t *args, int nargs);

    /* -- per field --------------------------------------------------------
     * Once per emulated field (59.94 Hz), inside the vertical-blank interrupt:
     * guest memory is safe to touch, but no guest thread is at a safe point,
     * so there is no context to call guest code with -- use on_frame. */
    int  (*on_field)(ac5_field_fn fn, void *user);

    /* -- names ------------------------------------------------------------
     * The address of a named function, or 0.  Names come from the recompiler,
     * config/game_symbols.txt and every enabled mod's symbols.txt. */
    uint32_t (*symbol)(const char *name);
    const char *(*symbol_name)(uint32_t guest_addr);

    /* -- configuration ----------------------------------------------------
     * A value from the mod's own mod.toml -- a top-level key the host does
     * not use itself, or any key under [config] -- or NULL. */
    const char *(*config)(const char *key);

    /* -- files ------------------------------------------------------------
     * The filesystem the game reads from (runtime/include/ps2_vfs.h): disc
     * paths, archive files by name or member, and new files mods added.
     * -1 when there is no such file. */
    int64_t (*file_size)(const char *path);
    int64_t (*file_read)(const char *path, uint64_t pos, void *dst,
                         uint32_t len);
    int64_t (*file_to_guest)(const char *path, uint64_t pos,
                             uint32_t guest_addr, uint32_t len);

    /* ==== ABI 3 ========================================================= */

    /* -- after-hooks and return values -----------------------------------
     * hook_after(): run fn once the function has returned.  entry_arg() reads
     * the arguments as the function received them (it may have overwritten
     * the registers since); return_value() and set_return() see and change
     * $v0, return_float() / set_return_float() the float result in $f0.
     * Functions with an after-hook are called rather than jumped to; see
     * runtime/include/ps2_hook.h for what that means for deep recursion. */
    int      (*hook_after)(uint32_t guest_addr, ac5_hook_fn fn, void *user,
                           int priority);
    int      (*unhook)(int handle);
    uint32_t (*entry_arg)(ac5_ctx *ctx, int n);
    uint32_t (*return_value)(ac5_ctx *ctx);
    float    (*return_float)(ac5_ctx *ctx);
    void     (*set_return_float)(ac5_ctx *ctx, float v);
    float    (*reg_float)(ac5_ctx *ctx, int n);             /* $f0..$f31 */
    void     (*set_reg_float)(ac5_ctx *ctx, int n, float v);

    /* -- game events -----------------------------------------------------
     * on_frame(): once per game frame, on the main thread, as the main loop
     *   dispatches its scene machine -- a safe point to call game functions.
     * on_scene(): whenever a scene machine changes state, before the new
     *   state's handler runs.
     * timer(): fn after `fields` fields; again every `fields` if `repeat`.
     * on_shutdown(): once, when the run ends.
     * cancel(): stop any of these, or an on_field / on_input callback. */
    int  (*on_frame)(ac5_frame_fn fn, void *user);
    int  (*on_scene)(ac5_scene_fn fn, void *user);
    int  (*timer)(uint32_t fields, int repeat, ac5_field_fn fn, void *user);
    int  (*on_shutdown)(ac5_field_fn fn, void *user);
    int  (*cancel)(int handle);

    /* -- the controller --------------------------------------------------
     * pad(): the state the game sees this field.  on_input(): called every
     * field after the pad is read and before the game sees it; whatever the
     * callback leaves in `pads` is what the game gets.  Callbacks run in
     * registration order, each seeing the last one's result. */
    int  (*pad)(int port, ac5_pad *out);
    int  (*on_input)(ac5_input_fn fn, void *user);

    /* -- memory patches (runtime/include/ps2_patch.h) --------------------
     * patch(): write `len` bytes at `addr`, once, every field, or both.  With
     * `original` (NULL, or `len` bytes) it only writes over that value.
     * Patches into the game's code are refused -- it was recompiled, so they
     * would do nothing.  A mod can also ship PCSX2 .pnach files in its
     * directory; the host applies them. */
    int  (*patch)(uint32_t addr, const void *bytes, const void *original,
                  uint32_t len, int when);
    int  (*unpatch)(int handle);

    /* -- the game's named parameters (runtime/include/ps2_params.h) -----
     * param_set(): override a CTxtio value by name (".enemyjet.distSound"),
     * converted to whatever type the game asks for it as; NULL clears. */
    int  (*param_set)(const char *name, const char *value);

    /* -- storage -----------------------------------------------------------
     * Strings kept between runs, per mod, in saves/mods/<id>.txt.  NULL value
     * deletes.  store_get returns NULL for a key that is not there; the
     * pointer is valid until the next store_set of that key. */
    const char *(*store_get)(const char *key);
    int  (*store_set)(const char *key, const char *value);
};

#ifdef __cplusplus
}
#endif

#endif /* AC5MOD_H */
