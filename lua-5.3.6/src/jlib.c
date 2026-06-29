/*
** Java Runtime Library  (jlib.c)
** Provides System.out (console I/O) and primitive type helpers.
*/

#define LUA_CORE
#include "lprefix.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "jlib.h"

/* forward declarations */
static int j_wildcard_index(lua_State *L);
static int j_import_wildcard(lua_State *L);

/* ================================================================
 *  Console output  —  System.out.println / System.out.print
 * ================================================================ */

/* Helper: write one value to stdout */
static void jlib_writeval(lua_State *L, int idx) {
  int t = lua_type(L, idx);
  switch (t) {
    case LUA_TNUMBER: {
      if (lua_isinteger(L, idx)) {
        fprintf(stdout, LUA_INTEGER_FMT, lua_tointeger(L, idx));
      } else {
        lua_Number v = lua_tonumber(L, idx);
        fprintf(stdout, LUA_NUMBER_FMT, v);
      }
      break;
    }
    case LUA_TSTRING: {
      size_t len;
      const char *s = lua_tolstring(L, idx, &len);
      fwrite(s, 1, len, stdout);
      break;
    }
    case LUA_TBOOLEAN:
      fputs(lua_toboolean(L, idx) ? "true" : "false", stdout);
      break;
    case LUA_TNIL:
      fputs("null", stdout);
      break;
    default: {
      /* fallback: use tostring */
      lua_getglobal(L, "tostring");
      lua_pushvalue(L, idx);
      lua_call(L, 1, 1);
      size_t len;
      const char *s = lua_tolstring(L, -1, &len);
      fwrite(s, 1, len, stdout);
      lua_pop(L, 1);
      break;
    }
  }
}

/* System.out.println(...)  —  print args with newline */
static int j_sys_println(lua_State *L) {
  int n = lua_gettop(L);
  int i;
  for (i = 1; i <= n; i++) {
    if (i > 1) printf("\t");
    jlib_writeval(L, i);
  }
  printf("\n");
  fflush(stdout);
  return 0;
}

/* System.out.print(...)  —  print args without newline */
static int j_sys_print(lua_State *L) {
  int n = lua_gettop(L);
  int i;
  for (i = 1; i <= n; i++) {
    if (i > 1) printf("\t");
    jlib_writeval(L, i);
  }
  fflush(stdout);
  return 0;
}


/* ================================================================
 *  Primitive type wrappers
 * ================================================================ */

/* Integer(x)  —  convert to integer (truncate toward zero) */
static int j_Integer(lua_State *L) {
  if (lua_isinteger(L, 1)) {
    lua_pushinteger(L, lua_tointeger(L, 1));
  } else if (lua_isnumber(L, 1)) {
    lua_Number v = lua_tonumber(L, 1);
    lua_pushinteger(L, (lua_Integer)trunc(v));
  } else if (lua_isboolean(L, 1)) {
    lua_pushinteger(L, lua_toboolean(L, 1) ? 1 : 0);
  } else {
    lua_pushinteger(L, 0);
  }
  return 1;
}

/* Float(x)  —  convert to float */
static int j_Float(lua_State *L) {
  lua_Number v = luaL_checknumber(L, 1);
  lua_pushnumber(L, v);
  return 1;
}

/* Boolean(x)  —  convert to boolean (Java truthiness) */
static int j_Boolean(lua_State *L) {
  lua_pushboolean(L, lua_toboolean(L, 1));
  return 1;
}

/* String.valueOf(x)  —  convert to string */
static int j_String_valueOf(lua_State *L) {
  if (lua_isstring(L, 1)) {
    lua_pushvalue(L, 1);
  } else if (lua_isinteger(L, 1)) {
    lua_pushfstring(L, LUA_INTEGER_FMT, lua_tointeger(L, 1));
  } else if (lua_isnumber(L, 1)) {
    lua_pushfstring(L, LUA_NUMBER_FMT, lua_tonumber(L, 1));
  } else if (lua_isboolean(L, 1)) {
    lua_pushstring(L, lua_toboolean(L, 1) ? "true" : "false");
  } else if (lua_isnoneornil(L, 1)) {
    lua_pushstring(L, "null");
  } else {
    lua_getglobal(L, "tostring");
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
  }
  return 1;
}


