#define LUA_CORE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "lopcodes.h"
#include "lobject.h"
#include "lstate.h"

/* ---- Lua→Java bridge declarations ---- */
LUAI_FUNC void jlex_init    (lua_State *L);
LUAI_FUNC void java_openlib (lua_State *L);

/* ============================================================
 * 1. Expose C functions to Lua
 * ============================================================ */
static int c_add(lua_State *L) {
    double a = luaL_checknumber(L, 1);
    double b = luaL_checknumber(L, 2);
    lua_pushnumber(L, a + b);
    return 1;
}

static int c_uppercase(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    char buf[256];
    size_t len = strlen(s);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    for (size_t i = 0; i < len; i++)
        buf[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
    buf[len] = '\0';
    lua_pushstring(L, buf);
    return 1;
}

static int c_get_version(lua_State *L) {
    lua_pushfstring(L, "Lua %s  (C side)", LUA_VERSION);
    return 1;
}

/* Helper: create a new object with metatable set to {__index = class_table}.
 * Stack: class_table → new_object (with metatable) */
static int c_new_instance(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);  /* class_table */
    lua_newtable(L);                     /* obj = {} */
    lua_newtable(L);                     /* mt = {} */
    lua_pushvalue(L, 1);                /* mt, class_table */
    lua_setfield(L, -2, "__index");      /* mt.__index = class_table */
    lua_setmetatable(L, -2);            /* setmetatable(obj, mt) */
    return 1;                            /* return obj */
}

/* Helper: set metatable on an existing table.
 * Stack: table, metatable → table (with metatable set) */
static int c_setmetatable(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_setmetatable(L, 1);
    lua_settop(L, 1);
    return 1;
}

static int writer_cb(lua_State *L, const void *p, size_t sz, void *ud) {
    (void)L;
    FILE *f = (FILE *)ud;
    return fwrite(p, sz, 1, f) == 1 ? 0 : 1;
}

static void dump_proto_debug(const Proto *f, int indent) {
    int i;
    (void)indent;
    printf("  dump_proto: f=%p\n", (void*)f); fflush(stdout);
    printf("  dump_proto: numparams=%d\n", f->numparams); fflush(stdout);
    printf("  dump_proto: maxstacksize=%d\n", f->maxstacksize); fflush(stdout);
    printf("  dump_proto: sizecode=%d\n", f->sizecode); fflush(stdout);
    printf("  dump_proto: sizek=%d\n", f->sizek); fflush(stdout);
    printf("  dump_proto: sizep=%d\n", f->sizep); fflush(stdout);
    printf("  Proto: params=%d stack=%d code=%d consts=%d protos=%d\n",
           f->numparams, f->maxstacksize, f->sizecode, f->sizek, f->sizep);
    fflush(stdout);
    /* print constants */
    for (i = 0; i < f->sizek; i++) {
        TValue *kv = &f->k[i];
        printf("    const[%d] = ", i);
        switch (ttype(kv)) {
            case LUA_TNIL: printf("nil"); break;
            case LUA_TBOOLEAN: printf(ivalue(kv) ? "true" : "false"); break;
            case LUA_TNUMFLT: printf("%g", fltvalue(kv)); break;
            case LUA_TNUMINT: printf("%lld", (long long)ivalue(kv)); break;
            case LUA_TSHRSTR: case LUA_TLNGSTR: printf("\"%s\"", getstr(tsvalue(kv))); break;
            default: printf("type=%d", ttype(kv)); break;
        }
        printf("\n");
    }
    fflush(stdout);
    for (i = 0; i < f->sizecode; i++) {
        Instruction inst = f->code[i];
        OpCode op = GET_OPCODE(inst);
        printf("  %4d %-9s A=%-2d", i, luaP_opnames[op], GETARG_A(inst));
        switch (getOpMode(op)) {
            case iABC:
                printf(" B=%-2d C=%-4d", GETARG_B(inst), GETARG_C(inst));
                if (GETARG_C(inst) & BITRK) {
                    int k = GETARG_C(inst) & ~BITRK;
                    if (k < f->sizek) {
                        TValue *kv = &f->k[k];
                        if (ttisstring(kv)) printf(" ; \"%s\"", getstr(tsvalue(kv)));
                        else if (ttisinteger(kv)) printf(" ; %lld", (long long)ivalue(kv));
                    }
                }
                break;
            case iABx:
                printf(" Bx=%-4d", GETARG_Bx(inst));
                if (op == OP_CLOSURE) printf(" ; proto[%d]", GETARG_Bx(inst));
                break;
            case iAsBx:
                printf(" sBx=%-4d", GETARG_sBx(inst));
                break;
            case iAx:
                printf(" Ax=%-4d", GETARG_Ax(inst));
                break;
        }
        printf("\n");
    }
    for (i = 0; i < f->sizep; i++) {
        printf("  Subproto[%d]: ", i);
        if (f->p[i] == NULL) {
            printf("NULL\n");
        } else {
            printf("\n");
            dump_proto_debug(f->p[i], indent + 1);
        }
    }
}

