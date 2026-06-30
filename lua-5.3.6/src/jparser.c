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
  fs->nactvar = bl->nactvar;
  /* Release all temporary registers allocated inside the block.
   * Local variables are deactivated (via nactvar) and their stack
   * slots become free for reuse.  freereg must reflect this
   * to prevent register pollution across block boundaries. */
  fs->freereg = fs->nactvar;
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
  /* Ensure locvars array has room at index 'reg'.
   * Use 'reg' as the index so that search_local's assumption
   * (active locals: locvars[i] <=> register i) holds even after
   * leaveblock creates gaps in the array. */
  if (reg >= f->sizelocvars)
    luaM_growvector(fs->ls->L, f->locvars, fs->nlocvars, f->sizelocvars,
                    LocVar, SHRT_MAX, "local variables");
  f->locvars[reg].varname = name;
  f->locvars[reg].startpc = fs->pc;
  fs->nactvar++;
  /* Only grow nlocvars if this register was previously unused */
  if (reg >= fs->nlocvars)
    fs->nlocvars = reg + 1;
  if (fs->nactvar > fs->freereg)
    fs->freereg = fs->nactvar;
  return reg;
}

/* search active local variables by name; returns register index or -1 */
static int search_local(FuncState *fs, TString *name) {
  int i;
  for (i = cast_int(fs->nactvar) - 1; i >= 0; i--) {
    if (fs->f->locvars[i].varname == name)
      return i;  /* for active locals, locvars index == register number */
  }
  return -1;
}

/* ---------- EXPRESSION PARSER ---------- */

static void simpleexpr(JLexState *ls, expdesc *v);
static void subexpr(JLexState *ls, expdesc *v, int limit);

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
  if (e->k == VJMP || e->t != e->f) {
    int src = luaK_exp2anyreg(fs, e);
    if (src != reg)
      luaK_codeABC(fs, OP_MOVE, reg, src, 0);
    e->u.info = reg;
    e->k = VNONRELOC;
    e->t = e->f = NO_JUMP;
    return;
  }
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
      int k = luaK_numberK(fs, e->u.nval);
      luaK_codek(fs, reg, k);
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

/*
** Helper: push a string constant to register reg
*/
static void emit_string_const(FuncState *fs, const char *s, int reg) {
  TString *ts = luaS_new(fs->ls->L, s);
  int k = luaK_stringK(fs, ts);
  luaK_codek(fs, reg, k);
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
      TString *name = ls->t.seminfo.ts;
      next(ls);

      /* All .chain and () call handling is deferred to subexpr().
       * Here we just classify the bare name. */
      {
        /* simple name: local, instance field, or global */
        int loc = search_local(ls->fs, name);
        if (loc >= 0) {
          init_exp(v, VNONRELOC, loc);
        } else {
          /* Check if 'this' is a local (inside instance method or constructor).
           * If so AND not followed by '(' or '.' (method call / qualified name),
           * treat unknown bare names as 'this.field' — instance field access.
           * Names followed by '(' or '.' resolve via _ENV (static methods/globals). */
          int this_loc = search_local(ls->fs, luaS_newliteral(ls->L, "this"));
          if (this_loc >= 0 && ls->t.token != '(' && ls->t.token != '.') {
            int k = luaK_stringK(ls->fs, name);
            int reg = ls->fs->freereg;
            luaK_codeABC(ls->fs, OP_GETTABLE, reg, this_loc, (unsigned int)(k | BITRK));
            ls->fs->freereg = reg + 1;
            init_exp(v, VNONRELOC, reg);
          } else {
            /* Deferred global lookup: emit GETTABUP with A=0, mark as VRELOCABLE
             * so exp2reg / luaK_exp2nextreg can patch A to the right register later. */
            int k = luaK_stringK(ls->fs, name);
            int instr = luaK_codeABC(ls->fs, OP_GETTABUP, 0, 0, (unsigned int)(k | BITRK));
            init_exp(v, VRELOCABLE, instr);
          }
        }

        /* NOTE: .chain and function call are handled by subexpr()
         * after simpleexpr() returns. Do NOT consume them here. */
      }
      break;
    }
    case '(': {
      next(ls);
      /* Distinguish type-cast (int)expr from parenthesized (expr).
       * Type-casts: token after '(' is a Java type keyword or a class name
       * that is followed by ')' (making it a cast target). */
      if (ls->t.token == TK_JAVA_INT || ls->t.token == TK_JAVA_DOUBLE ||
          ls->t.token == TK_JAVA_BOOLEAN || ls->t.token == TK_JAVA_CHAR ||
          ls->t.token == TK_JAVA_STRING || ls->t.token == TK_JAVA_VOID) {
        /* Primitive type cast: just skip the type and parse the expression */
        next(ls);  /* skip type keyword */
        /* Skip any post-type modifiers: e.g. (int[]) */
        while (ls->t.token == '[') {
          next(ls);
          check(ls, ']'); next(ls);
        }
        check(ls, ')'); next(ls);
        /* In Lua, all values are dynamically typed — cast is a no-op.
         * Just parse the expression naturally. */
        expr(ls, v);
      } else {
        /* Parenthesized expression */
        expr(ls, v);
        check(ls, ')'); next(ls);
      }
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
      /* new ClassName(args...) */
      if (ls->t.token != TK_JAVA_NAME)
        jlex_syntaxerror(ls, "expected class name after 'new'");
      TString *cname = ls->t.seminfo.ts;
      next(ls);

      /* skip generics: new ArrayList<>() */
      if (ls->t.token == '<') {
        int depth = 1;
        while (depth > 0 && ls->t.token != TK_JAVA_EOS) {
          next(ls);
          if (ls->t.token == '<') depth++;
          else if (ls->t.token == '>') depth--;
        }
        if (ls->t.token == '>') next(ls);
      }

      /* create empty table for the object */
      int reg = ls->fs->freereg;
      luaK_codeABC(ls->fs, OP_NEWTABLE, reg, 0, 0);
      ls->fs->freereg = reg + 1;

      /* parse constructor args first (at reg+1..reg+nargs) */
      check(ls, '('); next(ls);
      int nargs = 0;
      if (ls->t.token != ')') {
        expdesc arg;
        expr(ls, &arg);
        exp2reg(ls->fs, &arg, reg + 1);
        nargs = 1;
        while (ls->t.token == ',') {
          next(ls);
          expr(ls, &arg);
          exp2reg(ls->fs, &arg, reg + nargs + 1);
          nargs++;
        }
      }
      check(ls, ')'); next(ls);

      /* Reserve space for ctor call setup:
       *   ctor_reg = reg + nargs + 1  (one past args)
       *   self     = ctor_reg + 1
       *   args     = ctor_reg + 2 .. + 2 + nargs - 1
       * Then metatable temps go above that:
       *   cls_reg  = reg + 2*nargs + 3  (or just after ctor args)
       * We'll compute base of temp area after nargs is known.
       */
      {
        int ctor_reg = reg + nargs + 1;
        int tmp_base = ctor_reg + nargs + 2;  /* after self + args */
        ls->fs->freereg = tmp_base + 4;       /* need 4 temps for metatable */

        /* Look up class table and constructor */
        int k = luaK_stringK(ls->fs, cname);
        luaK_codeABC(ls->fs, OP_GETTABUP, ctor_reg, 0, (unsigned int)(k | BITRK));
        luaK_codeABC(ls->fs, OP_GETTABLE, ctor_reg, ctor_reg, k | BITRK);

        /* Relocate object (self) and args to be adjacent to ctor_reg */
        luaK_codeABC(ls->fs, OP_MOVE, ctor_reg + 1, reg, 0);  /* self */
        { int i;
          for (i = 0; i < nargs; i++) {
            luaK_codeABC(ls->fs, OP_MOVE, ctor_reg + 2 + i, reg + 1 + i, 0);
          }
        }

        /* Set up metatable on the new object BEFORE ctor call:
         *   local mt = {__index = _ENV[cname]}
         *   _setmetatable(obj, mt)
         */
        int cls_reg = tmp_base + 0;
        int mt_reg  = tmp_base + 1;
        int smt_reg = tmp_base + 2;

        /* R(cls_reg) = _ENV[cname] — get the class table */
        int kc = luaK_stringK(ls->fs, cname);
        luaK_codeABC(ls->fs, OP_GETTABUP, cls_reg, 0, (unsigned int)(kc | BITRK));

        /* R(mt_reg) = {} — create metatable */
        luaK_codeABC(ls->fs, OP_NEWTABLE, mt_reg, 0, 0);

        /* mt.__index = class_table */
        int ki = luaK_stringK(ls->fs, luaS_newliteral(ls->L, "__index"));
        luaK_codeABC(ls->fs, OP_SETTABLE, mt_reg, (unsigned int)(ki | BITRK), cls_reg);

        /* R(smt_reg) = _ENV["_setmetatable"] */
        int ks = luaK_stringK(ls->fs, luaS_newliteral(ls->L, "_setmetatable"));
        luaK_codeABC(ls->fs, OP_GETTABUP, smt_reg, 0, (unsigned int)(ks | BITRK));

        /* Call _setmetatable(obj, mt) */
        luaK_codeABC(ls->fs, OP_MOVE, tmp_base + 3, reg, 0);   /* self */
        luaK_codeABC(ls->fs, OP_MOVE, tmp_base + 4, mt_reg, 0); /* mt */
        luaK_codeABC(ls->fs, OP_CALL, smt_reg, 3, 1);

        /* call constructor: R(ctor_reg)(self, arg1, ...).
         * Object already at 'reg' with metatable set. */
        luaK_codeABC(ls->fs, OP_CALL, ctor_reg, nargs + 2, 1);
        ls->fs->freereg = reg + 1;
      }

      init_exp(v, VNONRELOC, reg);
      break;
    }
    case TK_JAVA_THIS: {
      next(ls);
      /* 'this' is the first parameter (self) in constructors and instance methods.
       * It's always at register 0 (the first local). */
      init_exp(v, VNONRELOC, 0);
      break;
    }
    case '{': {
      /* Array initializer: {expr, expr, ...}
       * Create a table and populate it with indexed values. */
      next(ls); /* skip '{' */
      int reg = ls->fs->freereg;
      luaK_codeABC(ls->fs, OP_NEWTABLE, reg, 0, 0);
      ls->fs->freereg = reg + 1;
      int idx = 0;
      if (ls->t.token != '}') {
        for (;;) {
          expdesc elem;
          expr(ls, &elem);
          /* store elem in table at index idx+1 (Lua 1-based) */
          int elem_reg = reg + 1;
          exp2reg(ls->fs, &elem, elem_reg);
          int ik = luaK_intK(ls->fs, idx + 1);
          luaK_codeABC(ls->fs, OP_SETTABLE, reg, (unsigned int)(ik | BITRK), elem_reg);
          ls->fs->freereg = reg + 1;
          idx++;
          if (ls->t.token == ',') { next(ls); }
          else break;
        }
      }
      check(ls, '}'); next(ls);
      init_exp(v, VNONRELOC, reg);
      break;
    }
    default:
      jlex_syntaxerror(ls, "unexpected token in expression");
  }
}

