#include "ps2_runtime.h"
#include "ps2_modapi.h"
#include "ps2_mod.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define LUA_MAX_DEPTH      16
#define CB_ERRORS_SHOWN    3
#define CB_ERRORS_DISABLE  10
#define FILE_READ_MAX      (64u * 1024u * 1024u)

typedef struct {
    const ac5_api *api;
    int env;
    unsigned scripts, script_errors, errors, disabled;
    char dir[640];
} lua_mod;

typedef struct {
    lua_mod *mod;
    int ref;
    char what[40];
    u64 calls;
    unsigned errors;
    int dead;
    int handle;
    int is_hook;
    int once;
    unsigned running;
    int release;
} lua_cb;

static lua_State *L;
static lua_mod **lmods;
static unsigned nlmods;
static lua_cb **cbs;
static unsigned ncbs, ccbs;
static u64 refused_thread, refused_switch, refused_depth, stack_drift;

static pthread_t lua_owner;
static unsigned lua_depth;

static int lua_enter(const char *what) {
    if (!ps2_kernel_on_ee_thread()) {
        if (!refused_thread++)
            ps2_log("lua: a %s arrived on a thread that does not hold the EE; "
                    "declined (counted in the run summary)", what);
        return 0;
    }
    if (lua_depth) {
        if (!pthread_equal(lua_owner, pthread_self())) {
            if (!refused_switch++)
                ps2_log("lua: a %s arrived on another guest thread while a "
                        "Lua call was still open on the first; declined "
                        "(counted in the run summary)", what);
            return 0;
        }
        if (lua_depth >= LUA_MAX_DEPTH) {
            if (!refused_depth++)
                ps2_log("lua: a %s would nest Lua %u calls deep; declined",
                        what, LUA_MAX_DEPTH);
            return 0;
        }
    } else {
        lua_owner = pthread_self();
    }
    lua_depth++;
    return 1;
}

static void lua_leave(void) { lua_depth--; }

static ps2_ctx **ctx_slot[LUA_MAX_DEPTH];
static int ctx_ref[LUA_MAX_DEPTH];

static void push_ctx(unsigned depth, ps2_ctx *ctx) {
    if (!ctx_slot[depth]) {
        ctx_slot[depth] = (ps2_ctx **)lua_newuserdatauv(L, sizeof(ps2_ctx *), 0);
        luaL_setmetatable(L, "ac5.ctx");
        ctx_ref[depth] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    *ctx_slot[depth] = ctx;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ctx_ref[depth]);
}

static ac5_ctx *ctx_arg(lua_State *s, int idx) {
    ps2_ctx **p = (ps2_ctx **)luaL_checkudata(s, idx, "ac5.ctx");
    if (!*p)
        luaL_error(s, "this context belongs to a callback that has already "
                      "returned; use the one your callback was given");
    return (ac5_ctx *)*p;
}

static int lua_msgh(lua_State *s) {
    const char *tn = luaL_typename(s, 1);
    const char *msg = luaL_tolstring(s, 1, NULL);
    lua_pushfstring(s, "(%s) %s", tn, msg ? msg : "?");
    luaL_traceback(s, s, lua_tostring(s, -1), 1);
    return 1;
}

static void log_error_lines(const char *msg) {
    char line[900];
    const char *p = msg ? msg : "(no message)";
    while (*p) {
        size_t n = strcspn(p, "\n");
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), p);
        ps2_log("    %s", line);
        p += n;
        if (*p) p++;
    }
}

static void cb_retire(lua_cb *cb) {
    if (cb->release) return;
    cb->release = 1;
    luaL_unref(L, LUA_REGISTRYINDEX, cb->ref);
    cb->ref = LUA_NOREF;
}

static void cb_free(lua_cb *cb) {
    luaL_unref(L, LUA_REGISTRYINDEX, cb->ref);
    for (unsigned i = 0; i < ncbs; i++)
        if (cbs[i] == cb) { cbs[i] = cbs[--ncbs]; break; }
    free(cb);
}

static int call_cb(lua_cb *cb, ps2_ctx *ctx, int (*push)(void *), void *extra) {
    unsigned depth;
    int top, base, rc, handled = 0, nargs;
    const ac5_api *prev;
    if (!L || cb->dead || cb->release) return 0;
    if (!lua_enter(cb->what)) return 0;
    depth = lua_depth - 1u;
    top = lua_gettop(L);
    lua_pushcfunction(L, lua_msgh);
    base = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);
    nargs = 0;
    if (ctx) { push_ctx(depth, ctx); nargs++; }
    if (push) nargs += push(extra);
    prev = ps2_modapi_enter(cb->mod->api);
    cb->calls++;
    cb->running++;
    rc = lua_pcall(L, nargs, 1, base);
    cb->running--;
    ps2_modapi_leave(prev);
    if (ctx) *ctx_slot[depth] = NULL;
    if (rc == LUA_OK) {
        handled = !(lua_isboolean(L, -1) && !lua_toboolean(L, -1));
    } else {
        cb->errors++;
        cb->mod->errors++;
        if (cb->errors <= CB_ERRORS_SHOWN) {
            ps2_log("lua[%s]: %s failed (error %u):", cb->mod->api->mod_id,
                    cb->what, cb->errors);
            log_error_lines(lua_tostring(L, -1));
        }
        if (cb->errors == CB_ERRORS_DISABLE) {
            cb->dead = 1;
            cb->mod->disabled++;
            ps2_log("lua[%s]: %s switched off after %u errors%s",
                    cb->mod->api->mod_id, cb->what, cb->errors,
                    strncmp(cb->what, "replacement", 11) ? ""
                        : "; the game's own function runs in its place");
        }
    }
    if (lua_gettop(L) != base + 1) {
        if (!stack_drift++)
            ps2_log("lua: %s left the stack unbalanced (%d over); cleared",
                    cb->what, lua_gettop(L) - base - 1);
    }
    lua_settop(L, top);
    lua_leave();
    if (cb->once) cb_retire(cb);
    return handled;
}

