/*
** Java Parser → Lua Bytecode  (jparser.c)
** Recursive-descent parser that emits Lua VM instructions via lcode.c
*/

#define LUA_CORE
#include "lprefix.h"
#include <string.h>

#include "lua.h"
#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lzio.h"
#include "llimits.h"

#include "jlex.h"
#include "jparser.h"

/* ---- block control (minimal, mirrors lparser.c) ---- */
typedef struct JBlockCnt {
  struct JBlockCnt *previous;
  lu_byte nactvar;
  lu_byte isloop;
} JBlockCnt;

/* ---- helper macros (mirrors lparser.c internals) ---- */
static void init_exp(expdesc *e, expkind k, int i) {
  e->k = k; e->u.info = i;
  e->t = e->f = NO_JUMP;
}

/* ---- forward declarations ---- */
static void statement(JLexState *ls);
static void expr(JLexState *ls, expdesc *v);
static void block(JLexState *ls);

/* ---- enter / leave block ---- */
static void enterblock(FuncState *fs, JBlockCnt *bl, lu_byte isloop) {
  bl->previous = (JBlockCnt*)fs->bl;
  bl->nactvar = fs->nactvar;
  bl->isloop = isloop;
  fs->bl = (struct BlockCnt*)bl;
}

static void leaveblock(FuncState *fs) {
  JBlockCnt *bl = (JBlockCnt*)fs->bl;
  fs->bl = (struct BlockCnt*)bl->previous;
}

/* ---- advance to next token ---- */
static void next(JLexState *ls) {
  jlex_next(ls);
}

/* ---- require token or error ---- */
static void check(JLexState *ls, int token) {
  if (ls->t.token != token)
    jlex_syntaxerror(ls, luaO_pushfstring(ls->L, "expected '%s'",
      (token < 256) ? luaO_pushfstring(ls->L, "%c", token) : "?"));
}

/* ---- skip semicolons ---- */
static void skip_semicolon(JLexState *ls) {
  if (ls->t.token == ';') next(ls);
}

/* ---- variable name tracking ---- */
static int new_localvar(FuncState *fs, TString *name) {
  Proto *f = fs->f;
  int reg = fs->nactvar;
  /* store name in proto's locvars for later lookup */
  luaM_growvector(fs->ls->L, f->locvars, fs->nlocvars, f->sizelocvars,
                  LocVar, SHRT_MAX, "local variables");
  f->locvars[fs->nlocvars].varname = name;
  f->locvars[fs->nlocvars].startpc = fs->pc;
  fs->nlocvars++;
  fs->nactvar++;
  if (fs->nactvar > fs->freereg)
    fs->freereg = fs->nactvar;
  return reg;
}

/* search active local variables by name; returns register index or -1 */
static int search_local(FuncState *fs, TString *name) {
  int i;
  for (i = cast_int(fs->nlocvars) - 1; i >= 0; i--) {
    if (fs->f->locvars[i].varname == name)
      return i;  /* register = position in locvars */
  }
  return -1;
}

/* ---------- EXPRESSION PARSER ---------- */

/* forward */
static void simpleexpr(JLexState *ls, expdesc *v);
static void subexpr(JLexState *ls, expdesc *v, int limit);

/* binary operator -> lcode.c BinOpr mapping */
static BinOpr getbinopr(int token) {
  switch (token) {
    case '+':  return OPR_ADD;
    case '-':  return OPR_SUB;
    case '*':  return OPR_MUL;
    case '/':  return OPR_DIV;
    case '%':  return OPR_MOD;
    case TK_JAVA_EQ: return OPR_EQ;
    case TK_JAVA_NE: return OPR_NE;
    case '<':  return OPR_LT;
    case '>':  return OPR_GT;
    case TK_JAVA_LE: return OPR_LE;
    case TK_JAVA_GE: return OPR_GE;
    case TK_JAVA_AND: return OPR_AND;
    case TK_JAVA_OR:  return OPR_OR;
    default:   return OPR_NOBINOPR;
  }
}