/* ================================================================
 *  ArrayList  —  java.util.ArrayList (C implementation)
 * ================================================================ */

static int j_arraylist_ctor(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);       /* self */
  /* Create metatable: {__index = ArrayList} */
  lua_newtable(L);                          /* mt */
  lua_getglobal(L, "ArrayList");            /* class table */
  lua_setfield(L, -2, "__index");
  lua_setmetatable(L, 1);                   /* setmetatable(self, mt) */
  /* initialize _size = 0 */
  lua_pushinteger(L, 0);
  lua_setfield(L, 1, "_size");
  return 0;
}

static int j_arraylist_add(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_getfield(L, 1, "_size");
  lua_Integer sz = lua_tointeger(L, -1);
  lua_pop(L, 1);
  sz++;
  lua_pushvalue(L, 2);
  lua_seti(L, 1, sz);
  lua_pushinteger(L, sz);
  lua_setfield(L, 1, "_size");
  return 0;
}

static int j_arraylist_get(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_Integer idx = luaL_checkinteger(L, 2) + 1;  /* Java 0-based → Lua 1-based */
  lua_geti(L, 1, idx);
  return 1;
}

static int j_arraylist_size(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_getfield(L, 1, "_size");
  return 1;
}

/* ================================================================
 *  Java file searcher for require()
 *  Converts "java.util.Xxx" → "java/util/Xxx.java" and compiles it.
 *  Returns (compiled_chunk, path) on success, error message on failure.
 * ================================================================ */

static int j_searcher_java(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);

  /* Build path: dots → slashes + ".java" */
  char path[512];
  int pos = 0;
  for (const char *p = name; *p && pos < 500; p++)
    path[pos++] = (*p == '.') ? '/' : *p;
  memcpy(path + pos, ".java", 6);  /* includes '\0' */

  /* Compile the .java file as a Java→Lua chunk */
  if (luaL_loadfilex(L, path, "j") != LUA_OK) {
    /* loadfile left error on stack — pop it, return our own message */
    lua_pop(L, 1);
    lua_pushfstring(L, "\n\tno java module '%s' (%s)", name, path);
    return 1;
  }

  /* Success: return (compiled_chunk, path) for require to execute */
  lua_pushstring(L, path);
  return 2;
}

/* ================================================================
 *  HashMap  —  java.util.HashMap (C implementation)
 * ================================================================ */

static int j_hashmap_ctor(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);       /* self */
  lua_newtable(L);                          /* mt */
  lua_getglobal(L, "HashMap");              /* class table */
  lua_setfield(L, -2, "__index");
  lua_setmetatable(L, 1);
  /* init _size = 0 */
  lua_pushinteger(L, 0);
  lua_setfield(L, 1, "_size");
  return 0;
}

static int j_hashmap_put(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  /* Check if key already exists (increment _size only for new keys) */
  lua_pushvalue(L, 2);  /* key */
  lua_gettable(L, 1);   /* self[key] */
  int is_new = lua_isnil(L, -1);
  lua_pop(L, 1);
  if (is_new) {
    lua_getfield(L, 1, "_size");
    lua_pushinteger(L, lua_tointeger(L, -1) + 1);
    lua_setfield(L, 1, "_size");
    lua_pop(L, 1);
  }
  /* store: self[key] = value, then return the old value or nil */
  lua_pushvalue(L, 2);  /* key */
  lua_pushvalue(L, 3);  /* value */
  lua_settable(L, 1);
  return 0;
}

static int j_hashmap_get(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_pushvalue(L, 2);   /* key */
  lua_gettable(L, 1);    /* self[key] */
  return 1;
}

static int j_hashmap_size(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_getfield(L, 1, "_size");
  return 1;
}

