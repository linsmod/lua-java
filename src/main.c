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

/* ---- java_main: Lua function (not C!) so debug hook can yield safely ---- */
/* Loaded at init via luaL_dostring (see main()) */

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

static int g_dump = 0;  /* global: enable bytecode dump? */

static int writer_cb(lua_State *L, const void *p, size_t sz, void *ud) {
    (void)L;
    FILE *f = (FILE *)ud;
    return fwrite(p, sz, 1, f) == 1 ? 0 : 1;
}

static void dump_proto_debug(const Proto *f, int indent) {
    int i;
    const char *sp = "  ";
    for (int j = 0; j < indent; j++) printf("  ");
    printf("=== Proto %p: params=%d stack=%d code=%d consts=%d protos=%d ===\n",
           (void*)f, f->numparams, f->maxstacksize, f->sizecode, f->sizek, f->sizep);
    fflush(stdout);
    /* print constants */
    if (f->sizek > 0) {
        for (int j = 0; j < indent; j++) printf("  ");
        printf("-- Constants (%d):\n", f->sizek);
        for (i = 0; i < f->sizek; i++) {
            TValue *kv = &f->k[i];
            for (int j = 0; j < indent; j++) printf("  ");
            printf("  [%2d] ", i);
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
    }
    /* print bytecode */
    if (f->sizecode > 0) {
        for (int j = 0; j < indent; j++) printf("  ");
        printf("-- Code (%d):\n", f->sizecode);
        for (i = 0; i < f->sizecode; i++) {
            Instruction inst = f->code[i];
            OpCode op = GET_OPCODE(inst);
            for (int j = 0; j < indent; j++) printf("  ");
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
                            else if (ttisfloat(kv)) printf(" ; %g", fltvalue(kv));
                        }
                    }
                    if (op == OP_GETTABLE || op == OP_SETTABLE) {
                        printf("  -- %s", op == OP_GETTABLE ? "get" : "set");
                    }
                    if (op == OP_GETTABUP) printf("  -- _ENV lookup");
                    if (op == OP_SETTABUP) printf("  -- _ENV store");
                    if (op == OP_CLOSURE) {
                        int pi = GETARG_Bx(inst);
                        printf("  -- closure proto[%d]", pi);
                    }
                    if (op == OP_CALL) {
                        printf("  -- call(args=%d, rets=%d)",
                               GETARG_B(inst), GETARG_C(inst));
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
    }
    fflush(stdout);
    /* recurse into sub-protos (methods / nested functions) */
    for (i = 0; i < f->sizep; i++) {
        if (f->p[i] == NULL) {
            for (int j = 0; j < indent; j++) printf("  ");
            printf("  Subproto[%d]: NULL\n", i);
        } else {
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
    /* ---- Dump generated bytecode (when -d is active) ---- */
    if (g_dump) {
        printf("  --- Bytecode dump ---\n");
        fflush(stdout);
        dump_function_debug(L, -1);
        printf("  --- End bytecode dump ---\n");
        fflush(stdout);
    }
    /* execute: runs the compiled closure, which returns the class table */
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        fprintf(stderr, "Runtime error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    /* Return value: int exit code from main() (nil → 0) */
    {
        int exit_code = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 0;
        if (exit_code != 0)
            printf("Exit code: %d\n", exit_code);
        lua_pop(L, 1);
    }

    /* Debug: check _ENV entries */
    lua_getglobal(L, "add");
    printf("  _ENV['add'] type: %s\n", luaL_typename(L, -1));
    lua_pop(L, 1);

    lua_getglobal(L, "sum");
    printf("  _ENV['sum'] type: %s\n", luaL_typename(L, -1));
    lua_pop(L, 1);

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

static void print_usage(const char *prog) {
    printf("Usage: %s [-d] [file...]\n", prog);
    printf("  -d       dump generated Lua bytecode for .java files\n");
    printf("  Each file is loaded and executed:\n");
    printf("    .java  → compiled by Java→Lua compiler, then run\n");
    printf("    .lua   → run as Lua script\n");
    printf("  With no arguments, runs default test suite.\n");
    printf("Examples:\n");
    printf("  %s                                    (default suite)\n", prog);
    printf("  %s -d scripts/tests/12_instance_method.java\n", prog);
    printf("  %s scripts/tests/*.java\n", prog);
}

int main(int argc, char *argv[]) {
    lua_State *L = luaL_newstate();
    if (!L) { fprintf(stderr, "Failed to create Lua state\n"); return 1; }

    luaL_openlibs(L);
    jlex_init(L);     /* register Java reserved words */
    java_openlib(L);  /* register System.out, primitive types */
    /* java_main as Lua function (not C!) so debug hook can yield inside main() */
    if (luaL_dostring(L,
        "java_main = function()\n"
        "  local argc = argc or 0\n"
        "  local argv = argv\n"
        "  for k, v in pairs(_ENV) do\n"
        "    if type(v) == 'table' and type(v.main) == 'function' then\n"
        "      return v.main(argc, argv) or 0\n"
        "    end\n"
        "  end\n"
        "  return 0\n"
        "end\n") != LUA_OK) {
        fprintf(stderr, "Failed to load java_main: %s\n",
                lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_gc(L, LUA_GCSTOP, 0);  /* DEBUG: stop GC to check for GC corruption */

    printf("=== Lua + Java Test ===\n");
    printf("Lua version: %s\n\n", LUA_RELEASE);

    /* Set argc/argv globals for java_main to read at runtime */
    lua_pushinteger(L, argc);
    lua_setglobal(L, "argc");
    lua_pushlightuserdata(L, argv);
    lua_setglobal(L, "argv");

    lua_register(L, "c_add",        c_add);
    lua_register(L, "c_uppercase",  c_uppercase);
    lua_register(L, "c_get_version", c_get_version);
    lua_register(L, "_new_instance", c_new_instance);
    lua_register(L, "_setmetatable", c_setmetatable);

    /* ---- Parse -d flag ---- */
    int argi = 1;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        g_dump = 1;
        argi = 2;
    }

    if (argi >= argc) {
        /* ---- Default: run smoke-test suite ---- */
        printf("No arguments supplied – running default smoke tests.\n");
        print_usage(argv[0]);

        /* Inline Lua */
        printf("\n--- Inline Lua ---\n");
        luaL_dostring(L,
            "print('Hello from inline Lua!')\n"
            "print('3.14 + 2.86 = ' .. c_add(3.14, 2.86))\n"
        );

        /* External Lua script */
        run_lua_file(L, "scripts/test.lua");

        /* Minimal Java smoke tests */
        run_java_file(L, "scripts/test_min2.java");
        run_java_file(L, "scripts/test_min.java");
    } else {
        /* ---- Run user-supplied files ---- */
        int passed = 0, failed = 0;
        for (int i = argi; i < argc; i++) {
            const char *fn = argv[i];
            size_t len = strlen(fn);
            int ok = 0;

            if (len > 5 && strcmp(fn + len - 5, ".java") == 0) {
                ok = run_java_file(L, fn);
            } else if (len > 4 && strcmp(fn + len - 4, ".lua") == 0) {
                ok = run_lua_file(L, fn);
            } else {
                fprintf(stderr, "Unknown file type (skip): %s\n", fn);
                continue;
            }

            if (ok) passed++; else failed++;
        }
        printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    }

    lua_close(L);
    printf("\n=== Done ===\n");
    return 0;
}