/* precedence level for binary operators */
static int binop_level(int token) {
  switch (token) {
    case TK_JAVA_OR:  return 1;
    case TK_JAVA_AND: return 2;
    case '|': return 3;
    case '^': case '&': return 4;
    case TK_JAVA_EQ: case TK_JAVA_NE: return 5;
    case '<': case '>': case TK_JAVA_LE: case TK_JAVA_GE: return 6;
    case '+': case '-': return 7;
    case '*': case '/': case '%': return 8;
    default: return 0;
  }
}

/* dispatch expression result to a register */
static void exp2reg(FuncState *fs, expdesc *e, int reg) {
  luaK_dischargevars(fs, e);
  switch (e->k) {
    case VNIL: case VFALSE: case VTRUE: {
      luaK_codeABC(fs, OP_LOADBOOL, reg, e->k == VTRUE, 0);
      break;
    }
    case VK: {
      luaK_codek(fs, reg, e->u.info);
      break;
    }
    case VKFLT: {
      int k = luaK_intK(fs, (lua_Integer)e->u.nval); /* simplified */
      luaK_codeABx(fs, OP_LOADK, reg, k);
      break;
    }
    case VKINT: {
      int k = luaK_intK(fs, e->u.ival);
      luaK_codeABx(fs, OP_LOADK, reg, (unsigned int)k);
      break;
    }
    case VRELOCABLE: {
      Instruction *pc = &fs->f->code[e->u.info];
      SETARG_A(*pc, reg);
      break;
    }
    case VNONRELOC: {
      if (reg != e->u.info)
        luaK_codeABC(fs, OP_MOVE, reg, e->u.info, 0);
      break;
    }
    default: break;
  }
  e->u.info = reg;
  e->k = VNONRELOC;
  e->t = e->f = NO_JUMP;
}

/* simple expressions */
static void simpleexpr(JLexState *ls, expdesc *v) {
  switch (ls->t.token) {
    case TK_JAVA_INTLIT: {
      init_exp(v, VKINT, 0);
      v->u.ival = ls->t.seminfo.i;
      next(ls);
      break;
    }
    case TK_JAVA_FLOATLIT: {
      init_exp(v, VKFLT, 0);
      v->u.nval = ls->t.seminfo.r;
      next(ls);
      break;
    }
    case TK_JAVA_STRLIT: {
      TString *ts = ls->t.seminfo.ts;
      init_exp(v, VK, luaK_stringK(ls->fs, ts));
      next(ls);
      break;
    }
    case TK_JAVA_TRUE: {
      init_exp(v, VTRUE, 0);
      next(ls);
      break;
    }
    case TK_JAVA_FALSE: {
      init_exp(v, VFALSE, 0);
      next(ls);
      break;
    }
    case TK_JAVA_NULL: {
      init_exp(v, VNIL, 0);
      next(ls);
      break;
    }
    case TK_JAVA_NAME: {
      /* Variable / field-chain / method-call.
       * First check if it's a local variable, otherwise lookup
       * from _ENV (OP_GETTABUP). Then chain .field accesses
       * with OP_GETTABLE, and handle (args) calls. */
      TString *name = ls->t.seminfo.ts;
      next(ls);

      /* Step 1: local or global? */
      int loc = search_local(ls->fs, name);
      if (loc >= 0) {
        /* local variable — already in register */
        init_exp(v, VNONRELOC, loc);
      } else {
        /* global — lookup from _ENV into a register */
        int reg = ls->fs->freereg;
        int k = luaK_stringK(ls->fs, name);
        luaK_codeABC(ls->fs, OP_GETTABUP, reg, 0, (unsigned int)(k | BITRK));
        ls->fs->freereg = reg + 1;
        init_exp(v, VNONRELOC, reg);
      }

      /* Step 2: chain .field accesses using GETTABLE on the object */
      while (ls->t.token == '.') {
        next(ls);
        if (ls->t.token != TK_JAVA_NAME)
          jlex_syntaxerror(ls, "expected identifier after '.'");
        TString *field = ls->t.seminfo.ts;
        next(ls);

        luaK_exp2nextreg(ls->fs, v);
        int tabreg = v->u.info;
        int fk = luaK_stringK(ls->fs, field);
        luaK_codeABC(ls->fs, OP_GETTABLE, tabreg, tabreg, fk | BITRK);
        init_exp(v, VNONRELOC, tabreg);
      }

      /* Step 3: handle function call if followed by (args) */
      if (ls->t.token == '(') {
        next(ls);
        int base = v->u.info;
        int nargs = 0;
        if (ls->t.token != ')') {
          expdesc arg;
          expr(ls, &arg);
          exp2reg(ls->fs, &arg, base + 1);  /* 1st arg at base+1 */
          nargs = 1;
          while (ls->t.token == ',') {
            next(ls);
            expr(ls, &arg);
            exp2reg(ls->fs, &arg, base + nargs + 1);
            nargs++;
          }
        }
        check(ls, ')'); next(ls);
        luaK_codeABC(ls->fs, OP_CALL, base, nargs + 1, 2);
        ls->fs->freereg = base;  /* result in base */
        init_exp(v, VRELOCABLE, ls->fs->pc - 1);
      }
      break;
    }
    case '(': {
      next(ls);
      expr(ls, v);
      check(ls, ')'); next(ls);
      break;
    }
    case '!': {
      next(ls);
      simpleexpr(ls, v);
      luaK_prefix(ls->fs, OPR_NOT, v, ls->linenumber);
      break;
    }
    case '-': {
      next(ls);
      simpleexpr(ls, v);
      luaK_prefix(ls->fs, OPR_MINUS, v, ls->linenumber);
      break;
    }
    case TK_JAVA_NEW: {
      next(ls);
      /* new ClassName() → empty table */
      if (ls->t.token != TK_JAVA_NAME)
        jlex_syntaxerror(ls, "expected class name after 'new'");
      next(ls); /* skip class name */
      if (ls->t.token == '(') {
        next(ls);
        if (ls->t.token != ')')
          jlex_syntaxerror(ls, "constructor args not supported yet");
      }
      check(ls, ')'); next(ls);
      int reg = ls->fs->freereg;
      luaK_codeABC(ls->fs, OP_NEWTABLE, reg, 0, 0);
      ls->fs->freereg = reg + 1;
      init_exp(v, VNONRELOC, reg);
      break;
    }
    case TK_JAVA_THIS: {
      /* "this" → reference to first upvalue (the class table) */
      next(ls);
      int reg = ls->fs->freereg;
      luaK_codeABC(ls->fs, OP_GETUPVAL, reg, 0, 0);
      ls->fs->freereg = reg + 1;
      init_exp(v, VNONRELOC, reg);
      break;
    }
    default:
      jlex_syntaxerror(ls, "unexpected token in expression");
  }
}