/* subexpr: precedence climbing with string concat support */
static void subexpr(JLexState *ls, expdesc *v, int limit) {
  BinOpr op;
  simpleexpr(ls, v);

  /* chain .name and .method() access (always processed, not a binary op).
   * Save root's global/static flag BEFORE the chain — once global, always global.
   * VRELOCABLE with no jumps = GETTABUP deferred global lookup. */
  int chain_global = (v->k == VRELOCABLE);

  while (ls->t.token == '.') {
    next(ls);
    if (ls->t.token != TK_JAVA_NAME)
      jlex_syntaxerror(ls, "expected identifier after '.'");
    TString *field = ls->t.seminfo.ts;
    next(ls);

    int was_global = chain_global;

    luaK_exp2nextreg(ls->fs, v);
    int obj_reg = v->u.info;
    int k = luaK_stringK(ls->fs, field);

    if (ls->t.token == '(') {
      /* Method call: allocate method reg, GETTABLE, CALL */
      int method_reg = ls->fs->freereg;
      ls->fs->freereg = method_reg + 1;
      luaK_codeABC(ls->fs, OP_GETTABLE, method_reg, obj_reg, k | BITRK);
      next(ls);

      int first_arg_reg;  /* where arguments start */
      int nargs;          /* argument count (not including self) */

      if (was_global) {
        /* Static call (ClassName.method()): no self, args start at method_reg+1 */
        first_arg_reg = method_reg + 1;
        nargs = 0;
      } else {
        /* Instance call (obj.method()): self = obj at method_reg+1, args at method_reg+2 */
        luaK_codeABC(ls->fs, OP_MOVE, method_reg + 1, obj_reg, 0);
        first_arg_reg = method_reg + 2;
        nargs = 1;  /* self counts as arg */
      }

      /* Ensure freereg accounts for used registers */
      if (ls->fs->freereg < first_arg_reg)
        ls->fs->freereg = first_arg_reg;

      if (ls->t.token != ')') {
        expdesc arg;
        expr(ls, &arg);
        exp2reg(ls->fs, &arg, first_arg_reg);
        /* Advance freereg past the placed argument so that parsing the
         * NEXT argument (which may allocate temp registers via method
         * calls, etc.) does not clobber this one. */
        if (ls->fs->freereg < first_arg_reg + 1)
          ls->fs->freereg = first_arg_reg + 1;
        nargs++;
        while (ls->t.token == ',') {
          next(ls);
          expr(ls, &arg);
          /* first_arg_reg already skips self, so nargs (which includes self)
           * would double-count. Use method_reg+1+nargs for correct position. */
          int arg_reg = method_reg + 1 + nargs;
          exp2reg(ls->fs, &arg, arg_reg);
          if (ls->fs->freereg < arg_reg + 1)
            ls->fs->freereg = arg_reg + 1;
          nargs++;
        }
      }
      check(ls, ')'); next(ls);
      /* nargs+1: CALL's B field encodes nargs as B-1, so B = nargs+1 */
      luaK_codeABC(ls->fs, OP_CALL, method_reg, nargs + 1, 2);
      ls->fs->freereg = method_reg;
      init_exp(v, VCALL, ls->fs->pc - 1);
    } else if (getstr(field)[0] == 'l'
               && strcmp(getstr(field), "length") == 0) {
      /* .length on an array/table: read the "length" field.
       * Tables store an explicit length field for 0-based arrays. */
      int len_reg = ls->fs->freereg;
      ls->fs->freereg = len_reg + 1;
      int kl = luaK_stringK(ls->fs, field);
      luaK_codeABC(ls->fs, OP_GETTABLE, len_reg, obj_reg, kl | BITRK);
      init_exp(v, VNONRELOC, len_reg);
    } else {
      /* Field access: use VINDEXED so expression reading emits GETTABLE
       * and assignment (luaK_storevar) emits SETTABLE. */
      v->k = VINDEXED;
      v->u.ind.t = (lu_byte)obj_reg;
      v->u.ind.vt = VLOCAL;
      v->u.ind.idx = (short)(k | BITRK);
      v->t = v->f = NO_JUMP;
    }
  }

  /* Bare function call: name(args) where name is not followed by '.'.
   * E.g. add(5, 7), println("hello"), etc. */
  if (ls->t.token == '(') {
    /* Resolve VRELOCABLE (deferred global) to a register first */
    if (v->k == VRELOCABLE)
      luaK_exp2nextreg(ls->fs, v);
    int base = v->u.info;
    next(ls);  /* skip '(' */
    /* Ensure argument expressions start after the function register */
    if (ls->fs->freereg < base + 1)
      ls->fs->freereg = base + 1;
    int nargs = 0;
    if (ls->t.token != ')') {
      expdesc arg;
      expr(ls, &arg);
      exp2reg(ls->fs, &arg, base + 1);
      /* Advance freereg past the placed argument so nested expressions in
       * subsequent arguments don't clobber it. */
      if (ls->fs->freereg < base + 2)
        ls->fs->freereg = base + 2;
      nargs = 1;
      while (ls->t.token == ',') {
        next(ls);
        expr(ls, &arg);
        int arg_reg = base + nargs + 1;
        exp2reg(ls->fs, &arg, arg_reg);
        if (ls->fs->freereg < arg_reg + 1)
          ls->fs->freereg = arg_reg + 1;
        nargs++;
      }
    }
    check(ls, ')'); next(ls);
    luaK_codeABC(ls->fs, OP_CALL, base, nargs + 1, 2);
    ls->fs->freereg = base;
    init_exp(v, VCALL, ls->fs->pc - 1);
  }

  /* Array / table indexing: arr[index]
   * E.g. arr[i], table[key]
   * Arrays are stored Lua 0-based (matching Java convention).
   *
   * Uses VINDEXED to keep table register and index register cleanly
   * separated.  For reads, luaK_dischargevars emits GETTABLE into
   * a pending register.  For writes, luaK_storevar directly emits
   * SETTABLE using the saved table/index.  No instruction patching. */
  while (ls->t.token == '[') {
    next(ls);  /* skip '[' */
    expdesc idx;
    expr(ls, &idx);
    check(ls, ']'); next(ls);  /* skip ']' */

    /* Resolve v (the table/array) to a register */
    if (v->k == VRELOCABLE)
      luaK_exp2nextreg(ls->fs, v);
    int tbl_reg = v->u.info;

    /* Put index into a register (after tbl_reg). 0-based, no offset needed. */
    int idx_reg = ls->fs->freereg;
    if (idx_reg <= tbl_reg) idx_reg = tbl_reg + 1;
    exp2reg(ls->fs, &idx, idx_reg);
    ls->fs->freereg = idx_reg + 1;

    /* Create VINDEXED: preserves table register and index register
     * so that both reads (GETTABLE) and writes (SETTABLE) work
     * correctly without instruction patching. */
    v->u.ind.t = (lu_byte)tbl_reg;
    v->u.ind.vt = VLOCAL;
    v->u.ind.idx = (short)idx_reg;
    v->k = VINDEXED;
    v->t = v->f = NO_JUMP;
  }

  /* binary operators with string concat handling */
  while (1) {
    int tok = ls->t.token;
    op = getbinopr(tok);

    /* precedence gate — MUST be checked before any '+' handling so that
     * recursive subexpr calls stop at same-level operators (left-assoc). */
    if (op == OPR_NOBINOPR || binop_level(tok) <= limit) break;

    int thislevel = binop_level(tok);

    /* Java '+': if the LEFT operand is a string literal (VK) or the result
     * of a previous concatenation (VRELOCABLE OP_CONCAT), this '+' is
     * string concatenation.  Handle it with explicit register placement
     * instead of luaK_infix/luaK_posfix.
     *
     * Why: CONCAT requires its two operands in CONSECUTIVE registers
     * (R(B)..R(C) with no gaps).  A method-call right operand such as
     * 'fruits.size()' leaves a dead copy of the receiver in the register
     * between the left operand and the call result, which would wrongly
     * be pulled into the CONCAT range.  By placing the left operand in a
     * register FIRST and then discharging the right operand's result into
     * exactly left+1 (overwriting any such dead intermediate registers),
     * we guarantee a clean two-register concat range. */
    if (tok == '+' && (v->k == VK || (v->k == VRELOCABLE &&
            GET_OPCODE(getinstruction(ls->fs, v)) == OP_CONCAT))) {
      next(ls);  /* consume '+' */
      luaK_exp2nextreg(ls->fs, v);   /* place LEFT in a register */
      int lreg = v->u.info;
      expdesc v2;
      subexpr(ls, &v2, thislevel);   /* respects limit, so no over-absorption */
      /* Discharge RIGHT's result into lreg+1 — the register immediately
       * after the left operand.  exp2reg emits a MOVE/LOADK as needed,
       * collapsing any gap left by a method-call receiver/self copy. */
      int rreg = lreg + 1;
      exp2reg(ls->fs, &v2, rreg);
      luaK_codeABC(ls->fs, OP_CONCAT, lreg, lreg, rreg);
      ls->fs->freereg = lreg + 1;  /* result in lreg; rreg now dead */
      init_exp(v, VNONRELOC, lreg);
      continue;
    }

    next(ls);  /* consume operator */
    luaK_infix(ls->fs, op, v);
    { expdesc v2;
      subexpr(ls, &v2, thislevel);
      /* Right-side string literal: switch from OPR_ADD to OPR_CONCAT.
       * A string literal allocates no register while parsing, so it is
       * safe to place the left operand here (after parsing the right).
       * (Right is a bare string literal, so no method-call gap issue.) */
      if (tok == '+' && op != OPR_CONCAT && v2.k == VK) {
        luaK_exp2nextreg(ls->fs, v);  /* put left into a register */
        op = OPR_CONCAT;
      }
      luaK_posfix(ls->fs, op, v, &v2, ls->linenumber);
    }
  }
}