static int j_hashmap_containsKey(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_pushvalue(L, 2);
  lua_gettable(L, 1);
  lua_pushboolean(L, !lua_isnil(L, -1));
  return 1;
}

/* ================================================================
 *  StringBuilder  —  java.lang.StringBuilder (C implementation)
 * ================================================================ */

static int j_sb_ctor(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);       /* self */
  lua_newtable(L);                          /* mt */
  lua_getglobal(L, "StringBuilder");        /* class table */
  lua_setfield(L, -2, "__index");
  lua_setmetatable(L, 1);
  /* init _buf = "" (or initial string if provided) */
  if (lua_gettop(L) >= 2 && lua_isstring(L, 2)) {
    lua_pushvalue(L, 2);
  } else if (lua_gettop(L) >= 2) {
    /* fallback: call tostring on arg */
    lua_getglobal(L, "tostring");
    lua_pushvalue(L, 2);
    lua_call(L, 1, 1);
  } else {
    lua_pushstring(L, "");
  }
  lua_setfield(L, 1, "_buf");
  return 0;
}

static int j_sb_append(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_getfield(L, 1, "_buf");
  /* convert arg to string */
  if (lua_isstring(L, 2)) {
    lua_pushvalue(L, 2);
  } else if (lua_isinteger(L, 2)) {
    lua_pushfstring(L, LUA_INTEGER_FMT, lua_tointeger(L, 2));
  } else if (lua_isnumber(L, 2)) {
    lua_pushfstring(L, LUA_NUMBER_FMT, lua_tonumber(L, 2));
  } else {
    lua_getglobal(L, "tostring");
    lua_pushvalue(L, 2);
    lua_call(L, 1, 1);
  }
  lua_concat(L, 2);         /* _buf .. arg_str */
  lua_setfield(L, 1, "_buf");
  lua_pushvalue(L, 1);      /* return self for chaining */
  return 1;
}

static int j_sb_toString(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_getfield(L, 1, "_buf");
  return 1;
}

/* ================================================================
 *  Preload helper: returns an upvalue (used for package.preload)
 * ================================================================ */

static int j_preload_value(lua_State *L) {
  lua_pushvalue(L, lua_upvalueindex(1));  /* return the upvalue */
  return 1;
}

/* ================================================================
 *  Main registration
 * ================================================================ */

/* Helpers to register constants/literals as globals */
static void jlib_set_global_cf(lua_State *L, const char *name,
                                lua_CFunction fn) {
  lua_pushcfunction(L, fn);
  lua_setglobal(L, name);
}