/* subexpr: precedence climbing */
static void subexpr(JLexState *ls, expdesc *v, int limit) {
  BinOpr op;
  simpleexpr(ls, v);

  /* chain .name and .method() access */
  while (ls->t.token == '.' && (binop_level(ls->t.token) > limit || limit == 0)) {
    next(ls);
    if (ls->t.token != TK_JAVA_NAME)
      jlex_syntaxerror(ls, "expected identifier after '.'");
    TString *field = ls->t.seminfo.ts;
    next(ls);

    /* get field */
    luaK_exp2nextreg(ls->fs, v);
    int reg = v->u.info;
    int k = luaK_stringK(ls->fs, field);
    luaK_codeABC(ls->fs, OP_GETTABLE, reg, reg, k + (1 << 8)); /* RK */
    /* method call? (self-call: R(reg) = method, R(reg+1) = self) */
    if (ls->t.token == '(') {
      next(ls);
      luaK_codeABC(ls->fs, OP_MOVE, reg + 1, reg, 0);
      int nargs = 1;  /* self */
      if (ls->t.token != ')') {
        expdesc arg;
        expr(ls, &arg);
        exp2reg(ls->fs, &arg, reg + 2);  /* 1st user arg at reg+2 */
        nargs++;
        while (ls->t.token == ',') {
          next(ls);
          expr(ls, &arg);
          exp2reg(ls->fs, &arg, reg + nargs + 1);
          nargs++;
        }
      }
      check(ls, ')'); next(ls);
      luaK_codeABC(ls->fs, OP_CALL, reg, nargs + 1, 2);
      ls->fs->freereg = reg;
      init_exp(v, VRELOCABLE, ls->fs->pc - 1);
    } else {
      init_exp(v, VNONRELOC, reg);
    }
  }

  /* binary operators */
  while ((op = getbinopr(ls->t.token)) != OPR_NOBINOPR &&
         binop_level(ls->t.token) > limit) {
    int thislevel = binop_level(ls->t.token);
    next(ls);
    luaK_infix(ls->fs, op, v);          /* left operand in v */
    { expdesc v2;
      subexpr(ls, &v2, thislevel);       /* right operand in v2 */
      luaK_posfix(ls->fs, op, v, &v2, ls->linenumber); }
  }
}

