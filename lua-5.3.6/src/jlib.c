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

  /* ---- Math (already from Lua, but re-expose as alias if needed) ---- */
  /* (Lua's math library is already available) */
}