static int hook_call(ac5_ctx *ctx, void *user) {
    return call_cb((lua_cb *)user, (ps2_ctx *)ctx, NULL, NULL);
}

static void field_call(void *user) {
    call_cb((lua_cb *)user, NULL, NULL, NULL);
}

static void frame_call(ac5_ctx *ctx, void *user) {
    call_cb((lua_cb *)user, (ps2_ctx *)ctx, NULL, NULL);
}

typedef struct { int v[5]; } scene_args;

static int push_scene(void *extra) {
    scene_args *a = (scene_args *)extra;
    for (int i = 0; i < 5; i++) lua_pushinteger(L, a->v[i]);
    return 5;
}

static void scene_call(ac5_ctx *ctx, int machine, int major, int minor,
                       int from_major, int from_minor, void *user) {
    scene_args a = {{ machine, major, minor, from_major, from_minor }};
    call_cb((lua_cb *)user, (ps2_ctx *)ctx, push_scene, &a);
}

static const char *const pad_fields[] = {
    "connected", "buttons", "lx", "ly", "rx", "ry", "l2", "r2", NULL };

static void pad_to_table(lua_State *s, const ac5_pad *p) {
    lua_createtable(s, 0, 8);
    lua_pushboolean(s, p->connected);   lua_setfield(s, -2, "connected");
    lua_pushinteger(s, p->buttons);     lua_setfield(s, -2, "buttons");
    lua_pushinteger(s, p->lx);          lua_setfield(s, -2, "lx");
    lua_pushinteger(s, p->ly);          lua_setfield(s, -2, "ly");
    lua_pushinteger(s, p->rx);          lua_setfield(s, -2, "rx");
    lua_pushinteger(s, p->ry);          lua_setfield(s, -2, "ry");
    lua_pushinteger(s, p->l2);          lua_setfield(s, -2, "l2");
    lua_pushinteger(s, p->r2);          lua_setfield(s, -2, "r2");
}

static int clamp8(lua_Integer v) { return v < 0 ? 0 : v > 255 ? 255 : (int)v; }

static void pad_from_table(lua_State *s, int idx, ac5_pad *p) {
    for (int f = 0; pad_fields[f]; f++) {
        lua_getfield(s, idx, pad_fields[f]);
        if (f == 0) {
            if (!lua_isnil(s, -1)) p->connected = (uint8_t)lua_toboolean(s, -1);
        } else if (lua_isinteger(s, -1)) {
            lua_Integer v = lua_tointeger(s, -1);
            switch (f) {
            case 1: p->buttons = (uint16_t)v; break;
            case 2: p->lx = (uint8_t)clamp8(v); break;
            case 3: p->ly = (uint8_t)clamp8(v); break;
            case 4: p->rx = (uint8_t)clamp8(v); break;
            case 5: p->ry = (uint8_t)clamp8(v); break;
            case 6: p->l2 = (uint8_t)clamp8(v); break;
            case 7: p->r2 = (uint8_t)clamp8(v); break;
            }
        }
        lua_pop(s, 1);
    }
}

typedef struct { ac5_pad *pads; int ports; int table; } input_args;