static void expr(JLexState *ls, expdesc *v) {
  subexpr(ls, v, 0);
}

/* ---------- STATEMENT PARSER ---------- */

/* forward */
static void statement(JLexState *ls);

/* block: { statement* } */
static void block(JLexState *ls) {
  check(ls, '{'); next(ls);
  while (ls->t.token != '}' && ls->t.token != TK_JAVA_EOS)
    statement(ls);
  check(ls, '}'); next(ls);
}

/* variable declaration */
static void var_declaration(JLexState *ls) {
  /* skip type */
  if (ls->t.token == TK_JAVA_INT || ls->t.token == TK_JAVA_STRING ||
      ls->t.token == TK_JAVA_VOID || ls->t.token == TK_JAVA_BOOLEAN) {
    next(ls);
  }
  /* skip modifiers like static, public, private */
  while (ls->t.token == TK_JAVA_STATIC || ls->t.token == TK_JAVA_PUBLIC ||
         ls->t.token == TK_JAVA_PRIVATE || ls->t.token == TK_JAVA_FINAL) {
    next(ls);
    /* re-check type after modifiers */
    if (ls->t.token == TK_JAVA_INT || ls->t.token == TK_JAVA_STRING ||
        ls->t.token == TK_JAVA_VOID || ls->t.token == TK_JAVA_BOOLEAN) {
      next(ls);
    }
  }

  if (ls->t.token == TK_JAVA_NAME) {
    TString *name = ls->t.seminfo.ts;
    next(ls);

    /* skip array brackets */
    while (ls->t.token == '[') { next(ls); check(ls, ']'); next(ls); }

    int reg;
    if (ls->t.token == '=') {
      next(ls);
      expdesc v;
      expr(ls, &v);
      reg = new_localvar(ls->fs, name);
      exp2reg(ls->fs, &v, reg);
    } else {
      reg = new_localvar(ls->fs, name);
      luaK_nil(ls->fs, reg, 1);
    }
    /* allow multiple vars: int a, b; but simplify */
    while (ls->t.token == ',') {
      next(ls);
      if (ls->t.token != TK_JAVA_NAME) break;
      name = ls->t.seminfo.ts; next(ls);
      while (ls->t.token == '[') { next(ls); check(ls, ']'); next(ls); }
      reg = new_localvar(ls->fs, name);
      luaK_nil(ls->fs, reg, 1);
    }
    skip_semicolon(ls);
  }
}

/* if statement */
static void if_statement(JLexState *ls) {
  next(ls); /* skip 'if' */
  check(ls, '('); next(ls);

  expdesc cond;
  expr(ls, &cond);
  luaK_goiftrue(ls->fs, &cond);
  int j_false = cond.f;

  check(ls, ')'); next(ls);

  block(ls);

  if (ls->t.token == TK_JAVA_ELSE) {
    next(ls);
    int j_end = luaK_jump(ls->fs);
    luaK_patchtohere(ls->fs, j_false);
    /* else if? */
    if (ls->t.token == TK_JAVA_IF) {
      if_statement(ls);
    } else {
      block(ls);
    }
    luaK_patchtohere(ls->fs, j_end);
  } else {
    luaK_patchtohere(ls->fs, j_false);
  }
}

/* while statement */
static void while_statement(JLexState *ls) {
  next(ls); /* skip 'while' */
  check(ls, '('); next(ls);

  int loop_pc = ls->fs->pc;

  expdesc cond;
  expr(ls, &cond);
  luaK_goiftrue(ls->fs, &cond);
  int j_false = cond.f;

  check(ls, ')'); next(ls);

  JBlockCnt bl;
  enterblock(ls->fs, &bl, 1);

  block(ls);

  /* jump back to condition */
  luaK_codeAsBx(ls->fs, OP_JMP, 0, loop_pc - ls->fs->pc - 1);
  luaK_patchtohere(ls->fs, j_false);

  leaveblock(ls->fs);
}

