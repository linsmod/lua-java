#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

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

/* ============================================================
 * 2. Load & run a Java file
 * ============================================================ */
static int run_java_file(lua_State *L, const char *filename) {
    printf("\n--- Java: %s ---\n", filename);
    if (luaL_loadjava(L, filename) != LUA_OK) {
        fprintf(stderr, "Compile error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
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
            lua_pop(L, 2);  /* error msg + class table */
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

    printf("=== Lua + Java Test ===\n");
    printf("Lua version: %s\n\n", LUA_RELEASE);

    lua_register(L, "c_add",        c_add);
    lua_register(L, "c_uppercase",  c_uppercase);
    lua_register(L, "c_get_version", c_get_version);

    /* ------ Load & run Java file ------ */
    run_java_file(L, "scripts/test.java");

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