void java_openlib(lua_State *L) {
  /* ---- System.out.println / System.out.print ---- */
  lua_newtable(L);  /* System table */

  lua_newtable(L);  /* System.out table */
  lua_pushcfunction(L, j_sys_println);
  lua_setfield(L, -2, "println");
  lua_pushcfunction(L, j_sys_print);
  lua_setfield(L, -2, "print");
  lua_setfield(L, -2, "out");  /* System.out = out_table */

  lua_setglobal(L, "System");

  /* ---- Primitive type conversion helpers (Java-like) ---- */
  jlib_set_global_cf(L, "Integer", j_Integer);
  jlib_set_global_cf(L, "Float",   j_Float);
  jlib_set_global_cf(L, "Boolean", j_Boolean);

  /* ---- String.valueOf ---- */
  lua_newtable(L);
  lua_pushcfunction(L, j_String_valueOf);
  lua_setfield(L, -2, "valueOf");
  lua_setglobal(L, "String");

  /* ---- ArrayList (java.util.ArrayList) ---- */
  {
    lua_newtable(L);  /* ArrayList class table */

    lua_pushcfunction(L, j_arraylist_ctor);
    lua_setfield(L, -2, "ArrayList");  /* constructor */
    lua_pushcfunction(L, j_arraylist_ctor);
    lua_setfield(L, -2, "new");        /* also as 'new' */

    lua_pushcfunction(L, j_arraylist_add);
    lua_setfield(L, -2, "add");
    lua_pushcfunction(L, j_arraylist_get);
    lua_setfield(L, -2, "get");
    lua_pushcfunction(L, j_arraylist_size);
    lua_setfield(L, -2, "size");

    lua_setglobal(L, "ArrayList");
  }

  /* ---- HashMap (java.util.HashMap) ---- */
  {
    lua_newtable(L);  /* HashMap class table */

    lua_pushcfunction(L, j_hashmap_ctor);
    lua_setfield(L, -2, "HashMap");   /* constructor */
    lua_pushcfunction(L, j_hashmap_ctor);
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, j_hashmap_put);
    lua_setfield(L, -2, "put");
    lua_pushcfunction(L, j_hashmap_get);
    lua_setfield(L, -2, "get");
    lua_pushcfunction(L, j_hashmap_size);
    lua_setfield(L, -2, "size");
    lua_pushcfunction(L, j_hashmap_containsKey);
    lua_setfield(L, -2, "containsKey");

    lua_setglobal(L, "HashMap");
  }

  /* ---- StringBuilder (java.lang.StringBuilder) ---- */
  {
    lua_newtable(L);  /* StringBuilder class table */

    lua_pushcfunction(L, j_sb_ctor);
    lua_setfield(L, -2, "StringBuilder");  /* constructor */
    lua_pushcfunction(L, j_sb_ctor);
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, j_sb_append);
    lua_setfield(L, -2, "append");
    lua_pushcfunction(L, j_sb_toString);
    lua_setfield(L, -2, "toString");

    lua_setglobal(L, "StringBuilder");
  }

  /* ---- Register built-in classes as preload modules ---- */
  /* require("java.util.ArrayList") returns the ArrayList class table */
  lua_getglobal(L, "package");                        /* stack: package */
  lua_getfield(L, -1, "preload");                     /* stack: package, preload */

  /* java.util.ArrayList → returns ArrayList global */
  lua_getglobal(L, "ArrayList");                      /* push the class table */
  lua_pushcclosure(L, j_preload_value, 1);            /* closure with upvalue=ArrayList */
  lua_setfield(L, -2, "java.util.ArrayList");

  /* java.util.List → same as ArrayList (interface placeholder) */
  lua_getglobal(L, "ArrayList");
  lua_pushcclosure(L, j_preload_value, 1);
  lua_setfield(L, -2, "java.util.List");

  /* java.util.HashMap → HashMap global */
  lua_getglobal(L, "HashMap");
  lua_pushcclosure(L, j_preload_value, 1);
  lua_setfield(L, -2, "java.util.HashMap");

  /* java.lang.StringBuilder → StringBuilder global */
  lua_getglobal(L, "StringBuilder");
  lua_pushcclosure(L, j_preload_value, 1);
  lua_setfield(L, -2, "java.lang.StringBuilder");

  lua_pop(L, 2);  /* pop preload and package */

  /* ---- Register Java file searcher in package.searchers ---- */
  lua_getglobal(L, "package");
  lua_getfield(L, -1, "searchers");
  if (lua_istable(L, -1)) {
    /* Find last occupied index and append after it */
    int idx = 1;
    while (1) {
      lua_rawgeti(L, -1, idx);
      int isnil = lua_isnil(L, -1);
      lua_pop(L, 1);
      if (isnil) break;
      idx++;
    }
    lua_pushcfunction(L, j_searcher_java);
    lua_rawseti(L, -2, idx);  /* searchers[n] = func, pops func */
  }
  lua_pop(L, 2);  /* pop searchers and package */

  /* ---- Wildcard import infrastructure ---- */
  /* Initialize wildcard-import runtime */
  lua_newtable(L);
  lua_setglobal(L, "__WILDCARD_PACKAGES");
  jlib_set_global_cf(L, "_j_import_wildcard", j_import_wildcard);

  /* Math: Java code uses 'Math.floor', 'Math.abs', etc. (capital M).
   * Lua's standard library is lowercase 'math'; alias it as 'Math' so
   * Java-style static calls resolve correctly. */
  lua_getglobal(L, "math");
  lua_setglobal(L, "Math");
}