/* for statement (numeric only) */
static void for_statement(JLexState *ls) {
  next(ls); /* skip 'for' */
  check(ls, '('); next(ls);

  /* init: int i = expr */
  if (ls->t.token == TK_JAVA_INT) next(ls);
  if (ls->t.token != TK_JAVA_NAME)
    jlex_syntaxerror(ls, "expected variable name in for");
  TString *varname = ls->t.seminfo.ts;
  next(ls);
  check(ls, '='); next(ls);

  expdesc init_e;
  expr(ls, &init_e);
  int varreg = new_localvar(ls->fs, varname);
  exp2reg(ls->fs, &init_e, varreg);

  check(ls, ';'); next(ls);

  /* condition: i < expr */
  int condreg = ls->fs->freereg;
  expdesc cond;
  expr(ls, &cond);
  luaK_exp2nextreg(ls->fs, &cond);
  int limitreg = cond.u.info;
  ls->fs->freereg = condreg + 2;

  check(ls, ';'); next(ls);

  /* step: consume step expression (always ++ / i++ for now) */
  if (ls->t.token != ')') {
    if (ls->t.token == TK_JAVA_INC || ls->t.token == TK_JAVA_DEC) {
      next(ls);
    } else if (ls->t.token == TK_JAVA_NAME) {
      next(ls);
      if (ls->t.token == TK_JAVA_INC || ls->t.token == TK_JAVA_DEC)
        next(ls);
      else {
        while (ls->t.token != ')' && ls->t.token != TK_JAVA_EOS)
          next(ls);
      }
    }
  }
  check(ls, ')'); next(ls);

  /* Emit FORPREP / FORLOOP (Lua numeric for) */
  JBlockCnt bl;
  enterblock(ls->fs, &bl, 1);

  /* store limit in varreg+1, step in varreg+2 */
  ls->fs->freereg = varreg + 3;
  luaK_codeABC(ls->fs, OP_MOVE, varreg + 1, limitreg, 0);
  int kstep = luaK_intK(ls->fs, 1);
  luaK_codeABx(ls->fs, OP_LOADK, varreg + 2, (unsigned int)kstep);

  int prep_pc = ls->fs->pc;
  luaK_codeAsBx(ls->fs, OP_FORPREP, varreg, 0);

  block(ls);

  /* FORLOOP jumps back */
  luaK_codeAsBx(ls->fs, OP_FORLOOP, varreg, prep_pc - ls->fs->pc - 1);
  /* fix FORPREP */
  SETARG_sBx(ls->fs->f->code[prep_pc], ls->fs->pc - prep_pc - 1);

  leaveblock(ls->fs);
}

/* return statement */
static void return_statement(JLexState *ls) {
  next(ls); /* skip 'return' */
  if (ls->t.token == ';' || ls->t.token == '}') {
    luaK_ret(ls->fs, 0, 0);
    skip_semicolon(ls);
  } else {
    expdesc v;
    expr(ls, &v);
    luaK_exp2nextreg(ls->fs, &v);
    luaK_ret(ls->fs, v.u.info, 1);
    skip_semicolon(ls);
  }
}

/* expression statement */
static void expr_statement(JLexState *ls) {
  expdesc v;
  expr(ls, &v);
  skip_semicolon(ls);
}