static void expr(JLexState *ls, expdesc *v) {
  subexpr(ls, v, 0);
}

/* ---------- STATEMENT PARSER ---------- */

static void statement(JLexState *ls);

static void block(JLexState *ls) {
  check(ls, '{'); next(ls);
  while (ls->t.token != '}' && ls->t.token != TK_JAVA_EOS)
    statement(ls);
  check(ls, '}'); next(ls);
}

/* check if a token is a Java type keyword */
static int is_type_token(int token) {
  return token == TK_JAVA_INT || token == TK_JAVA_STRING ||
         token == TK_JAVA_VOID || token == TK_JAVA_BOOLEAN ||
         token == TK_JAVA_CHAR || token == TK_JAVA_DOUBLE;
}

/* skip modifiers and type, return type token */
static void skip_type(JLexState *ls) {
  if (is_type_token(ls->t.token)) {
    next(ls);
    /* skip array brackets on type: int[], String[], etc. */
    while (ls->t.token == '[') {
      next(ls);
      if (ls->t.token == ']') next(ls);
    }
    return;
  }
  /* qualified type name like java.util.List<String> */
  if (ls->t.token == TK_JAVA_NAME) {
    next(ls);
    while (ls->t.token == '.') {
      next(ls);
      if (ls->t.token == TK_JAVA_NAME) next(ls);
    }
    /* skip generics <...> */
    if (ls->t.token == '<') {
      int depth = 1;
      while (depth > 0 && ls->t.token != TK_JAVA_EOS) {
        next(ls);
        if (ls->t.token == '<') depth++;
        else if (ls->t.token == '>') depth--;
        else if (ls->t.token == '?') { /* wildcard */ }
      }
      if (ls->t.token == '>') next(ls);
    }
    /* skip array brackets */
    while (ls->t.token == '[') {
      next(ls);
      if (ls->t.token == ']') next(ls);
    }
    return;
  }
}