static int push_input(void *extra) {
    input_args *a = (input_args *)extra;
    lua_createtable(L, a->ports, 0);
    for (int i = 0; i < a->ports; i++) {
        pad_to_table(L, &a->pads[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_pushvalue(L, -1);
    a->table = luaL_ref(L, LUA_REGISTRYINDEX);
    return 1;
}

static void input_call(ac5_pad *pads, int ports, void *user) {
    input_args a = { pads, ports, LUA_NOREF };
    call_cb((lua_cb *)user, NULL, push_input, &a);
    if (a.table == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, a.table);
    for (int i = 0; i < ports; i++) {
        if (lua_rawgeti(L, -1, i + 1) == LUA_TTABLE) pad_from_table(L, -1, &pads[i]);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    luaL_unref(L, LUA_REGISTRYINDEX, a.table);
}

static lua_cb *cb_new(lua_State *s, lua_mod *M, int idx, const char *what) {
    lua_cb *cb = (lua_cb *)calloc(1, sizeof *cb);
    if (!cb) luaL_error(s, "out of memory");
    lua_pushvalue(s, idx);
    cb->ref = luaL_ref(s, LUA_REGISTRYINDEX);
    cb->mod = M;
    snprintf(cb->what, sizeof cb->what, "%s", what);
    if (ncbs == ccbs) {
        lua_cb **grown;
        ccbs = ccbs ? ccbs * 2u : 32u;
        grown = (lua_cb **)realloc(cbs, ccbs * sizeof *cbs);
        if (!grown) ps2_fatal("lua: out of memory registering a callback");
        cbs = grown;
    }
    cbs[ncbs++] = cb;
    return cb;
}

static void cb_drop(lua_State *s, lua_cb *cb) {
    (void)s;
    cb_free(cb);
}

static int push_handle(lua_State *s, lua_cb *cb, int handle, int is_hook) {
    if (handle < 0) {
        cb_drop(s, cb);
        lua_pushboolean(s, 0);
        return 1;
    }
    cb->handle = handle;
    cb->is_hook = is_hook;
    lua_pushinteger(s, handle);
    return 1;
}

#define MOD(s) ((lua_mod *)lua_touserdata((s), lua_upvalueindex(1)))

static u32 checku32(lua_State *s, int idx) {
    return (u32)(lua_Unsigned)luaL_checkinteger(s, idx);
}

static int l_log(lua_State *s) {
    luaL_Buffer b;
    int n = lua_gettop(s);
    luaL_buffinit(s, &b);
    for (int i = 1; i <= n; i++) {
        if (i > 1) luaL_addchar(&b, ' ');
        luaL_tolstring(s, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    MOD(s)->api->log("%s", lua_tostring(s, -1));
    return 0;
}

static int l_read8(lua_State *s)  { lua_pushinteger(s, MOD(s)->api->read8(checku32(s, 1)));  return 1; }
static int l_read16(lua_State *s) { lua_pushinteger(s, MOD(s)->api->read16(checku32(s, 1))); return 1; }
static int l_read32(lua_State *s) { lua_pushinteger(s, MOD(s)->api->read32(checku32(s, 1))); return 1; }
static int l_read_float(lua_State *s) { lua_pushnumber(s, MOD(s)->api->read_float(checku32(s, 1))); return 1; }
static int l_write8(lua_State *s)  { MOD(s)->api->write8(checku32(s, 1), (u8)checku32(s, 2));   return 0; }
static int l_write16(lua_State *s) { MOD(s)->api->write16(checku32(s, 1), (u16)checku32(s, 2)); return 0; }
static int l_write32(lua_State *s) { MOD(s)->api->write32(checku32(s, 1), checku32(s, 2));      return 0; }
static int l_write_float(lua_State *s) {
    MOD(s)->api->write_float(checku32(s, 1), (float)luaL_checknumber(s, 2));
    return 0;
}

static int l_read_bytes(lua_State *s) {
    u32 addr = checku32(s, 1);
    lua_Integer n = luaL_checkinteger(s, 2);
    luaL_Buffer b;
    luaL_argcheck(s, n >= 0 && n <= (lua_Integer)PS2_RAM_SIZE, 2, "length out of range");
    MOD(s)->api->read_bytes(luaL_buffinitsize(s, &b, (size_t)n), addr, (u32)n);
    luaL_pushresultsize(&b, (size_t)n);
    return 1;
}

static int l_write_bytes(lua_State *s) {
    size_t n;
    u32 addr = checku32(s, 1);
    const char *data = luaL_checklstring(s, 2, &n);
    luaL_argcheck(s, n <= PS2_RAM_SIZE, 2, "too long");
    MOD(s)->api->write_bytes(addr, data, (u32)n);
    return 0;
}

static int l_arg(lua_State *s) {
    ac5_ctx *c = ctx_arg(s, 1);
    lua_pushinteger(s, MOD(s)->api->arg(c, (int)luaL_checkinteger(s, 2)));
    return 1;
}
static int l_set_return(lua_State *s) {
    ac5_ctx *c = ctx_arg(s, 1);
    MOD(s)->api->set_return(c, checku32(s, 2));
    return 0;
}
static int l_reg(lua_State *s) {
    ac5_ctx *c = ctx_arg(s, 1);
    lua_pushinteger(s, MOD(s)->api->reg(c, (int)luaL_checkinteger(s, 2)));
    return 1;
}
static int l_set_reg(lua_State *s) {
    ac5_ctx *c = ctx_arg(s, 1);
    MOD(s)->api->set_reg(c, (int)luaL_checkinteger(s, 2), checku32(s, 3));
    return 0;
}

static u32 check_addr(lua_State *s, int idx) {
    if (lua_type(s, idx) == LUA_TSTRING) {
        const char *name = lua_tostring(s, idx);
        u32 addr = MOD(s)->api->symbol(name);
        if (!addr) luaL_error(s, "no function is named '%s'", name);
        return addr;
    }
    return checku32(s, idx);
}

static int hook_generic(lua_State *s, const char *kind) {
    lua_mod *M = MOD(s);
    u32 addr = check_addr(s, 1);
    int prio = (int)luaL_optinteger(s, 3, M->api->priority), handle;
    char what[40];
    lua_cb *cb;
    luaL_checktype(s, 2, LUA_TFUNCTION);
    snprintf(what, sizeof what, "%s %08X", kind, addr);
    cb = cb_new(s, M, 2, what);
    if (kind[0] == 'h')
        handle = M->api->hook_before(addr, hook_call, cb, prio);
    else if (kind[0] == 'a')
        handle = M->api->hook_after(addr, hook_call, cb, prio);
    else
        handle = M->api->hook_replace(addr, hook_call, cb);
    return push_handle(s, cb, handle, 1);
}

static int l_hook_before(lua_State *s)  { return hook_generic(s, "hook on"); }
static int l_hook_after(lua_State *s)   { return hook_generic(s, "after-hook on"); }
static int l_hook_replace(lua_State *s) { return hook_generic(s, "replacement of"); }

static lua_cb *cb_by_handle(int handle, int is_hook) {
    for (unsigned i = 0; i < ncbs; i++)
        if (cbs[i]->handle == handle && cbs[i]->is_hook == is_hook
            && !cbs[i]->release)
            return cbs[i];
    return NULL;
}

static int l_unhook(lua_State *s) {
    int handle = (int)luaL_checkinteger(s, 1);
    lua_cb *cb = cb_by_handle(handle, 1);
    int ok = MOD(s)->api->unhook(handle) == 0;
    if (ok && cb) cb_retire(cb);
    lua_pushboolean(s, ok);
    return 1;
}

static int l_cancel(lua_State *s) {
    int handle = (int)luaL_checkinteger(s, 1);
    lua_cb *cb = cb_by_handle(handle, 0);
    int ok = MOD(s)->api->cancel(handle) == 0;
    if (ok && cb) cb_retire(cb);
    lua_pushboolean(s, ok);
    return 1;
}

static int l_call_original(lua_State *s) {
    u32 addr = check_addr(s, 1);
    MOD(s)->api->call_original(addr, ctx_arg(s, 2));
    return 0;
}

static int l_redirect(lua_State *s) {
    int handle = MOD(s)->api->redirect(check_addr(s, 1), check_addr(s, 2));
    if (handle > 0) lua_pushinteger(s, handle); else lua_pushboolean(s, 0);
    return 1;
}

static int l_entry_arg(lua_State *s) {
    ac5_ctx *c = ctx_arg(s, 1);
    lua_pushinteger(s, MOD(s)->api->entry_arg(c, (int)luaL_checkinteger(s, 2)));
    return 1;
}
static int l_return_value(lua_State *s) {
    lua_pushinteger(s, MOD(s)->api->return_value(ctx_arg(s, 1)));
    return 1;
}
static int l_return_float(lua_State *s) {
    lua_pushnumber(s, MOD(s)->api->return_float(ctx_arg(s, 1)));
    return 1;
}
static int l_set_return_float(lua_State *s) {
    ac5_ctx *c = ctx_arg(s, 1);
    MOD(s)->api->set_return_float(c, (float)luaL_checknumber(s, 2));
    return 0;
}
static int l_reg_float(lua_State *s) {
    ac5_ctx *c = ctx_arg(s, 1);
    lua_pushnumber(s, MOD(s)->api->reg_float(c, (int)luaL_checkinteger(s, 2)));
    return 1;
}
static int l_set_reg_float(lua_State *s) {
    ac5_ctx *c = ctx_arg(s, 1);
    MOD(s)->api->set_reg_float(c, (int)luaL_checkinteger(s, 2),
                               (float)luaL_checknumber(s, 3));
    return 0;
}

static int l_on_frame(lua_State *s) {
    lua_mod *M = MOD(s);
    lua_cb *cb;
    luaL_checktype(s, 1, LUA_TFUNCTION);
    cb = cb_new(s, M, 1, "frame callback");
    return push_handle(s, cb, M->api->on_frame(frame_call, cb), 0);
}

static int l_on_scene(lua_State *s) {
    lua_mod *M = MOD(s);
    lua_cb *cb;
    luaL_checktype(s, 1, LUA_TFUNCTION);
    cb = cb_new(s, M, 1, "scene callback");
    return push_handle(s, cb, M->api->on_scene(scene_call, cb), 0);
}

static int timer_generic(lua_State *s, int repeat) {
    lua_mod *M = MOD(s);
    lua_Integer fields = luaL_checkinteger(s, 1);
    lua_cb *cb;
    luaL_argcheck(s, fields >= 1 && fields <= 0x7FFFFFFF, 1, "fields out of range");
    luaL_checktype(s, 2, LUA_TFUNCTION);
    cb = cb_new(s, M, 2, repeat ? "repeating timer" : "timer");
    cb->once = !repeat;
    return push_handle(s, cb, M->api->timer((u32)fields, repeat, field_call, cb), 0);
}

static int l_after(lua_State *s) { return timer_generic(s, 0); }
static int l_every(lua_State *s) { return timer_generic(s, 1); }

static int l_on_shutdown(lua_State *s) {
    lua_mod *M = MOD(s);
    lua_cb *cb;
    luaL_checktype(s, 1, LUA_TFUNCTION);
    cb = cb_new(s, M, 1, "shutdown callback");
    return push_handle(s, cb, M->api->on_shutdown(field_call, cb), 0);
}

static int l_pad(lua_State *s) {
    ac5_pad p;
    int port = (int)luaL_optinteger(s, 1, 1);
    if (MOD(s)->api->pad(port - 1, &p) != 0) { lua_pushnil(s); return 1; }
    pad_to_table(s, &p);
    return 1;
}

static int l_on_input(lua_State *s) {
    lua_mod *M = MOD(s);
    lua_cb *cb;
    luaL_checktype(s, 1, LUA_TFUNCTION);
    cb = cb_new(s, M, 1, "input callback");
    return push_handle(s, cb, M->api->on_input(input_call, cb), 0);
}

static int l_patch(lua_State *s) {
    lua_mod *M = MOD(s);
    u32 addr = checku32(s, 1);
    u8 bytes[256], orig[256];
    size_t len;
    int has_orig = !lua_isnoneornil(s, 4), when = AC5_PATCH_ONCE, handle;
    const char *w = luaL_optstring(s, 5, "once");
    if (!strcmp(w, "always")) when = AC5_PATCH_ALWAYS;
    else if (!strcmp(w, "both")) when = AC5_PATCH_BOTH;
    else luaL_argcheck(s, !strcmp(w, "once"), 5, "\"once\", \"always\" or \"both\"");
    if (lua_type(s, 2) == LUA_TSTRING) {
        const char *b = lua_tolstring(s, 2, &len);
        luaL_argcheck(s, len >= 1 && len <= sizeof bytes, 2, "1 to 256 bytes");
        memcpy(bytes, b, len);
        if (has_orig) {
            size_t olen;
            const char *o = luaL_checklstring(s, 4, &olen);
            luaL_argcheck(s, olen == len, 4, "the original must be as long as the patch");
            memcpy(orig, o, len);
        }
    } else {
        lua_Integer width = luaL_checkinteger(s, 3);
        u32 v = checku32(s, 2);
        luaL_argcheck(s, width == 1 || width == 2 || width == 4, 3, "1, 2 or 4");
        len = (size_t)width;
        for (size_t i = 0; i < len; i++) bytes[i] = (u8)(v >> (8 * i));
        if (has_orig) {
            u32 o = checku32(s, 4);
            for (size_t i = 0; i < len; i++) orig[i] = (u8)(o >> (8 * i));
        }
    }
    handle = M->api->patch(addr, bytes, has_orig ? orig : NULL, (u32)len, when);
    if (handle > 0) lua_pushinteger(s, handle); else lua_pushboolean(s, 0);
    return 1;
}

static int l_unpatch(lua_State *s) {
    lua_pushboolean(s, MOD(s)->api->unpatch((int)luaL_checkinteger(s, 1)) == 0);
    return 1;
}

static int l_param(lua_State *s) {
    const char *name = luaL_checkstring(s, 1);
    const char *value = NULL;
    if (!lua_isnoneornil(s, 2)) {
        luaL_argcheck(s, lua_type(s, 2) == LUA_TNUMBER || lua_type(s, 2) == LUA_TSTRING,
                      2, "a number, a string or nil");
        value = lua_tostring(s, 2);
    }
    lua_pushboolean(s, MOD(s)->api->param_set(name, value) == 0);
    return 1;
}

static int l_store(lua_State *s) {
    const char *key = luaL_checkstring(s, 1);
    const char *value = lua_isnoneornil(s, 2) ? NULL : luaL_tolstring(s, 2, NULL);
    lua_pushboolean(s, MOD(s)->api->store_set(key, value) == 0);
    return 1;
}

static int l_stored(lua_State *s) {
    const char *v = MOD(s)->api->store_get(luaL_checkstring(s, 1));
    if (v) lua_pushstring(s, v); else lua_pushnil(s);
    return 1;
}

static u32 check_addr(lua_State *s, int idx);

static int l_call_guest(lua_State *s) {
    ac5_ctx *c = ctx_arg(s, 1);
    u32 addr = check_addr(s, 2);
    u32 args[8];
    int n = lua_gettop(s) - 2;
    luaL_argcheck(s, n <= 8, 11, "at most eight arguments fit in registers");
    for (int i = 0; i < n; i++) args[i] = checku32(s, 3 + i);
    lua_pushinteger(s, MOD(s)->api->call_guest(c, addr, args, n));
    return 1;
}

static int l_on_field(lua_State *s) {
    lua_mod *M = MOD(s);
    lua_cb *cb;
    luaL_checktype(s, 1, LUA_TFUNCTION);
    cb = cb_new(s, M, 1, "field callback");
    return push_handle(s, cb, M->api->on_field(field_call, cb), 0);
}

static int l_symbol(lua_State *s) {
    u32 addr = MOD(s)->api->symbol(luaL_checkstring(s, 1));
    if (addr) lua_pushinteger(s, addr); else lua_pushnil(s);
    return 1;
}

static int l_symbol_name(lua_State *s) {
    const char *nm = MOD(s)->api->symbol_name(checku32(s, 1));
    if (nm) lua_pushstring(s, nm); else lua_pushnil(s);
    return 1;
}

static int l_config(lua_State *s) {
    const char *v = MOD(s)->api->config(luaL_checkstring(s, 1));
    if (v) lua_pushstring(s, v); else lua_pushnil(s);
    return 1;
}

static int l_file_size(lua_State *s) {
    int64_t n = MOD(s)->api->file_size(luaL_checkstring(s, 1));
    if (n >= 0) lua_pushinteger(s, (lua_Integer)n); else lua_pushnil(s);
    return 1;
}

static int l_file_read(lua_State *s) {
    lua_mod *M = MOD(s);
    const char *path = luaL_checkstring(s, 1);
    lua_Integer pos = luaL_optinteger(s, 2, 0);
    int64_t size = M->api->file_size(path), len, got;
    luaL_Buffer b;
    char *dst;
    luaL_argcheck(s, pos >= 0, 2, "negative position");
    if (size < 0) { lua_pushnil(s); return 1; }
    len = pos < size ? size - pos : 0;
    if (!lua_isnoneornil(s, 3)) {
        lua_Integer want = luaL_checkinteger(s, 3);
        luaL_argcheck(s, want >= 0, 3, "negative length");
        if (want < len) len = want;
    }
    if (len > (int64_t)FILE_READ_MAX)
        return luaL_error(s, "file_read of %I bytes; read it in pieces of at "
                             "most %d", (lua_Integer)len, (int)FILE_READ_MAX);
    dst = luaL_buffinitsize(s, &b, (size_t)len);
    got = M->api->file_read(path, (u64)pos, dst, (u32)len);
    if (got < 0) { lua_pushnil(s); return 1; }
    luaL_pushresultsize(&b, (size_t)got);
    return 1;
}

static int l_file_to_guest(lua_State *s) {
    lua_mod *M = MOD(s);
    const char *path = luaL_checkstring(s, 1);
    u32 addr = checku32(s, 2);
    lua_Integer pos = luaL_optinteger(s, 3, 0);
    lua_Integer len = luaL_optinteger(s, 4, (lua_Integer)PS2_RAM_SIZE);
    int64_t got;
    luaL_argcheck(s, pos >= 0, 3, "negative position");
    luaL_argcheck(s, len >= 0, 4, "negative length");
    if (len > (lua_Integer)PS2_RAM_SIZE) len = PS2_RAM_SIZE;
    got = M->api->file_to_guest(path, (u64)pos, addr, (u32)len);
    if (got >= 0) lua_pushinteger(s, (lua_Integer)got); else lua_pushnil(s);
    return 1;
}

static const luaL_Reg ac5_lib[] = {
    {"log",           l_log},
    {"read8",         l_read8},
    {"read16",        l_read16},
    {"read32",        l_read32},
    {"read_float",    l_read_float},
    {"write8",        l_write8},
    {"write16",       l_write16},
    {"write32",       l_write32},
    {"write_float",   l_write_float},
    {"read_bytes",    l_read_bytes},
    {"write_bytes",   l_write_bytes},
    {"arg",           l_arg},
    {"set_return",    l_set_return},
    {"reg",           l_reg},
    {"set_reg",       l_set_reg},
    {"hook_before",   l_hook_before},
    {"hook_replace",  l_hook_replace},
    {"call_original", l_call_original},
    {"redirect",      l_redirect},
    {"call_guest",    l_call_guest},
    {"on_field",      l_on_field},
    {"symbol",        l_symbol},
    {"symbol_name",   l_symbol_name},
    {"config",        l_config},
    {"file_size",     l_file_size},
    {"file_read",     l_file_read},
    {"file_to_guest", l_file_to_guest},
    {"hook_after",    l_hook_after},
    {"unhook",        l_unhook},
    {"entry_arg",     l_entry_arg},
    {"return_value",  l_return_value},
    {"return_float",  l_return_float},
    {"set_return_float", l_set_return_float},
    {"reg_float",     l_reg_float},
    {"set_reg_float", l_set_reg_float},
    {"on_frame",      l_on_frame},
    {"on_scene",      l_on_scene},
    {"after",         l_after},
    {"every",         l_every},
    {"on_shutdown",   l_on_shutdown},
    {"cancel",        l_cancel},
    {"pad",           l_pad},
    {"on_input",      l_on_input},
    {"patch",         l_patch},
    {"unpatch",       l_unpatch},
    {"param",         l_param},
    {"store",         l_store},
    {"stored",        l_stored},
    {NULL, NULL},
};

static int l_print(lua_State *s) {
    return l_log(s);
}

static int l_load(lua_State *s) {
    lua_mod *M = MOD(s);
    int has_env = !lua_isnone(s, 4);
    lua_settop(s, 4);
    lua_pushvalue(s, lua_upvalueindex(2));
    lua_pushvalue(s, 1);
    lua_pushvalue(s, 2);
    lua_pushliteral(s, "t");
    if (has_env) lua_pushvalue(s, 4);
    else lua_rawgeti(s, LUA_REGISTRYINDEX, M->env);
    lua_call(s, 4, LUA_MULTRET);
    return lua_gettop(s) - 4;
}

static int l_require(lua_State *s) {
    lua_mod *M = MOD(s);
    const char *name = luaL_checkstring(s, 1);
    char rel[256], path[1024];
    size_t n = strlen(name);
    luaL_argcheck(s, n > 0 && n < 200, 1, "a module name");
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '_' || c == '-'
              || (c == '.' && i && name[i - 1] != '.' && i + 1 < n)))
            return luaL_argerror(s, 1, "module names are letters, digits, _ "
                                       "and - separated by single dots");
        rel[i] = c == '.' ? '/' : c;
    }
    memcpy(rel + n, ".lua", 5);
    if (lua_getfield(s, lua_upvalueindex(2), name) != LUA_TNIL) return 1;
    lua_pop(s, 1);
    snprintf(path, sizeof path, "%s/%s", M->dir, rel);
    if (luaL_loadfilex(s, path, "t") != LUA_OK)
        return luaL_error(s, "require '%s': %s", name, lua_tostring(s, -1));
    lua_rawgeti(s, LUA_REGISTRYINDEX, M->env);
    if (!lua_setupvalue(s, -2, 1)) lua_pop(s, 1);
    lua_pushboolean(s, 1);
    lua_setfield(s, lua_upvalueindex(2), name);
    lua_call(s, 0, 1);
    if (lua_isnil(s, -1)) {
        lua_pop(s, 1);
        lua_pushboolean(s, 1);
    }
    lua_pushvalue(s, -1);
    lua_setfield(s, lua_upvalueindex(2), name);
    return 1;
}

static void copy_table(const char *lib) {
    lua_getglobal(L, lib);
    lua_newtable(L);
    lua_pushnil(L);
    while (lua_next(L, -3)) {
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_rawset(L, -4);
    }
    lua_setfield(L, -3, lib);
    lua_pop(L, 1);
}

static lua_mod *make_mod(const ac5_api *api, const char *dir) {
    static const char *const globals[] = {
        "assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal",
        "rawget", "rawlen", "rawset", "select", "setmetatable",
        "getmetatable", "tonumber", "tostring", "type", "xpcall", "_VERSION",
        NULL };
    static const char *const libs[] = {
        "string", "table", "math", "utf8", "coroutine", NULL };
    static const char *const buttons[16] = {
        "select", "l3", "r3", "start", "up", "right", "down", "left",
        "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square" };
    lua_mod *M = (lua_mod *)calloc(1, sizeof *M);
    if (!M) ps2_fatal("lua: out of memory");
    M->api = api;
    snprintf(M->dir, sizeof M->dir, "%s", dir);

    lua_newtable(L);
    for (const char *const *g = globals; *g; g++) {
        lua_getglobal(L, *g);
        lua_setfield(L, -2, *g);
    }
    for (const char *const *l = libs; *l; l++) copy_table(*l);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "_G");

    lua_pushlightuserdata(L, M);
    lua_pushcclosure(L, l_print, 1);
    lua_setfield(L, -2, "print");
    lua_pushlightuserdata(L, M);
    lua_getfield(L, LUA_REGISTRYINDEX, "ac5.rawload");
    lua_pushcclosure(L, l_load, 2);
    lua_setfield(L, -2, "load");
    lua_pushlightuserdata(L, M);
    lua_newtable(L);
    lua_pushcclosure(L, l_require, 2);
    lua_setfield(L, -2, "require");

    luaL_newlibtable(L, ac5_lib);
    lua_pushlightuserdata(L, M);
    luaL_setfuncs(L, ac5_lib, 1);
    lua_createtable(L, 0, 16);
    for (int b = 0; b < 16; b++) {
        lua_pushinteger(L, 1 << b);
        lua_setfield(L, -2, buttons[b]);
    }
    lua_setfield(L, -2, "BTN");
    lua_pushstring(L, api->mod_id);
    lua_setfield(L, -2, "mod_id");
    lua_pushinteger(L, api->priority);
    lua_setfield(L, -2, "priority");
    lua_pushinteger(L, api->abi_version);
    lua_setfield(L, -2, "abi_version");
    lua_pushstring(L, api->game_id);
    lua_setfield(L, -2, "game_id");
    lua_setfield(L, -2, "ac5");

    M->env = luaL_ref(L, LUA_REGISTRYINDEX);
    return M;
}

static int state_open(void) {
    static const luaL_Reg libs[] = {
        {LUA_GNAME,       luaopen_base},
        {LUA_TABLIBNAME,  luaopen_table},
        {LUA_STRLIBNAME,  luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
        {LUA_COLIBNAME,   luaopen_coroutine},
        {NULL, NULL},
    };
    L = luaL_newstate();
    if (!L) {
        ps2_log("lua: cannot create a Lua state");
        return -1;
    }
    for (const luaL_Reg *l = libs; l->func; l++) {
        luaL_requiref(L, l->name, l->func, 1);
        lua_pop(L, 1);
    }
    lua_getglobal(L, "load");
    lua_setfield(L, LUA_REGISTRYINDEX, "ac5.rawload");
    lua_pushliteral(L, "");
    if (lua_getmetatable(L, -1)) {
        lua_pushliteral(L, "string");
        lua_setfield(L, -2, "__metatable");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    luaL_newmetatable(L, "ac5.ctx");
    lua_pushliteral(L, "ac5.ctx");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
    return 0;
}

static int name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void run_script(lua_mod *M, const char *path) {
    const ac5_api *prev;
    int base, rc;
    lua_pushcfunction(L, lua_msgh);
    base = lua_gettop(L);
    rc = luaL_loadfilex(L, path, "t");
    if (rc == LUA_OK) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, M->env);
        if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);
        if (!lua_enter("script")) {
            lua_settop(L, base - 1);
            return;
        }
        prev = ps2_modapi_enter(M->api);
        rc = lua_pcall(L, 0, 0, base);
        ps2_modapi_leave(prev);
        lua_leave();
    }
    if (rc == LUA_OK) {
        M->scripts++;
        ps2_log("lua[%s]: %s loaded", M->api->mod_id, path);
    } else {
        M->script_errors++;
        ps2_log("lua[%s]: %s failed to load:", M->api->mod_id, path);
        log_error_lines(lua_tostring(L, -1));
    }
    lua_settop(L, base - 1);
}

void ps2_lua_start(void) {
    unsigned n = ps2_mod_count(), found = 0;
    ps2_log("lua: looking for scripts in %u mod(s)", n);
    for (unsigned i = 0; i < n; i++) {
        const char *dir = ps2_mod_dir_at(i);
        char **names = NULL;
        unsigned nnames = 0, cnames = 0;
        DIR *d;
        struct dirent *e;
        lua_mod *M = NULL;
        if (!dir) continue;
        d = opendir(dir);
        if (!d) {
            ps2_log("lua: cannot open %s", dir);
            continue;
        }
        while ((e = readdir(d)) != NULL) {
            size_t len = strlen(e->d_name);
            if (len < 5 || strcmp(e->d_name + len - 4, ".lua")) continue;
            if (nnames == cnames) {
                cnames = cnames ? cnames * 2u : 8u;
                names = (char **)realloc(names, cnames * sizeof *names);
                if (!names) ps2_fatal("lua: out of memory listing scripts");
            }
            names[nnames] = (char *)malloc(len + 1);
            if (!names[nnames]) ps2_fatal("lua: out of memory listing scripts");
            memcpy(names[nnames], e->d_name, len + 1);
            nnames++;
        }
        closedir(d);
        if (nnames > 1) qsort(names, nnames, sizeof *names, name_cmp);
        for (unsigned k = 0; k < nnames; k++) {
            char path[1024];
            int pn = snprintf(path, sizeof path, "%s/%s", dir, names[k]);
            free(names[k]);
            if (pn < 0 || (size_t)pn >= sizeof path) continue;
            if (!L && state_open() != 0) break;
            if (!M) {
                M = make_mod(ps2_mod_api_at(i), dir);
                lmods = (lua_mod **)realloc(lmods, (nlmods + 1u) * sizeof *lmods);
                if (!lmods) ps2_fatal("lua: out of memory");
                lmods[nlmods++] = M;
            }
            found++;
            run_script(M, path);
        }
        free(names);
    }
    if (!found) ps2_log("lua: no .lua files in any mod directory");
}

void ps2_lua_report(void) {
    if (!L) return;
    ps2_log("---- lua ----");
    for (unsigned i = 0; i < nlmods; i++) {
        const lua_mod *M = lmods[i];
        unsigned callbacks = 0;
        u64 calls = 0;
        for (unsigned k = 0; k < ncbs; k++)
            if (cbs[k]->mod == M) { callbacks++; calls += cbs[k]->calls; }
        ps2_log("   %-24s %u script(s)%s, %u callback(s) run %llu time(s), "
                "%u error(s)%s", M->api->mod_id, M->scripts,
                M->script_errors ? " (some failed to load)" : "", callbacks,
                (unsigned long long)calls, M->errors,
                M->disabled ? ", some callbacks switched off" : "");
    }
    if (refused_thread)
        ps2_log("   %llu call(s) declined: not on a thread holding the EE",
                (unsigned long long)refused_thread);
    if (refused_switch)
        ps2_log("   %llu call(s) declined: another guest thread's Lua call "
                "was still open", (unsigned long long)refused_switch);
    if (refused_depth)
        ps2_log("   %llu call(s) declined: nested %u deep",
                (unsigned long long)refused_depth, LUA_MAX_DEPTH);
    if (stack_drift)
        ps2_log("   %llu call(s) left the stack unbalanced",
                (unsigned long long)stack_drift);
}