/* single statement dispatch */
static void statement(JLexState *ls) {
  int first = ls->t.token;

  /* check for type + name pattern (variable declaration) */
  if (first == TK_JAVA_INT || first == TK_JAVA_STRING ||
      first == TK_JAVA_VOID || first == TK_JAVA_BOOLEAN ||
      first == TK_JAVA_PUBLIC || first == TK_JAVA_PRIVATE ||
      first == TK_JAVA_STATIC || first == TK_JAVA_FINAL ||
      first == TK_JAVA_PROTECTED) {
    int lookahead = jlex_lookahead(ls);
    if (lookahead == TK_JAVA_NAME ||
        lookahead == TK_JAVA_INT || lookahead == TK_JAVA_STRING ||
        lookahead == TK_JAVA_STATIC || lookahead == TK_JAVA_PUBLIC ||
        lookahead == TK_JAVA_PRIVATE) {
      var_declaration(ls);
      return;
    }
  }

  switch (first) {
    case '{':  block(ls); break;
    case TK_JAVA_IF:    if_statement(ls); break;
    case TK_JAVA_WHILE: while_statement(ls); break;
    case TK_JAVA_FOR:   for_statement(ls); break;
    case TK_JAVA_RETURN: return_statement(ls); break;
    case TK_JAVA_INTLIT:
    case TK_JAVA_FLOATLIT:
    case TK_JAVA_STRLIT:
    case TK_JAVA_NEW:
    case TK_JAVA_NAME:
    case '(': case '!': case '-':
    case TK_JAVA_TRUE: case TK_JAVA_FALSE:
    case TK_JAVA_NULL:
      expr_statement(ls); break;
    case ';': next(ls); break;  /* empty statement */
    default:
      jlex_syntaxerror(ls, "unexpected statement");
  }
}

/* ---------- METHOD / CLASS PARSER ---------- */

/* parse a Java method: public static int add(int a, int b) { ... } */
static void method_definition(JLexState *ls, FuncState *fs, int class_reg) {
  (void)fs;  /* fs == ls->fs after restore; used for clarity */
  /* skip modifiers (public, static, etc.) */
  while (ls->t.token == TK_JAVA_PUBLIC || ls->t.token == TK_JAVA_PRIVATE ||
         ls->t.token == TK_JAVA_PROTECTED || ls->t.token == TK_JAVA_STATIC ||
         ls->t.token == TK_JAVA_FINAL) {
    next(ls);
  }

  /* skip return type */
  if (ls->t.token == TK_JAVA_VOID || ls->t.token == TK_JAVA_INT ||
      ls->t.token == TK_JAVA_STRING || ls->t.token == TK_JAVA_BOOLEAN) {
    next(ls);
  }

  /* method name */
  if (ls->t.token != TK_JAVA_NAME)
    jlex_syntaxerror(ls, "expected method name");
  TString *method_name = ls->t.seminfo.ts;
  next(ls);

  /* parameters */
  check(ls, '('); next(ls);
  while (ls->t.token != ')') {
    /* skip param type */
    if (ls->t.token == TK_JAVA_INT || ls->t.token == TK_JAVA_STRING ||
        ls->t.token == TK_JAVA_BOOLEAN || ls->t.token == TK_JAVA_NAME) {
      next(ls);
    }
    /* skip param name */
    while (ls->t.token == '[') { next(ls); check(ls, ']'); next(ls); }
    if (ls->t.token == TK_JAVA_NAME) next(ls);
    else if (ls->t.token == '.') { /* qualified type like String[] */
      while (ls->t.token == '.') {
        next(ls);
        if (ls->t.token == TK_JAVA_NAME) next(ls);
      }
      while (ls->t.token == '[') { next(ls); check(ls, ']'); next(ls); }
      if (ls->t.token == TK_JAVA_NAME) next(ls);
    }
    if (ls->t.token == ',') next(ls);
  }
  next(ls); /* skip ) */

  /* create nested FuncState for the method body */
  FuncState method_fs;
  struct LexState method_mini_ls;  /* lcode.c needs fs->ls->L */
  Proto *proto = luaF_newproto(ls->L);
  proto->source = ls->source;
  luaC_objbarrier(ls->L, proto, proto->source);
  luaC_objbarrier(ls->L, ls->fs->f, proto);

  method_fs.f = proto;
  method_fs.prev = ls->fs;
  method_mini_ls.L = ls->L;
  method_mini_ls.h = ls->h;  /* share parent constant cache */
  method_fs.ls = &method_mini_ls;
  method_fs.bl = NULL;
  method_fs.pc = 0;
  method_fs.lasttarget = 0;
  method_fs.jpc = NO_JUMP;
  method_fs.nk = 0;
  method_fs.np = 0;
  method_fs.firstlocal = 0;
  method_fs.nlocvars = 0;
  method_fs.nactvar = 0;
  method_fs.nups = 0;
  method_fs.freereg = 0;

  method_fs.f->maxstacksize = 2;
  method_fs.f->is_vararg = 0;
  method_fs.f->numparams = 0;

  /* Give the method an _ENV upvalue (captured from enclosing closure).
   * This allows the method body to access globals like System. */
  {
    Proto *mf = method_fs.f;
    luaM_growvector(ls->L, mf->upvalues, method_fs.nups, mf->sizeupvalues,
                    Upvaldesc, MAXUPVAL, "upvalues");
    int oldsize = mf->sizeupvalues;
    while (oldsize < (int)mf->sizeupvalues)
      mf->upvalues[oldsize++].name = NULL;
    mf->upvalues[method_fs.nups].instack = 0;  /* from enclosing closure */
    mf->upvalues[method_fs.nups].idx = 0;       /* parent upvalue[0] = _ENV */
    mf->upvalues[method_fs.nups].name = ls->envn;
    luaC_objbarrier(ls->L, mf, ls->envn);
    method_fs.nups++;
    mf->sizeupvalues = method_fs.nups;
  }

  /* save parent context, set method as current */
  FuncState *saved_fs = ls->fs;
  ls->fs = &method_fs;

  JBlockCnt bl;
  enterblock(ls->fs, &bl, 0);

  /* parse method body */
  block(ls);

  /* final return (if not already present) */
  luaK_ret(ls->fs, 0, 0);
  leaveblock(ls->fs);

  /* finalize method proto */
  method_fs.f->sizecode = method_fs.pc;
  method_fs.f->sizek = method_fs.nk;
  method_fs.f->sizep = method_fs.np;
  method_fs.f->sizelocvars = method_fs.nlocvars;
  method_fs.f->sizeupvalues = method_fs.nups;
  method_fs.f->linedefined = 0;
  method_fs.f->lastlinedefined = 0;
  method_fs.f->maxstacksize = method_fs.freereg;

  /* register this proto as a sub-function in parent */
  int proto_idx = saved_fs->np;
  luaM_growvector(ls->L, saved_fs->f->p, proto_idx, saved_fs->np,
                  Proto *, MAXARG_Bx, "functions");
  saved_fs->f->p[proto_idx] = proto;
  saved_fs->np++;

  /* restore parent context */
  ls->fs = saved_fs;

  /* generate OP_CLOSURE in parent to create the closure */
  int reg = ls->fs->freereg;
  luaK_codeABx(ls->fs, OP_CLOSURE, reg, (unsigned int)proto_idx);
  ls->fs->freereg = reg + 1;

  /* store in class table: class_reg[method_name] = closure */
  /* SETTABLE A B C: R(A)[RK(B)] := RK(C); B=k|BITRK means K(k) */
  int mk = luaK_stringK(ls->fs, method_name);
  luaK_codeABC(ls->fs, OP_SETTABLE, class_reg, mk | BITRK, reg);
}

