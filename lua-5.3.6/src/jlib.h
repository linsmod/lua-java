/*
** Java Runtime Library for Lua VM  (jlib.h)
*/

#ifndef jlib_h
#define jlib_h

#include "lua.h"

/* Output writer callback for System.out.print/println redirection.
 * Embedders (Android/GUI/...) install one with java_setwriter();
 * when none is installed, output goes to stdout as before. */
typedef void (*jlib_writer_t)(lua_State *L, const char *s, size_t len, void *ud);

LUAI_FUNC void java_openlib(lua_State *L);
LUAI_FUNC void java_setwriter(lua_State *L, jlib_writer_t w, void *ud);

#endif