/* variable declaration */
static void var_declaration(JLexState *ls) {
  /* skip type */
  skip_type(ls);

  /* skip modifiers like static, public, private */
  while (ls->t.token == TK_JAVA_STATIC || ls->t.token == TK_JAVA_PUBLIC ||
         ls->t.token == TK_JAVA_PRIVATE || ls->t.token == TK_JAVA_FINAL) {
    next(ls);
    if (is_type_token(ls->t.token)) skip_type(ls);
  }

  if (ls->t.token == TK_JAVA_NAME) {
    TString *name = ls->t.seminfo.ts;
    next(ls);

    /* skip array brackets */
    while (ls->t.token == '[') { next(ls); check(ls, ']'); next(ls); }

    int reg;
    if (ls->t.token == '=') {
      next(ls);
      reg = new_localvar(ls->fs, name);
      expdesc v;
      expr(ls, &v);
      exp2reg(ls->fs, &v, reg);
    } else {
      reg = new_localvar(ls->fs, name);
      luaK_nil(ls->fs, reg, 1);
    }
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
  next(ls);
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
  next(ls);
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

  luaK_codeAsBx(ls->fs, OP_JMP, 0, loop_pc - ls->fs->pc - 1);
  luaK_patchtohere(ls->fs, j_false);

  leaveblock(ls->fs);
}

/* do-while statement */
static void do_while_statement(JLexState *ls) {
  next(ls); /* skip 'do' */

  int body_pc = ls->fs->pc;

  JBlockCnt bl;
  enterblock(ls->fs, &bl, 1);

  block(ls);

  check(ls, TK_JAVA_WHILE); next(ls); /* 'while' */
  check(ls, '('); next(ls);

  expdesc cond;
  expr(ls, &cond);
  luaK_goiftrue(ls->fs, &cond);
  int j_false = cond.f;

  check(ls, ')'); next(ls);
  skip_semicolon(ls);

  /* jump back to body if condition true */
  luaK_codeAsBx(ls->fs, OP_JMP, 0, body_pc - ls->fs->pc - 1);
  luaK_patchtohere(ls->fs, j_false);

  leaveblock(ls->fs);
}

/* for statement (numeric and enhanced) */
static void for_statement(JLexState *ls) {
  next(ls);
  check(ls, '('); next(ls);

  /* Check for enhanced for: for (Type var : collection) */
  int is_enhanced = 0;
  /* peek ahead: if we see type + name + ':', it's enhanced for */
  if (is_type_token(ls->t.token) || ls->t.token == TK_JAVA_NAME) {
    int t1 = ls->t.token;
    int la1 = jlex_lookahead(ls);
    if (la1 == TK_JAVA_NAME) {
      /* "Type Name" - check if next after that is ':' */
      /* consume type */
      next(ls);
      int la2 = jlex_lookahead(ls);
      if (la2 == ':') {
        /* enhanced for: for (Type var : expr) */
        is_enhanced = 1;
      }
      /* we've consumed type, name is next token */
    } else if (t1 == TK_JAVA_NAME && la1 == TK_JAVA_NAME) {
      /* "Type Name" pattern */
      next(ls); /* skip type */
      if (jlex_lookahead(ls) == ':') {
        is_enhanced = 1;
      }
    }
  }

  if (is_enhanced) {
    /* Enhanced for: for (Type var : expr) { ... }
     * Compile as: local _coll = expr; local _i = 1; local _lim = #_coll;
     *              while _i <= _lim do var = _coll[_i]; ...; _i++ end */
    /* type token already consumed, current token is var name */
    if (ls->t.token != TK_JAVA_NAME)
      jlex_syntaxerror(ls, "expected variable name in for-each");
    TString *varname = ls->t.seminfo.ts;
    next(ls);

    check(ls, ':'); next(ls);

    /* collection expression */
    expdesc coll;
    expr(ls, &coll);
    check(ls, ')'); next(ls);

    JBlockCnt bl;
    enterblock(ls->fs, &bl, 1);

    /* allocate loop variable FIRST (so it doesn't overlap with temp regs) */
    int varreg = new_localvar(ls->fs, varname);

    /* allocate temp registers past all active locals */
    if (ls->fs->freereg < ls->fs->nactvar)
      ls->fs->freereg = ls->fs->nactvar;
    int coll_reg = ls->fs->freereg;
    exp2reg(ls->fs, &coll, coll_reg);
    int idx_reg = coll_reg + 1;
    int lim_reg = coll_reg + 2;

    /* idx = 1; limit = #collection */
    int k1 = luaK_intK(ls->fs, 1);
    luaK_codeABx(ls->fs, OP_LOADK, idx_reg, (unsigned int)k1);
    luaK_codeABC(ls->fs, OP_LEN, lim_reg, coll_reg, 0);

    ls->fs->freereg = coll_reg + 3;

    int loop_start = ls->fs->pc;

    /* if idx <= limit → continue (skip next), else jump to exit */
    luaK_codeABC(ls->fs, OP_LE, 0, idx_reg, lim_reg);
    int exit_jmp = luaK_jump(ls->fs);

    /* var = collection[idx] */
    luaK_codeABC(ls->fs, OP_GETTABLE, varreg, coll_reg, idx_reg);

    block(ls);  /* loop body */

    /* idx++ */
    luaK_codeABC(ls->fs, OP_ADD, idx_reg, idx_reg, k1 | BITRK);

    /* jump back to condition check */
    luaK_patchlist(ls->fs, luaK_jump(ls->fs), loop_start);

    /* exit: */
    luaK_patchtohere(ls->fs, exit_jmp);

    leaveblock(ls->fs);
    /* freereg is now nactvar — all block-internal temp registers released */
    return;
  }

  /* init: int i = expr */
  if (is_type_token(ls->t.token)) next(ls);
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

  /* condition: i < expr (compile as boolean test for while-style loop) */
  int cond_pc = ls->fs->pc;  /* save PC where condition bytecodes start */
  expdesc cond;
  expr(ls, &cond);

  /* detect step type (i++ or i--) before consuming tokens */
  int step_is_inc = 1;  /* default: i++ */
  check(ls, ';'); next(ls);
  if (ls->t.token != ')') {
    if (ls->t.token == TK_JAVA_INC || ls->t.token == TK_JAVA_DEC) {
      step_is_inc = (ls->t.token == TK_JAVA_INC);
      next(ls);
    } else if (ls->t.token == TK_JAVA_NAME) {
      next(ls);
      if (ls->t.token == TK_JAVA_INC || ls->t.token == TK_JAVA_DEC) {
        step_is_inc = (ls->t.token == TK_JAVA_INC);
        next(ls);
      } else {
        while (ls->t.token != ')' && ls->t.token != TK_JAVA_EOS)
          next(ls);
      }
    }
  }
  check(ls, ')'); next(ls);

  JBlockCnt bl;
  enterblock(ls->fs, &bl, 1);

  /* Compile as while-style loop:
   *   cond: if cond false → jump past loop
   *   body
   *   step: i = i +/- 1
   *   jump back to cond
   *   end:
   */
  luaK_goiftrue(ls->fs, &cond);  /* cond.t→here (body), cond.f→exit jumps */

  block(ls);

  /* step: i++ or i-- */
  int k1 = luaK_intK(ls->fs, 1);
  if (step_is_inc)
    luaK_codeABC(ls->fs, OP_ADD, varreg, varreg, k1 | BITRK);
  else
    luaK_codeABC(ls->fs, OP_SUB, varreg, varreg, k1 | BITRK);

  /* jump back to re-evaluate condition */
  luaK_patchlist(ls->fs, luaK_jump(ls->fs), cond_pc);

  /* false exits go past the loop */
  luaK_patchtohere(ls->fs, cond.f);

  leaveblock(ls->fs);
}

/* switch statement */
static void switch_statement(JLexState *ls) {
  /* switch (expr) { case val: stmts; break; ... default: stmts; } */
  /* Compile as: if (val == case1) goto body1; elseif ... else goto default;
   * Each body ends with a jump past all cases (unless fallthrough) */
  next(ls); /* skip 'switch' */
  check(ls, '('); next(ls);

  expdesc switch_val;
  expr(ls, &switch_val);
  int valreg = ls->fs->freereg;
  luaK_exp2nextreg(ls->fs, &switch_val);
  valreg = switch_val.u.info;

  check(ls, ')'); next(ls);
  check(ls, '{'); next(ls);

  /* We'll build a chain of if/elseif comparisons.
   * Collect case values and bodies. */
  int j_end_all = NO_JUMP;  /* jump past all cases (from break) */
  int j_prev_false = NO_JUMP;  /* chain of false jumps */
  int has_default = 0;
  int default_body_pc = -1;
  int *j_ends = NULL;  /* patch list for break jumps */
  int nj_ends = 0;

  while (ls->t.token == TK_JAVA_CASE || ls->t.token == TK_JAVA_DEFAULT) {
    if (ls->t.token == TK_JAVA_DEFAULT) {
      has_default = 1;
      next(ls);
      check(ls, ':'); next(ls);
      /* patch all pending false jumps to here */
      if (j_prev_false != NO_JUMP) {
        luaK_patchtohere(ls->fs, j_prev_false);
        j_prev_false = NO_JUMP;
      }
      /* parse statements until break or next case */
      while (ls->t.token != TK_JAVA_CASE && ls->t.token != TK_JAVA_DEFAULT &&
             ls->t.token != '}') {
        if (ls->t.token == TK_JAVA_BREAK) {
          next(ls);
          skip_semicolon(ls);
          /* jump past all cases */
          int j = luaK_jump(ls->fs);
          luaK_concat(ls->fs, &j_end_all, j);
          break;
        }
        statement(ls);
      }
    } else {
      /* case value: */
      next(ls);
      expdesc case_val;
      expr(ls, &case_val);
      luaK_exp2nextreg(ls->fs, &case_val);
      int case_reg = case_val.u.info;

      check(ls, ':'); next(ls);

      /* emit: if (switch_val != case_val) goto next_case;
       * OP_EQ A=0 means: skip next instruction when values ARE equal.
       * So: EQ 0,val,case  /  JMP next_case  /  ...body...
       * If equal → skip JMP → execute body
       * If not equal → don't skip → JMP jumps over body */
      luaK_codeABC(ls->fs, OP_EQ, 0, valreg, case_reg);
      int j_false = luaK_jump(ls->fs);  /* skip body when not equal */

      /* parse body */
      int j_break = NO_JUMP;
      while (ls->t.token != TK_JAVA_CASE && ls->t.token != TK_JAVA_DEFAULT &&
             ls->t.token != '}') {
        if (ls->t.token == TK_JAVA_BREAK) {
          next(ls);
          skip_semicolon(ls);
          j_break = luaK_jump(ls->fs);
          break;
        }
        statement(ls);
      }
      /* jump past all remaining cases (end of this case body) */
      int j_body_end = luaK_jump(ls->fs);
      /* patch the false jump here */
      luaK_patchtohere(ls->fs, j_false);
      /* chain the body-end jump */
      luaK_concat(ls->fs, &j_end_all, j_body_end);
      if (j_break != NO_JUMP)
        luaK_concat(ls->fs, &j_end_all, j_break);
    }
  }

  check(ls, '}'); next(ls);

  /* patch all jumps past the switch */
  luaK_patchtohere(ls->fs, j_end_all);

  (void)j_prev_false; (void)has_default; (void)default_body_pc;
  (void)j_ends; (void)nj_ends;
}

/* break statement */
static void break_statement(JLexState *ls) {
  next(ls);
  skip_semicolon(ls);
  /* Find enclosing loop block and jump past it.
   * For now we use a simple approach: just generate a forward jump
   * that will be patched later. But proper break requires tracking loop exits.
   * Simplified: generate a jump that will be patched in the loop statement. */
  (void)ls;
  /* TODO: proper break handling */
}

/* return statement */
static void return_statement(JLexState *ls) {
  next(ls);
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

/* try-catch-finally */
static void try_statement(JLexState *ls) {
  /* For now: skip entire try/catch/finally block.
   * Proper implementation requires exception handling which is complex. */
  next(ls); /* skip 'try' */
  /* skip try body */
  int brace_depth = 0;
  while (ls->t.token != TK_JAVA_EOS) {
    if (ls->t.token == '{') { brace_depth++; next(ls); continue; }
    if (ls->t.token == '}') {
      if (brace_depth == 0) break;
      brace_depth--;
      next(ls);
      continue;
    }
    if (ls->t.token == TK_JAVA_CATCH && brace_depth == 0) {
      /* skip catch block */
      next(ls); /* skip 'catch' */
      if (ls->t.token == '(') {
        while (ls->t.token != ')' && ls->t.token != TK_JAVA_EOS) next(ls);
      }
      if (ls->t.token == ')') next(ls);
      /* skip catch body */
      if (ls->t.token == '{') {
        next(ls); brace_depth = 1;
        while (brace_depth > 0 && ls->t.token != TK_JAVA_EOS) {
          if (ls->t.token == '{') brace_depth++;
          else if (ls->t.token == '}') brace_depth--;
          next(ls);
        }
      }
      continue;
    }
    if (ls->t.token == TK_JAVA_FINALLY && brace_depth == 0) {
      next(ls); /* skip 'finally' */
      if (ls->t.token == '{') {
        next(ls); brace_depth = 1;
        while (brace_depth > 0 && ls->t.token != TK_JAVA_EOS) {
          if (ls->t.token == '{') brace_depth++;
          else if (ls->t.token == '}') brace_depth--;
          next(ls);
        }
      }
      break;
    }
    next(ls);
  }
  /* emit a no-op for the try block */
  /* Just emit a println to show we hit this */
  {
    int reg = ls->fs->freereg;
    emit_string_const(ls->fs, "  (try/catch skipped)", reg);
    luaK_codeABC(ls->fs, OP_GETTABUP, reg + 1, 0,
      (unsigned int)(luaK_stringK(ls->fs, luaS_new(ls->L, "System")) | BITRK));
    int fk = luaK_stringK(ls->fs, luaS_new(ls->L, "out"));
    luaK_codeABC(ls->fs, OP_GETTABLE, reg + 1, reg + 1, fk | BITRK);
    fk = luaK_stringK(ls->fs, luaS_new(ls->L, "println"));
    luaK_codeABC(ls->fs, OP_GETTABLE, reg + 1, reg + 1, fk | BITRK);
    luaK_codeABC(ls->fs, OP_MOVE, reg + 2, reg + 1, 0);
    luaK_codeABC(ls->fs, OP_MOVE, reg + 3, reg, 0);
    luaK_codeABC(ls->fs, OP_CALL, reg + 2, 2, 1);
    ls->fs->freereg = reg + 1;
  }
}

/* expression statement (also handles assignments) */
static void expr_statement(JLexState *ls) {
  expdesc v;
  expr(ls, &v);

  /* Handle postfix ++ and -- (e.g. j++) */
  if (ls->t.token == TK_JAVA_INC || ls->t.token == TK_JAVA_DEC) {
    int is_inc = (ls->t.token == TK_JAVA_INC);
    next(ls);
    /* emit increment/decrement for the variable */
    if (v.k == VNONRELOC) {
      int k1 = luaK_intK(ls->fs, 1);
      luaK_codeABC(ls->fs, is_inc ? OP_ADD : OP_SUB,
                   v.u.info, v.u.info, k1 | BITRK);
    }
    skip_semicolon(ls);
    return;
  }

  /* Check for assignment: x = expr */
  if (ls->t.token == '=') {
    next(ls);
    expdesc rhs;
    expr(ls, &rhs);
    if (v.k == VINDEXED)
      luaK_storevar(ls->fs, &v, &rhs);  /* obj.field or arr[idx] = value → SETTABLE */
    else if (v.k == VNONRELOC) {
      /* Normal local = value */
      exp2reg(ls->fs, &rhs, v.u.info);
    }
    skip_semicolon(ls);
    return;
  }

  /* Check for compound assignment: +=, -=, *=, /=, %= */
  if (ls->t.token == TK_JAVA_PLUSEQ || ls->t.token == TK_JAVA_MINEQ ||
      ls->t.token == TK_JAVA_MULEQ || ls->t.token == TK_JAVA_DIVEQ ||
      ls->t.token == TK_JAVA_MODEQ) {
    int op_token = ls->t.token;
    next(ls);
    expdesc rhs;
    expr(ls, &rhs);
    if (v.k == VNONRELOC) {
      int rreg = ls->fs->freereg;
      exp2reg(ls->fs, &rhs, rreg);
      ls->fs->freereg = rreg + 1;
      int lua_op;
      switch (op_token) {
        case TK_JAVA_PLUSEQ: lua_op = OP_ADD; break;
        case TK_JAVA_MINEQ: lua_op = OP_SUB; break;
        case TK_JAVA_MULEQ: lua_op = OP_MUL; break;
        case TK_JAVA_DIVEQ: lua_op = OP_DIV; break;
        case TK_JAVA_MODEQ: lua_op = OP_MOD; break;
        default:  lua_op = OP_ADD; break;
      }
      luaK_codeABC(ls->fs, lua_op, v.u.info, v.u.info, rreg);
    }
    skip_semicolon(ls);
    return;
  }

  skip_semicolon(ls);
}

/* throw statement */
static void throw_statement(JLexState *ls) {
  next(ls); /* skip 'throw' */
  /* skip the thrown expression */
  while (ls->t.token != ';' && ls->t.token != TK_JAVA_EOS)
    next(ls);
  skip_semicolon(ls);
  /* Emit error */
  luaK_codeABC(ls->fs, OP_GETTABUP, 0, 0,
    (unsigned int)(luaK_stringK(ls->fs, luaS_new(ls->L, "System")) | BITRK));
}

/* single statement dispatch */
static void statement(JLexState *ls) {
  int first = ls->t.token;

  /* check for type + name pattern (variable declaration) */
  if (first == TK_JAVA_INT || first == TK_JAVA_STRING ||
      first == TK_JAVA_VOID || first == TK_JAVA_BOOLEAN ||
      first == TK_JAVA_CHAR || first == TK_JAVA_DOUBLE ||
      first == TK_JAVA_PUBLIC || first == TK_JAVA_PRIVATE ||
      first == TK_JAVA_STATIC || first == TK_JAVA_FINAL ||
      first == TK_JAVA_PROTECTED) {
    int lookahead = jlex_lookahead(ls);
    if (lookahead == TK_JAVA_NAME || lookahead == '[' ||
        lookahead == TK_JAVA_INT || lookahead == TK_JAVA_STRING ||
        lookahead == TK_JAVA_CHAR || lookahead == TK_JAVA_DOUBLE ||
        lookahead == TK_JAVA_STATIC || lookahead == TK_JAVA_PUBLIC ||
        lookahead == TK_JAVA_PRIVATE) {
      var_declaration(ls);
      return;
    }
  }

  /* check for Name type (e.g. "List<String>" vs "System.out.println()")
   * Distinguish type-declaration from expression:
   * - "Name <..." or "Name [" → type (generics or array)
   * - "Name ." → expression (qualified access like System.out)
   * - "Name (" → expression (method call)
   * - "Name Name" → could be either. Check if 3rd token is "(".
   *   We can't use double-lookahead (jlex_lookahead only stores one).
   *   Instead: "Name Name" is treated as var decl if 2nd Name is followed
   *   by something that looks like a variable (not '(').
   *   But since we can only peek one token ahead, we conservatively
   *   treat "Name Name" as a potential type+var decl and let
   *   var_declaration handle it. var_declaration will parse the type
   *   and if the next thing is not a valid var decl, it'll error.
   *   However, we must NOT treat "Name ." as var decl — that's always expr.
   */
  if (first == TK_JAVA_NAME) {
    int la = jlex_lookahead(ls);
    if (la == '<') {
      /* "Name<...>" → generics type declaration */
      var_declaration(ls);
      return;
    }
    if (la == '.') {
      /* "Name." → qualified expression (System.out, obj.field, etc.)
       * NOT a type declaration. Keep lookahead (it stores '.'),
       * and fall through to expr_statement which will consume it. */
      /* fall through to switch below */
    } else if (la == '(') {
      /* "Name(" → method call, not var decl.
       * Keep lookahead, fall through to expr_statement. */
      /* fall through to switch below */
    } else if (la == TK_JAVA_NAME) {
      /* "Name Name" → type + variable name.
       * next() returns lookahead (second name), losing the first.
       * Save the first name (type, unused for now) and treat the second
       * as the variable name. */
      next(ls);  /* returns lookahead → ls->t = second name (variable) */
      TString *vname = ls->t.seminfo.ts;
      next(ls);  /* consume variable name */
      while (ls->t.token == '[') { next(ls); check(ls, ']'); next(ls); }
      int reg;
      if (ls->t.token == '=') {
        next(ls);
        reg = new_localvar(ls->fs, vname);
        expdesc v;
        expr(ls, &v);
        exp2reg(ls->fs, &v, reg);
      } else {
        reg = new_localvar(ls->fs, vname);
        luaK_nil(ls->fs, reg, 1);
      }
      skip_semicolon(ls);
      return;
    }
    /* For other la values (=, ;, +, etc.), it's an expression.
     * Keep lookahead, fall through to expr_statement. */
  }

  switch (first) {
    case '{':  block(ls); break;
    case TK_JAVA_IF:    if_statement(ls); break;
    case TK_JAVA_WHILE: while_statement(ls); break;
    case TK_JAVA_DO:    do_while_statement(ls); break;
    case TK_JAVA_FOR:   for_statement(ls); break;
    case TK_JAVA_SWITCH: switch_statement(ls); break;
    case TK_JAVA_RETURN: return_statement(ls); break;
    case TK_JAVA_BREAK: break_statement(ls); break;
    case TK_JAVA_TRY:   try_statement(ls); break;
    case TK_JAVA_THROW: throw_statement(ls); break;
    case TK_JAVA_INTLIT:
    case TK_JAVA_FLOATLIT:
    case TK_JAVA_STRLIT:
    case TK_JAVA_NEW:
    case TK_JAVA_NAME:
    case '(': case '!': case '-':
    case TK_JAVA_TRUE: case TK_JAVA_FALSE:
    case TK_JAVA_NULL:
    case TK_JAVA_THIS:
      expr_statement(ls); break;
    case ';': next(ls); break;
    default:
      jlex_syntaxerror(ls, "unexpected statement");
  }
}

/* ---------- METHOD / CLASS PARSER ---------- */

/*
** Parse method parameters and return parameter count + register them.
** Returns: number of parameters registered.
** Parameters start at register 0 (method body's first local slots).
*/
static int parse_method_params(JLexState *ls, int *has_varargs, TString **vararg_name) {
  int nparams = 0;
  *has_varargs = 0;
  *vararg_name = NULL;
  check(ls, '('); next(ls);
  while (ls->t.token != ')') {
    /* skip param type */
    if (is_type_token(ls->t.token)) next(ls);
    else if (ls->t.token == TK_JAVA_NAME) {
      next(ls);
      while (ls->t.token == '.') {
        next(ls);
        if (ls->t.token == TK_JAVA_NAME) next(ls);
      }
    }
    /* skip array brackets */
    while (ls->t.token == '[') { next(ls); check(ls, ']'); next(ls); }
    /* check for varargs '...' */
    int is_vararg = 0;
    if (ls->t.token == '.') {
      while (ls->t.token == '.') next(ls);
      is_vararg = 1;
      *has_varargs = 1;
    }
    /* param name */
    if (ls->t.token == TK_JAVA_NAME) {
      TString *pname = ls->t.seminfo.ts;
      next(ls);
      if (is_vararg) {
        *vararg_name = pname;  /* save name, emit packing code in caller */
      } else {
        new_localvar(ls->fs, pname);
        nparams++;
      }
    }
    if (ls->t.token == ',') next(ls);
  }
  next(ls); /* skip ) */
  return nparams;
}

/* parse a Java method (or field if no '(' follows).
 * Returns 1 if this is a constructor, 0 otherwise. */
static int method_definition(JLexState *ls, FuncState *fs, int class_reg,
                              TString *class_name, int *has_main,
                              int main_cands_reg) {
  (void)fs;

  /* Capture line where method/ctor definition starts */
  int method_start_line = ls->linenumber;

  /* Check if this is a constructor: name == class name and no return type */
  int is_ctor = 0;

  /* skip modifiers, track static */
  int is_static = 0;
  int saw_return_type = 0;
  while (ls->t.token == TK_JAVA_PUBLIC || ls->t.token == TK_JAVA_PRIVATE ||
         ls->t.token == TK_JAVA_PROTECTED || ls->t.token == TK_JAVA_STATIC ||
         ls->t.token == TK_JAVA_FINAL) {
    if (ls->t.token == TK_JAVA_STATIC) is_static = 1;
    next(ls);
  }

  /* check for return type or constructor */
  TString *method_name = NULL;
  if (is_type_token(ls->t.token)) {
    saw_return_type = 1;
    next(ls); /* skip return type */
  } else if (ls->t.token == TK_JAVA_NAME) {
    /* could be constructor or return type */
    int la = jlex_lookahead(ls);
    if (la == TK_JAVA_NAME || la == '<') {
      /* "ClassName methodName" → return type + method name */
      saw_return_type = 1;
      next(ls); /* skip return type */
      /* skip generics */
      if (ls->t.token == '<') {
        while (ls->t.token != '>' && ls->t.token != TK_JAVA_EOS) next(ls);
        if (ls->t.token == '>') next(ls);
      }
    }
    /* else: "methodName(" → method name (constructor or no return type) */
  }

  /* method/field name */
  if (ls->t.token != TK_JAVA_NAME)
    jlex_syntaxerror(ls, "expected method name");
  method_name = ls->t.seminfo.ts;
  /* Detect constructor: same name as class, no return type, not static */
  if (class_name != NULL && !is_static && !saw_return_type &&
      method_name == class_name) {
    is_ctor = 1;
  }
  next(ls);

  /* Handle field declaration: no '(' after name */
  if (ls->t.token != '(') {
    /* This is a field declaration, not a method.
     * Skip array brackets, initializer, and semicolon. */
    while (ls->t.token == '[') { next(ls); check(ls, ']'); next(ls); }
    if (ls->t.token == '=') {
      next(ls);
      /* parse initializer expression and store in class table */
      expdesc v;
      expr(ls, &v);
      int reg = ls->fs->freereg;
      exp2reg(ls->fs, &v, reg);
      int fk = luaK_stringK(ls->fs, method_name);
      luaK_codeABC(ls->fs, OP_SETTABLE, class_reg, fk | BITRK, reg);
      /* if static field, also expose in _ENV for unqualified access */
      if (is_static) {
        luaK_codeABC(ls->fs, OP_SETTABUP, 0, fk | BITRK, reg);
      }
      ls->fs->freereg = reg;
    }
    skip_semicolon(ls);
    return;
  }

  /* create nested FuncState */
  FuncState method_fs;
  struct LexState method_mini_ls;
  Proto *proto = luaF_newproto(ls->L);
  proto->source = ls->source;
  luaC_objbarrier(ls->L, proto, proto->source);
  luaC_objbarrier(ls->L, ls->fs->f, proto);

  method_fs.f = proto;
  method_fs.prev = ls->fs;
  method_mini_ls.L = ls->L;
  method_mini_ls.h = ls->h;
  method_mini_ls.lastline = ls->linenumber;
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

  /* _ENV upvalue */
  {
    Proto *mf = method_fs.f;
    luaM_growvector(ls->L, mf->upvalues, method_fs.nups, mf->sizeupvalues,
                    Upvaldesc, MAXUPVAL, "upvalues");
    int oldsize = mf->sizeupvalues;
    while (oldsize < (int)mf->sizeupvalues)
      mf->upvalues[oldsize++].name = NULL;
    mf->upvalues[method_fs.nups].instack = 0;
    mf->upvalues[method_fs.nups].idx = 0;
    mf->upvalues[method_fs.nups].name = ls->envn;
    luaC_objbarrier(ls->L, mf, ls->envn);
    method_fs.nups++;
    mf->sizeupvalues = method_fs.nups;
  }

  /* Constructor and instance methods: 'this' is the first parameter (R0).
   * Reserve R0 so explicit params start at R1.
   * Must happen AFTER switching fs to method_fs. */

  FuncState *saved_fs = ls->fs;
  ls->fs = &method_fs;

  JBlockCnt bl;
  enterblock(ls->fs, &bl, 0);

  /* For non-static methods, reserve R0 for 'self'/'this' */
  if (!is_static) {
    new_localvar(ls->fs, luaS_newliteral(ls->L, "this"));
  }

  /* parse parameters (explicit params start at R1 for non-static) */
  int has_varargs = 0;
  TString *vararg_name = NULL;
  int nparams = parse_method_params(ls, &has_varargs, &vararg_name);
  if (!is_static) nparams++;  /* include 'this' as first param */

  /* skip 'throws' clause */
  while (ls->t.token == TK_JAVA_THROWS) {
    next(ls);
    while (ls->t.token == TK_JAVA_NAME) {
      next(ls);
      if (ls->t.token == ',') next(ls); else break;
    }
  }

  /* set varargs info before emitting any varargs-related code */
  method_fs.f->numparams = (lu_byte)nparams;
  method_fs.f->is_vararg = has_varargs ? 1 : 0;

  /* if varargs method, emit packing code: name = {...} */
  if (has_varargs && vararg_name) {
    int pack_reg = ls->fs->freereg;
    luaK_codeABC(ls->fs, OP_NEWTABLE, pack_reg, 0, 0);
    luaK_codeABC(ls->fs, OP_VARARG, pack_reg + 1, 0, 0);
    luaK_codeABC(ls->fs, OP_SETLIST, pack_reg, 0, 1);
    /* store table["length"] = #table (for 0-based array length access) */
    {
      int len_reg = pack_reg + 2;
      luaK_codeABC(ls->fs, OP_LEN, len_reg, pack_reg, 0);
      int kl = luaK_stringK(ls->fs, luaS_newliteral(ls->L, "length"));
      luaK_codeABC(ls->fs, OP_SETTABLE, pack_reg, kl | BITRK, len_reg);
    }
    new_localvar(ls->fs, vararg_name);
    if (ls->fs->freereg < pack_reg + 3)
      ls->fs->freereg = pack_reg + 3;
  }

  /* parse body or skip abstract/native */
  if (ls->t.token == '{') {
    block(ls);
  } else if (ls->t.token == ';') {
    next(ls); /* abstract or interface method */
    /* restore and skip */
    ls->fs = saved_fs;
    return is_ctor;
  }

  /* Save peak register count before leaveblock resets freereg */
  lu_byte maxreg = ls->fs->freereg;
  luaK_ret(ls->fs, 0, 0);
  leaveblock(ls->fs);

  /* finalize method proto */
  method_fs.f->sizecode = method_fs.pc;
  method_fs.f->sizek = method_fs.nk;
  method_fs.f->sizep = method_fs.np;
  method_fs.f->sizelocvars = method_fs.nlocvars;
  method_fs.f->sizeupvalues = method_fs.nups;
  method_fs.f->linedefined = method_start_line;
  method_fs.f->lastlinedefined = ls->linenumber;
  method_fs.f->maxstacksize = maxreg;
  if ((int)method_fs.f->maxstacksize < (int)nparams)
      method_fs.f->maxstacksize = (lu_byte)nparams;
  method_fs.f->numparams = (lu_byte)nparams;  /* already set above, but ensure consistency */

  /* register proto in parent */
  int proto_idx = saved_fs->np;
  luaM_growvector(ls->L, saved_fs->f->p, proto_idx, saved_fs->f->sizep,
                  Proto *, MAXARG_Bx, "functions");
  saved_fs->f->p[proto_idx] = proto;
  saved_fs->np++;

  ls->fs = saved_fs;

  /* generate OP_CLOSURE */
  int reg = ls->fs->freereg;
  luaK_codeABx(ls->fs, OP_CLOSURE, reg, (unsigned int)proto_idx);
  ls->fs->freereg = reg + 1;

  /* store in class table */
  int mk = luaK_stringK(ls->fs, method_name);
  luaK_codeABC(ls->fs, OP_SETTABLE, class_reg, mk | BITRK, reg);

  /* if static, also expose in _ENV for unqualified access (e.g. add(5,7)) */
  if (is_static) {
    if (has_main && method_name == luaS_newliteral(ls->L, "main")) {
      if (*has_main)
        jlex_syntaxerror(ls, "duplicate static 'main' definition");
      *has_main = 1;
      /* store in _MAIN_CANDIDATES[class_name] instead of _ENV */
      int ck = luaK_stringK(ls->fs, class_name);
      luaK_codeABC(ls->fs, OP_SETTABLE, main_cands_reg, ck | BITRK, reg);
    } else {
      luaK_codeABC(ls->fs, OP_SETTABUP, 0, mk | BITRK, reg);
    }
  }

  /* if constructor, also store as "new" for lookup */
  if (is_ctor) {
    int nk = luaK_stringK(ls->fs, luaS_newliteral(ls->L, "new"));
    luaK_codeABC(ls->fs, OP_SETTABLE, class_reg, nk | BITRK, reg);
  }

  ls->fs->freereg = reg;
  return is_ctor;
}

/* parse an enum inside class body */
static void enum_definition(JLexState *ls, int class_reg, int main_cands_reg) {
  next(ls); /* skip 'enum' */

  if (ls->t.token != TK_JAVA_NAME)
    jlex_syntaxerror(ls, "expected enum name");
  TString *enum_name = ls->t.seminfo.ts;
  next(ls);

  check(ls, '{'); next(ls);

  int enum_table_reg = ls->fs->freereg;
  luaK_codeABC(ls->fs, OP_NEWTABLE, enum_table_reg, 0, 0);
  ls->fs->freereg = enum_table_reg + 2;  /* reserve reg+1 for temp values */
  /* Ensure maxstacksize accounts for temp registers (see commits 2daeabf, 9e39aaf) */
  if (ls->fs->f->maxstacksize < (lu_byte)(enum_table_reg + 2))
    ls->fs->f->maxstacksize = (lu_byte)(enum_table_reg + 2);

  int idx = 0;
  while (ls->t.token != '}' && ls->t.token != TK_JAVA_EOS) {
    if (ls->t.token == TK_JAVA_NAME) {
      TString *cname = ls->t.seminfo.ts;
      next(ls);

      /* store: enum_table[cname] = idx (ordinal) */
      int ik = luaK_intK(ls->fs, idx);
      luaK_codeABx(ls->fs, OP_LOADK, enum_table_reg + 1, (unsigned int)ik);
      int fk = luaK_stringK(ls->fs, cname);
      luaK_codeABC(ls->fs, OP_SETTABLE, enum_table_reg, fk | BITRK, enum_table_reg + 1);

      /* also make each constant globally accessible: _ENV[cname] = idx */
      int gk = luaK_stringK(ls->fs, cname);
      luaK_codeABC(ls->fs, OP_SETTABUP, 0, gk | BITRK, enum_table_reg + 1);

      idx++;
      if (ls->t.token == ',') next(ls);
      else break;
    } else if (ls->t.token == ';') {
      next(ls);
      /* enum may have methods/fields after ; */
      while (ls->t.token != '}' && ls->t.token != TK_JAVA_EOS) {
        if (ls->t.token == TK_JAVA_NAME || ls->t.token == TK_JAVA_PUBLIC ||
            ls->t.token == TK_JAVA_PRIVATE || ls->t.token == TK_JAVA_STATIC) {
          int la = jlex_lookahead(ls);
          if (la == '(') {
            /* method */
            method_definition(ls, ls->fs, enum_table_reg, NULL, NULL, main_cands_reg);
          } else {
            /* skip */
            next(ls);
            while (ls->t.token != ';' && ls->t.token != '}' && ls->t.token != TK_JAVA_EOS)
              next(ls);
            if (ls->t.token == ';') next(ls);
          }
        } else {
          next(ls);
        }
      }
      break;
    } else {
      next(ls);
    }
  }
  check(ls, '}'); next(ls);

  /* store enum table as a field in the class table */
  int fk = luaK_stringK(ls->fs, enum_name);
  luaK_codeABC(ls->fs, OP_SETTABLE, class_reg, fk | BITRK, enum_table_reg);

  /* also make the enum table globally accessible: _ENV[enum_name] = enum_table */
  luaK_codeABC(ls->fs, OP_SETTABUP, 0, fk | BITRK, enum_table_reg);

  ls->fs->freereg = enum_table_reg;
}

/* parse a Java class */
static void class_definition(JLexState *ls, FuncState *fs, int *has_main,
                              int main_cands_reg) {
  next(ls); /* skip 'class' */

  if (ls->t.token != TK_JAVA_NAME)
    jlex_syntaxerror(ls, "expected class name");
  TString *class_name = ls->t.seminfo.ts;
  next(ls);

  /* skip extends/implements */
  if (ls->t.token == TK_JAVA_EXTENDS || ls->t.token == TK_JAVA_IMPLEMENTS) {
    next(ls);
    while (ls->t.token == TK_JAVA_NAME) {
      next(ls);
      if (ls->t.token == ',') next(ls);
      else break;
    }
  }

  /* create class table */
  int class_reg = fs->freereg;
  luaK_codeABC(fs, OP_NEWTABLE, class_reg, 0, 0);
  fs->freereg = class_reg + 1;

  /* store class table in _ENV for 'new ClassName()' and static access */
  {
    int ck = luaK_stringK(fs, class_name);
    luaK_codeABC(fs, OP_SETTABUP, 0, ck | BITRK, class_reg);
  }

  check(ls, '{'); next(ls);

  /* parse members, track if a constructor was defined */
  int has_constructor = 0;
  while (ls->t.token != '}' && ls->t.token != TK_JAVA_EOS) {
    int first = ls->t.token;

    /* enum (with optional modifiers) */
    if (first == TK_JAVA_ENUM) {
      enum_definition(ls, class_reg, main_cands_reg);
      continue;
    }
    if (first == TK_JAVA_PUBLIC || first == TK_JAVA_PRIVATE ||
        first == TK_JAVA_PROTECTED || first == TK_JAVA_STATIC) {
      /* Check if next token after modifier is 'enum' */
      int la = jlex_lookahead(ls);
      if (la == TK_JAVA_ENUM) {
        next(ls); /* skip modifier (consumes lookahead) */
        enum_definition(ls, class_reg, main_cands_reg);
        continue;
      }
      /* Lookahead was not 'enum'. Keep it — method_definition will
       * consume it via next(ls) which uses lookahead first. */
    }

    /* method/constructor/field: use method_definition for everything */
    if (first == TK_JAVA_PUBLIC || first == TK_JAVA_PRIVATE ||
        first == TK_JAVA_PROTECTED || first == TK_JAVA_STATIC ||
        first == TK_JAVA_FINAL || is_type_token(first) ||
        first == TK_JAVA_NAME) {
      if (method_definition(ls, fs, class_reg, class_name, has_main, main_cands_reg))
        has_constructor = 1;
      continue;
    }

    /* unknown token - skip */
    next(ls);
  }
  check(ls, '}'); next(ls);

  /* If no explicit constructor was defined, generate a default one.
   * Default constructor: empty body, just returns (no-op). */
  if (!has_constructor) {
    int ctor_start_line = ls->linenumber;

    /* Create minimal Proto for default constructor */
    Proto *proto = luaF_newproto(ls->L);
    proto->source = ls->source;
    luaC_objbarrier(ls->L, proto, proto->source);
    luaC_objbarrier(ls->L, fs->f, proto);

    FuncState ctor_fs;
    struct LexState ctor_mini_ls;
    ctor_fs.f = proto;
    ctor_fs.prev = fs;
    ctor_mini_ls.L = ls->L;
    ctor_mini_ls.h = ls->h;
    ctor_mini_ls.lastline = ls->linenumber;
    ctor_fs.ls = &ctor_mini_ls;
    ctor_fs.bl = NULL;
    ctor_fs.pc = 0;
    ctor_fs.lasttarget = 0;
    ctor_fs.jpc = NO_JUMP;
    ctor_fs.nk = 0;
    ctor_fs.np = 0;
    ctor_fs.firstlocal = 0;
    ctor_fs.nlocvars = 0;
    ctor_fs.nactvar = 0;
    ctor_fs.nups = 0;
    ctor_fs.freereg = 0;
    ctor_fs.f->maxstacksize = 6;
    ctor_fs.f->is_vararg = 0;
    ctor_fs.f->numparams = 1;

    /* _ENV upvalue */
    {
      Proto *mf = ctor_fs.f;
      luaM_growvector(ls->L, mf->upvalues, ctor_fs.nups, mf->sizeupvalues,
                      Upvaldesc, MAXUPVAL, "upvalues");
      int oldsize = mf->sizeupvalues;
      while (oldsize < (int)mf->sizeupvalues)
        mf->upvalues[oldsize++].name = NULL;
      mf->upvalues[ctor_fs.nups].instack = 0;
      mf->upvalues[ctor_fs.nups].idx = 0;
      mf->upvalues[ctor_fs.nups].name = ls->envn;
      luaC_objbarrier(ls->L, mf, ls->envn);
      ctor_fs.nups++;
      mf->sizeupvalues = ctor_fs.nups;
    }

    FuncState *saved_fs = fs;
    ls->fs = &ctor_fs;
    JBlockCnt bl;
    enterblock(ls->fs, &bl, 0);

    /* 'this' param at R0 */
    new_localvar(ls->fs, luaS_newliteral(ls->L, "this"));

    luaK_ret(ls->fs, 0, 0);
    leaveblock(ls->fs);

    /* Finalize proto */
    ctor_fs.f->sizecode = ctor_fs.pc;
    ctor_fs.f->sizek = ctor_fs.nk;
    ctor_fs.f->sizep = ctor_fs.np;
    ctor_fs.f->sizelocvars = ctor_fs.nlocvars;
    ctor_fs.f->sizeupvalues = ctor_fs.nups;
    ctor_fs.f->linedefined = ctor_start_line;
    ctor_fs.f->lastlinedefined = ls->linenumber;
    ctor_fs.f->maxstacksize = ctor_fs.freereg;
    if ((int)ctor_fs.f->maxstacksize < (int)ctor_fs.f->numparams)
        ctor_fs.f->maxstacksize = ctor_fs.f->numparams;

    /* Register proto in parent */
    int proto_idx = saved_fs->np;
    luaM_growvector(ls->L, saved_fs->f->p, proto_idx, saved_fs->f->sizep,
                    Proto *, MAXARG_Bx, "functions");
    saved_fs->f->p[proto_idx] = proto;
    saved_fs->np++;

    ls->fs = saved_fs;

    /* OP_CLOSURE */
    int ctor_reg_val = ls->fs->freereg;
    luaK_codeABx(ls->fs, OP_CLOSURE, ctor_reg_val, (unsigned int)proto_idx);
    ls->fs->freereg = ctor_reg_val + 1;

    /* Store as class_table[ClassName] and class_table["new"] */
    int ck = luaK_stringK(ls->fs, class_name);
    luaK_codeABC(ls->fs, OP_SETTABLE, class_reg, ck | BITRK, ctor_reg_val);

    int nk = luaK_stringK(ls->fs, luaS_newliteral(ls->L, "new"));
    luaK_codeABC(ls->fs, OP_SETTABLE, class_reg, nk | BITRK, ctor_reg_val);

    ls->fs->freereg = ctor_reg_val; /* release closure register */
  }

}

/* ---------- ENTRY: main function ---------- */

static void parser_main(JLexState *ls, FuncState *fs) {
  JBlockCnt bl;
  enterblock(fs, &bl, 0);

  fs->f->is_vararg = 0;

  /* Create _ENV upvalue */
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

  /* skip package declaration */
  if (ls->t.token == TK_JAVA_PACKAGE) {
    next(ls);
    while (ls->t.token == TK_JAVA_NAME || ls->t.token == '.') next(ls);
    if (ls->t.token == ';') next(ls);
  }

  /* process import statements: generate require() calls */
  while (ls->t.token == TK_JAVA_IMPORT) {
    next(ls);  /* skip 'import' */
    int is_static = 0;
    if (ls->t.token == TK_JAVA_STATIC) { is_static = 1; next(ls); }

    /* Collect full qualified name: java.util.ArrayList */
    char full_path[256];
    int path_len = 0;
    while ((ls->t.token == TK_JAVA_NAME || ls->t.token == '.')
           && path_len < 250) {
      if (ls->t.token == '.') {
        full_path[path_len++] = '.';
      } else {
        const char *s = getstr(ls->t.seminfo.ts);
        size_t slen = strlen(s);
        if (path_len + (int)slen >= 250) break;
        memcpy(full_path + path_len, s, slen);
        path_len += (int)slen;
      }
      next(ls);
    }
    full_path[path_len] = '\0';

    int is_wildcard = (ls->t.token == '*');
    if (is_wildcard) { next(ls); /* skip '*' */ }

    if (ls->t.token == ';') next(ls);

    /* Skip static imports and empty paths */
    if (is_static || path_len == 0) continue;

    if (is_wildcard) {
      /* Wildcard import: generate _j_import_wildcard("pkg")
       * Bytecode:
       *   GETTABUP  reg, 0, "_j_import_wildcard"
       *   LOADK     reg+1, "pkg"
       *   CALL      reg, 2, 1
       */
      int reg = fs->freereg;
      fs->freereg = reg + 2;
      TString *ts_fn = luaS_newliteral(ls->L, "_j_import_wildcard");
      int k_fn = luaK_stringK(fs, ts_fn);
      luaK_codeABC(fs, OP_GETTABUP, reg, 0, k_fn | BITRK);
      TString *ts_pkg = luaS_newlstr(ls->L, full_path, (size_t)path_len);
      int k_pkg = luaK_stringK(fs, ts_pkg);
      luaK_codeABx(fs, OP_LOADK, reg + 1, k_pkg);
      luaK_codeABC(fs, OP_CALL, reg, 2, 2);
      continue;
    }

    /* Generate: _ENV[shortName] = require("full.path")
     * Bytecode:
     *   GETTABUP  reg, 0, "require"
     *   LOADK     reg+1, "full.path"
     *   CALL      reg, 1, 1
     *   SETTABUP  0, "shortName", reg
     */
    /* Extract short class name (last component after last '.') */
    char *short_name = full_path;
    for (int i = path_len - 1; i >= 0; i--) {
      if (full_path[i] == '.') { short_name = full_path + i + 1; break; }
    }
    size_t short_len = strlen(short_name);
    if (short_len == 0) continue;

    int reg = fs->freereg;
    fs->freereg = reg + 2;  /* we use reg and reg+1 */

    /* GETTABUP reg, _ENV(0), "require" */
    TString *ts_req = luaS_newliteral(ls->L, "require");
    int k_req = luaK_stringK(fs, ts_req);
    luaK_codeABC(fs, OP_GETTABUP, reg, 0, k_req | BITRK);

    /* LOADK reg+1, full_path  (iABx format, no BITRK) */
    TString *ts_path = luaS_newlstr(ls->L, full_path, (size_t)path_len);
    int k_path = luaK_stringK(fs, ts_path);
    luaK_codeABx(fs, OP_LOADK, reg + 1, k_path);

    /* CALL reg, 2, 2  →  reg = require(full_path)  (nargs=1, nrets=1) */
    luaK_codeABC(fs, OP_CALL, reg, 2, 2);

    /* SETTABUP _ENV(0), shortName, reg */
    TString *ts_name = luaS_newlstr(ls->L, short_name, short_len);
    int k_name = luaK_stringK(fs, ts_name);
    luaK_codeABC(fs, OP_SETTABUP, 0, k_name | BITRK, reg);

    fs->freereg = reg;  /* release temporaries */
  }

  /* create _MAIN_CANDIDATES = {} to collect all static main() functions */
  int main_cands_reg = fs->freereg;
  luaK_codeABC(fs, OP_NEWTABLE, main_cands_reg, 0, 0);
  {
    int k = luaK_stringK(fs, luaS_newliteral(ls->L, "_MAIN_CANDIDATES"));
    luaK_codeABC(fs, OP_SETTABUP, 0, k | BITRK, main_cands_reg);
  }
  fs->freereg = main_cands_reg + 1;  /* keep register allocated */

  /* parse compilation unit: class definitions */
  int has_main = 0;
  while (ls->t.token != TK_JAVA_EOS) {
    if (ls->t.token == TK_JAVA_CLASS) {
      class_definition(ls, fs, &has_main, main_cands_reg);
    } else if (ls->t.token == TK_JAVA_PUBLIC) {
      next(ls);
      if (ls->t.token == TK_JAVA_CLASS) {
        class_definition(ls, fs, &has_main, main_cands_reg);
      } else if (ls->t.token == TK_JAVA_INTERFACE || ls->t.token == TK_JAVA_ENUM) {
        /* skip public interface/enum for now */
        int brace_depth = 0;
        while (ls->t.token != TK_JAVA_EOS) {
          if (ls->t.token == '{') brace_depth++;
          else if (ls->t.token == '}') {
            if (brace_depth == 0) { next(ls); break; }
            brace_depth--;
          }
          next(ls);
        }
      } else {
        jlex_syntaxerror(ls, "expected 'class' after 'public'");
      }
    } else {
      jlex_syntaxerror(ls, "expected class definition");
    }
  }

  leaveblock(fs);
  luaK_ret(fs, 0, 0);  /* all classes registered, return nothing */
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
    mini_ls.lastline = 1;
    funcstate.ls = &mini_ls;
    parser_main(&lexstate, &funcstate);
  }

  /* 7. finalize proto */
  funcstate.f->sizecode = funcstate.pc;
  funcstate.f->sizek = funcstate.nk;
  funcstate.f->sizep = funcstate.np;
  funcstate.f->sizelocvars = funcstate.nlocvars;
  funcstate.f->sizeupvalues = funcstate.nups;
  funcstate.f->linedefined = 1;
  funcstate.f->lastlinedefined = lexstate.linenumber;
  funcstate.f->maxstacksize = funcstate.freereg > funcstate.f->maxstacksize
                                ? funcstate.freereg : funcstate.f->maxstacksize;

  L->top--;  /* pop scanner's string table */
  return cl;
}