/* parse a Java class: class ClassName { methods... } */
static void class_definition(JLexState *ls, FuncState *fs) {
  next(ls); /* skip 'class' */

  if (ls->t.token != TK_JAVA_NAME)
    jlex_syntaxerror(ls, "expected class name");
  /* TString *class_name = ls->t.seminfo.ts; */
  next(ls);

  /* create class table */
  int class_reg = fs->freereg;
  luaK_codeABC(fs, OP_NEWTABLE, class_reg, 0, 0);
  fs->freereg = class_reg + 1;

  check(ls, '{'); next(ls);

  /* parse methods */
  while (ls->t.token != '}' && ls->t.token != TK_JAVA_EOS) {
    method_definition(ls, fs, class_reg);
  }
  check(ls, '}'); next(ls);

  /* return the class table */
  luaK_ret(fs, class_reg, 1);
}

/* ---------- ENTRY: main function ---------- */

static void javamain(JLexState *ls, FuncState *fs) {
  JBlockCnt bl;
  enterblock(fs, &bl, 0);

  fs->f->is_vararg = 0;

  /* Create _ENV upvalue (mirrors Lua's mainfunc).
   * OP_GETTABUP needs this to access the global table. */
  {
    Proto *f = fs->f;
    int oldsize = f->sizeupvalues;
    luaM_growvector(ls->L, f->upvalues, fs->nups, f->sizeupvalues,
                    Upvaldesc, MAXUPVAL, "upvalues");
    while (oldsize < (int)f->sizeupvalues)
      f->upvalues[oldsize++].name = NULL;
    f->upvalues[fs->nups].instack = 1;
    f->upvalues[fs->nups].idx = 0;
    f->upvalues[fs->nups].name = ls->envn;
    luaC_objbarrier(ls->L, f, ls->envn);
    fs->nups++;
    f->sizeupvalues = fs->nups;
  }

  next(ls); /* read first token */

  /* parse compilation unit: class definitions */
  while (ls->t.token != TK_JAVA_EOS) {
    if (ls->t.token == TK_JAVA_CLASS) {
      class_definition(ls, fs);
    } else if (ls->t.token == TK_JAVA_PUBLIC) {
      /* public class ... */
      next(ls);
      if (ls->t.token == TK_JAVA_CLASS) {
        class_definition(ls, fs);
      } else {
        jlex_syntaxerror(ls, "expected 'class' after 'public'");
      }
    } else {
      jlex_syntaxerror(ls, "expected class definition");
    }
  }

  leaveblock(fs);
}