static void dump_function_debug(lua_State *L, int idx) {
    printf("    dump: idx=%d top=%d ci=%p\n", idx, lua_gettop(L), (void*)L->ci);
    fflush(stdout);
    StkId o;
    if (idx > 0) {
        o = L->ci->func + (idx - 1);
    } else {
        o = L->top + idx;
    }
    printf("    dump: o=%p L->stack=%p L->top=%p\n", (void*)o, (void*)L->stack, (void*)L->top);
    fflush(stdout);
    if (ttisLclosure(o)) {
        printf("    dump: it's an LClosure\n"); fflush(stdout);
        LClosure *cl = clLvalue(o);
        printf("    dump: proto=%p\n", (void*)cl->p); fflush(stdout);
        dump_proto_debug(cl->p, 0);
    } else {
        printf("  Not an LClosure, type=%d\n", ttype(o));
    }
}

/* ============================================================
 * 2. Load & run a Java file
 * ============================================================ */
static int run_java_file(lua_State *L, const char *filename) {
    printf("\n--- Java: %s ---\n", filename);
    fflush(stdout);
    if (luaL_loadjava(L, filename) != LUA_OK) {
        fprintf(stderr, "Compile error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    printf("  Compiled OK\n");
    /* execute: runs the compiled closure, which returns the class table */
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        fprintf(stderr, "Runtime error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    /* class table is now on stack */
    if (!lua_istable(L, -1)) {
        fprintf(stderr, "Expected class table, got %s\n", luaL_typename(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    printf("Class loaded successfully!\n");

    /* Debug: check _ENV entries */
    lua_getglobal(L, "add");
    printf("  _ENV['add'] type: %s\n", luaL_typename(L, -1));
    lua_pop(L, 1);

    lua_getglobal(L, "sum");
    printf("  _ENV['sum'] type: %s\n", luaL_typename(L, -1));
    lua_pop(L, 1);

    /* List methods in the class table */
    lua_pushnil(L);
    printf("Methods: ");
    int first = 1;
    while (lua_next(L, -2) != 0) {
        if (!first) printf(", ");
        printf("%s", lua_tostring(L, -2));
        first = 0;
        lua_pop(L, 1);
    }
    printf("\n");

    /* ---- invoke main() if it exists ---- */
    lua_getfield(L, -1, "main");
    if (lua_isfunction(L, -1)) {
        printf("Calling main()...\n");
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            fprintf(stderr, "main() error: %s\n", lua_tostring(L, -1));
            /* traceback */
            luaL_traceback(L, L, NULL, 1);
            fprintf(stderr, "%s\n", lua_tostring(L, -1));
            lua_pop(L, 3);  /* error msg + traceback + class table */
            return 0;
        }
    } else {
        lua_pop(L, 1);  /* nil */
    }

    lua_pop(L, 1); /* pop class table */
    return 1;
}

/* ============================================================
 * 3. Load & run Lua script
 * ============================================================ */
static int run_lua_file(lua_State *L, const char *filename) {
    printf("\n--- Lua: %s ---\n", filename);
    if (luaL_dofile(L, filename) != LUA_OK) {
        fprintf(stderr, "Error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    return 1;
}

/* ============================================================
 * 4. main
 * ============================================================ */
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    lua_State *L = luaL_newstate();
    if (!L) { fprintf(stderr, "Failed to create Lua state\n"); return 1; }

    luaL_openlibs(L);
    jlex_init(L);     /* register Java reserved words */
    java_openlib(L);  /* register System.out, primitive types */
    lua_gc(L, LUA_GCSTOP, 0);  /* DEBUG: stop GC to check for GC corruption */

    printf("=== Lua + Java Test ===\n");
    printf("Lua version: %s\n\n", LUA_RELEASE);

    lua_register(L, "c_add",        c_add);
    lua_register(L, "c_uppercase",  c_uppercase);
    lua_register(L, "c_get_version", c_get_version);
    lua_register(L, "_new_instance", c_new_instance);
    lua_register(L, "_setmetatable", c_setmetatable);

    /* ------ Load & run Java file ------ */
    // run_java_file(L, "scripts/test.java");

    /* ------ Test absolute minimal case ------ */
    run_java_file(L, "scripts/test_min2.java");

    /* ------ Test minimal instance method ------ */
    run_java_file(L, "scripts/test_min.java");

    /* ------ Test instance method ------ */
    // run_java_file(L, "scripts/test_instance.java");

    /* ------ Load & run JavaFeaturesDemo ------ */
    // run_java_file(L, "scripts/com/example/demo/JavaFeaturesDemo.java");

    /* ------ Inline Lua ------ */
    printf("\n--- Inline Lua ---\n");
    luaL_dostring(L,
        "print('Hello from inline Lua!')\n"
        "print('3.14 + 2.86 = ' .. c_add(3.14, 2.86))\n"
    );

    /* ------ External Lua script ------ */
    run_lua_file(L, "scripts/test.lua");

    lua_close(L);
    printf("\n=== Done ===\n");
    return 0;
}