/* ================================================================
 *  Wildcard import  —  _j_import_wildcard(package_name)
 *
 *  Usage: import java.util.*   →  _j_import_wildcard("java.util")
 *
 *  Adds the package to a global list and sets up a __index
 *  metatable on _G so that unknown global lookups try require()
 *  with each wildcard package prefix.
 * ================================================================ */

/* __index handler for _G: resolve unknown globals via wildcard packages */
static int j_wildcard_index(lua_State *L) {
  /* L[1]=_G(table), L[2]=key(class name) */
  /* Try rawget on _G first (avoid __index recursion) */
  lua_pushvalue(L, 2);
  lua_rawget(L, 1);
  if (!lua_isnil(L, -1))
    return 1;  /* already exists */
  lua_pop(L, 1);

  /* Key must be a string and look like a class name */
  if (lua_type(L, 2) != LUA_TSTRING) {
    lua_pushnil(L);
    return 1;
  }
  const char *name = lua_tostring(L, 2);
  if (name == NULL || name[0] < 'A' || name[0] > 'Z') {
    /* Only try to resolve Capitalized names (class names) */
    lua_pushnil(L);
    return 1;
  }

  /* Get wildcard packages list */
  lua_getglobal(L, "__WILDCARD_PACKAGES");
  int len = (int)lua_rawlen(L, -1);

  for (int i = 1; i <= len; i++) {
    lua_rawgeti(L, -1, i);         /* package name */
    const char *pkg = lua_tostring(L, -1);
    if (pkg == NULL) { lua_pop(L, 1); continue; }

    char full[256];
    snprintf(full, sizeof(full), "%s.%s", pkg, name);

    /* Try require(pkg . "." . name) */
    lua_getglobal(L, "require");
    lua_pushstring(L, full);
    if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
      /* Success — cache result in _G so next lookup is fast */
      lua_pushvalue(L, 2);         /* key */
      lua_pushvalue(L, -2);        /* result */
      lua_rawset(L, 1);            /* _G[key] = result */

      /* Pop pkgs table + pkg name, leave result on top */
      lua_remove(L, -3);           /* remove pkg table */
      lua_remove(L, -2);           /* remove old pkg name */
      return 1;
    }
    /* require failed — pop error message and package name */
    lua_pop(L, 2);
  }

  lua_pop(L, 1);  /* pop __WILDCARD_PACKAGES table */
  lua_pushnil(L);
  return 1;
}

/* _j_import_wildcard("java.util") — called for wildcard imports */
static int j_import_wildcard(lua_State *L) {
  const char *pkg = luaL_checkstring(L, 1);

  /* Get / ensure __WILDCARD_PACKAGES table */
  lua_getglobal(L, "__WILDCARD_PACKAGES");
  /* Append package to list */
  int len = (int)lua_rawlen(L, -1);
  lua_pushstring(L, pkg);
  lua_rawseti(L, -2, len + 1);
  lua_pop(L, 1);

  /* Set up metatable on _G with __index = j_wildcard_index (once) */
  lua_getglobal(L, "__WILDCARD_MT_SET");
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    lua_getglobal(L, "_G");
    /* If _G already has a metatable, preserve its __index */
    if (!lua_getmetatable(L, -1)) {
      lua_newtable(L);            /* new mt */
      lua_setmetatable(L, -2);    /* setmetatable(_G, mt) */
      lua_getmetatable(L, -1);    /* push mt */
    }
    /* Push our __index on top of existing metatable */
    lua_pushcfunction(L, j_wildcard_index);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 2);  /* pop mt and _G */

    lua_pushboolean(L, 1);
    lua_setglobal(L, "__WILDCARD_MT_SET");
  } else {
    lua_pop(L, 1);
  }

  return 0;
}
