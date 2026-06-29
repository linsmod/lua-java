/*
** Java Parser for Lua VM  (jparser.h)
*/

#ifndef jparser_h
#define jparser_h

#include "lobject.h"
#include "lzio.h"
#include "lparser.h"  /* FuncState, expdesc, BlockCnt, etc. */
#include "jlex.h"

/* entry point */
LUAI_FUNC LClosure *javaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                                  const char *name, int firstchar);

#endif