/* ---------- TOP-LEVEL ENTRY POINT ---------- */

LClosure *javaY_parser(lua_State *L, ZIO *z, Mbuffer *buff,
                       const char *name, int firstchar) {
  JLexState lexstate;
  FuncState funcstate;

  /* 1. create main closure */
  LClosure *cl = luaF_newLclosure(L, 1);
  setclLvalue(L, L->top, cl);
  luaD_inctop(L);

  /* 2. allocate + anchor proto */
  funcstate.f = cl->p = luaF_newproto(L);
  luaC_objbarrier(L, cl, cl->p);
  funcstate.f->source = luaS_new(L, name);

  /* 3. init FuncState */
  funcstate.prev = NULL;
  funcstate.ls = NULL;
  funcstate.bl = NULL;
  funcstate.pc = 0;
  funcstate.lasttarget = 0;
  funcstate.jpc = NO_JUMP;
  funcstate.nk = 0;
  funcstate.np = 0;
  funcstate.firstlocal = 0;
  funcstate.nlocvars = 0;
  funcstate.nactvar = 0;
  funcstate.nups = 0;
  funcstate.freereg = 0;
  funcstate.f->maxstacksize = 2;
  funcstate.f->is_vararg = 0;
  funcstate.f->numparams = 0;

  /* 4. create scanner/constant-cache table */
  lexstate.h = luaH_new(L);
  sethvalue(L, L->top, lexstate.h);
  luaD_inctop(L);

  /* 5. init lexer */
  lexstate.buff = buff;
  lexstate.fs = &funcstate;
  lexstate.L = L;
  lexstate.z = z;
  lexstate.t.token = 0;
  lexstate.current = firstchar;
  lexstate.lookahead.token = TK_JAVA_EOS;
  lexstate.linenumber = 1;
  lexstate.lastline = 1;
  lexstate.source = funcstate.f->source;
  lexstate.envn = luaS_newliteral(L, "_ENV");
  luaZ_resizebuffer(L, buff, LUA_MINBUFFER);

  /* 6. set up mini_ls for lcode.c (needs fs->ls with L + h) and parse */
  {
    struct LexState mini_ls;
    mini_ls.L = L;
    mini_ls.h = lexstate.h;
    funcstate.ls = &mini_ls;
    javamain(&lexstate, &funcstate);
  }

  /* 7. finalize proto */
  funcstate.f->sizecode = funcstate.pc;
  funcstate.f->sizek = funcstate.nk;
  funcstate.f->sizep = funcstate.np;
  funcstate.f->sizelocvars = funcstate.nlocvars;
  funcstate.f->sizeupvalues = funcstate.nups;
  funcstate.f->linedefined = 0;
  funcstate.f->lastlinedefined = 0;
  funcstate.f->maxstacksize = funcstate.freereg > funcstate.f->maxstacksize
                                ? funcstate.freereg : funcstate.f->maxstacksize;

  L->top--;  /* pop scanner's string table */
  return cl;
}
