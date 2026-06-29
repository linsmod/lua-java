/*
** Java Lexer for Lua VM
*/

#ifndef jlex_h
#define jlex_h

#include "lobject.h"
#include "lzio.h"
#include "lparser.h"

/* ---- multi-char operator tokens (256-299) ---- */
enum {
  TK_JAVA_EQ = 256, TK_JAVA_NE, TK_JAVA_LE, TK_JAVA_GE,
  TK_JAVA_AND, TK_JAVA_OR, TK_JAVA_INC, TK_JAVA_DEC,
  TK_JAVA_PLUSEQ, TK_JAVA_MINEQ, TK_JAVA_MULEQ, TK_JAVA_DIVEQ, TK_JAVA_MODEQ
};

/* ---- keyword tokens (300+) ---- */
enum {
  TK_JAVA_CLASS = 300, TK_JAVA_PUBLIC, TK_JAVA_PRIVATE, TK_JAVA_PROTECTED,
  TK_JAVA_STATIC, TK_JAVA_VOID, TK_JAVA_INT, TK_JAVA_STRING,
  TK_JAVA_NEW, TK_JAVA_RETURN, TK_JAVA_IF, TK_JAVA_ELSE,
  TK_JAVA_FOR, TK_JAVA_WHILE, TK_JAVA_TRUE, TK_JAVA_FALSE,
  TK_JAVA_NULL, TK_JAVA_THIS,
  TK_JAVA_BOOLEAN, TK_JAVA_FINAL,
  /* new keywords */
  TK_JAVA_PACKAGE, TK_JAVA_IMPORT, TK_JAVA_CHAR, TK_JAVA_DOUBLE,
  TK_JAVA_ENUM, TK_JAVA_SWITCH, TK_JAVA_CASE, TK_JAVA_DEFAULT,
  TK_JAVA_BREAK, TK_JAVA_DO, TK_JAVA_TRY, TK_JAVA_CATCH,
  TK_JAVA_FINALLY, TK_JAVA_THROW, TK_JAVA_THROWS,
  TK_JAVA_IMPLEMENTS, TK_JAVA_EXTENDS, TK_JAVA_INTERFACE,
  TK_JAVA_INTLIT, TK_JAVA_FLOATLIT, TK_JAVA_STRLIT, TK_JAVA_NAME,
  TK_JAVA_EOS
};

typedef struct JavaSemInfo {
  lua_Number r;
  lua_Integer i;
  TString *ts;
} JavaSemInfo;

typedef struct JavaToken {
  int token;
  JavaSemInfo seminfo;
} JavaToken;

typedef struct JLexState {
  int current;
  int linenumber;
  int lastline;
  JavaToken t;
  JavaToken lookahead;
  struct FuncState *fs;
  lua_State *L;
  ZIO *z;
  Mbuffer *buff;
  Table *h;
  TString *source;
  TString *envn;
} JLexState;

LUAI_FUNC void  jlex_init      (lua_State *L);
LUAI_FUNC void  jlex_setinput  (lua_State *L, JLexState *ls, ZIO *z,
                                TString *source, int firstchar);
LUAI_FUNC void  jlex_next      (JLexState *ls);
LUAI_FUNC int   jlex_lookahead (JLexState *ls);
LUAI_FUNC TString *jlex_newstring (JLexState *ls, const char *str, size_t l);
LUAI_FUNC l_noret jlex_syntaxerror (JLexState *ls, const char *msg);

#endif
