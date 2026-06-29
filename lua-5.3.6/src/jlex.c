/*
** Java Lexer for Lua VM  (jlex.c)
*/

#define LUA_CORE
#include "lprefix.h"
#include <string.h>

#include "lua.h"
#include "lctype.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lzio.h"
#include "llex.h"
#include "jlex.h"

#define next(ls)          ((ls)->current = zgetc((ls)->z))
#define currIsNewline(ls) ((ls)->current == '\n' || (ls)->current == '\r')

static const char *const jlex_tokens[] = {
    "class", "public", "private", "protected",
    "static", "void", "int", "String",
    "new", "return", "if", "else",
    "for", "while", "true", "false",
    "null", "this",
    "boolean", "final",
    /* new keywords */
    "package", "import", "char", "double",
    "enum", "switch", "case", "default",
    "break", "do", "try", "catch",
    "finally", "throw", "throws",
    "implements", "extends", "interface",
    "<integer>", "<float>", "<string>", "<name>",
    "<eof>"
};

/* Number of actual keyword entries (excludes the 5 pseudo-tokens at end) */
#define NJAVA_KW  (TK_JAVA_INTLIT - TK_JAVA_CLASS)

/* Keyword lookup table — independent of ts->extra to avoid
 * conflicts with Lua reserved words */
static struct {
    TString *ts;
    int token;
} jlex_kw[NJAVA_KW];

void jlex_init(lua_State *L) {
    int i;
    global_State *g = G(L);
    for (i = 0; i < NJAVA_KW; i++) {
        TString *ts = luaS_new(L, jlex_tokens[i]);
        jlex_kw[i].ts = ts;
        jlex_kw[i].token = TK_JAVA_CLASS + i;
        /* fix strings that Lua hasn't already fixed.
         * luaC_fix requires 'o' to be at the head of 'allgc'.
         * If 'ts' is a pre-existing string (e.g. "package" from
         * luaL_openlibs), it won't be at head — skip fixing it.
         * (Pre-existing strings have live references in Lua tables,
         * so they won't be collected during normal operation.) */
        if (ts->extra == 0 && g->allgc == obj2gco(ts)) {
            luaC_fix(L, obj2gco(ts));
        }
    }
}

/* Look up a string in the Java keyword table */
static int jlex_keyword(TString *ts) {
    int i;
    for (i = 0; i < NJAVA_KW; i++) {
        if (jlex_kw[i].ts == ts) return jlex_kw[i].token;
    }
    return TK_JAVA_NAME;
}

/* ---- buffer helpers ---- */
static void save(JLexState *ls, int c) {
    Mbuffer *b = ls->buff;
    if (luaZ_bufflen(b) + 1 > luaZ_sizebuffer(b)) {
        if (luaZ_sizebuffer(b) >= MAX_SIZE / 2)
            jlex_syntaxerror(ls, "lexical element too long");
        luaZ_resizebuffer(ls->L, b, luaZ_sizebuffer(b) * 2);
    }
    b->buffer[luaZ_bufflen(b)++] = cast(char, c);
}
#define save_and_next(ls)  (save(ls, (ls)->current), next(ls))

static void inclinenumber(JLexState *ls) {
    int old = ls->current;
    lua_assert(currIsNewline(ls));
    next(ls);
    if (currIsNewline(ls) && ls->current != old) next(ls);
    if (++ls->linenumber >= MAX_INT)
        jlex_syntaxerror(ls, "chunk has too many lines");
}

/* ---- string interning (same as luaX_newstring) ---- */
TString *jlex_newstring(JLexState *ls, const char *str, size_t l) {
    lua_State *L = ls->L;
    TString *ts = luaS_newlstr(L, str, l);
    TValue *o;
    setsvalue2s(L, L->top++, ts);
    o = luaH_set(L, ls->h, L->top - 1);
    if (ttisnil(o)) { setbvalue(o, 1); luaC_checkGC(L); }
    else ts = tsvalue(keyfromval(o));
    L->top--;
    return ts;
}

/* ---- error ---- */
static const char *token_name(lua_State *L, int token) {
    if (token < 256) return luaO_pushfstring(L, "'%c'", token);
    if (token == TK_JAVA_EQ)  return "'=='";
    if (token == TK_JAVA_NE)  return "'!='";
    if (token == TK_JAVA_LE)  return "'<='";
    if (token == TK_JAVA_GE)  return "'>='";
    if (token == TK_JAVA_AND) return "'&&'";
    if (token == TK_JAVA_OR)  return "'||'";
    if (token == TK_JAVA_INC) return "'++'";
    if (token == TK_JAVA_DEC) return "'--'";
    if (token >= TK_JAVA_CLASS && token < TK_JAVA_INTLIT)
        return luaO_pushfstring(L, "'%s'", jlex_tokens[token - TK_JAVA_CLASS]);
    if (token == TK_JAVA_EOS) return "'<eof>'";
    return luaO_pushfstring(L, "'%s'", jlex_tokens[token - TK_JAVA_CLASS]);
}

l_noret jlex_syntaxerror(JLexState *ls, const char *msg) {
    const char *tn = token_name(ls->L, ls->t.token);
    const char *full = luaO_pushfstring(ls->L, "%s near %s", msg, tn);
    full = luaG_addinfo(ls->L, full, ls->source, ls->linenumber);
    luaD_throw(ls->L, LUA_ERRSYNTAX);
}

/* ---- numeric ---- */
static int read_numeral(JLexState *ls, JavaSemInfo *seminfo) {
    TValue obj;
    int is_float = 0;
    const char *expo = "Ee";
    int first = ls->current;
    save_and_next(ls);
    if (first == '0' && (ls->current == 'x' || ls->current == 'X')) {
        expo = "Pp"; save_and_next(ls);
    }
    for (;;) {
        if (strchr(expo, ls->current)) {
            save_and_next(ls);
            if (ls->current == '-' || ls->current == '+') save_and_next(ls);
        }
        if (lisxdigit(ls->current)) save_and_next(ls);
        else if (ls->current == '.') { save_and_next(ls); is_float = 1; }
        else if (ls->current == 'f' || ls->current == 'F' ||
                 ls->current == 'd' || ls->current == 'D' ||
                 ls->current == 'l' || ls->current == 'L') {
            next(ls); is_float = 1; break;
        } else break;
    }
    save(ls, '\0');
    if (luaO_str2num(luaZ_buffer(ls->buff), &obj) == 0)
        jlex_syntaxerror(ls, "malformed number");
    if (ttisinteger(&obj) && !is_float) {
        seminfo->i = ivalue(&obj); return TK_JAVA_INTLIT;
    } else {
        seminfo->r = fltvalue(&obj); return TK_JAVA_FLOATLIT;
    }
}

/* ---- string literal ---- */
static void read_string(JLexState *ls, JavaSemInfo *seminfo) {
    int del = '"';
    save_and_next(ls);
    while (ls->current != del) {
        switch (ls->current) {
        case EOZ: jlex_syntaxerror(ls, "unfinished string");
        case '\n': case '\r': jlex_syntaxerror(ls, "unfinished string");
        case '\\': {
            int c;
            save_and_next(ls);
            switch (ls->current) {
            case 'a': c='\a'; goto s_save;
            case 'b': c='\b'; goto s_save;
            case 'f': c='\f'; goto s_save;
            case 'n': c='\n'; goto s_save;
            case 'r': c='\r'; goto s_save;
            case 't': c='\t'; goto s_save;
            case 'v': c='\v'; goto s_save;
            case '\\': case '"': case '\'': c=ls->current; goto s_save;
            case '0': case '1': case '2': case '3':
            case '4': case '5': case '6': case '7': {
                int i; c=0;
                for(i=0;i<3&&lisdigit(ls->current);i++){
                    c=10*c+ls->current-'0'; save_and_next(ls);
                }
                luaZ_buffremove(ls->buff,i+1); save(ls,c); goto s_nosave;
            }
            case 'x': {
                save_and_next(ls); c=0;
                if(lisxdigit(ls->current)){c=luaO_hexavalue(ls->current);save_and_next(ls);}
                if(lisxdigit(ls->current)){c=(c<<4)+luaO_hexavalue(ls->current);save_and_next(ls);}
                luaZ_buffremove(ls->buff,3); save(ls,c); goto s_nosave;
            }
            case 'u': {
                int i; unsigned long r=0;
                save_and_next(ls);
                for(i=0;i<4;i++){
                    if(!lisxdigit(ls->current)) jlex_syntaxerror(ls,"invalid unicode escape");
                    r=(r<<4)+luaO_hexavalue(ls->current); save_and_next(ls);
                }
                luaZ_buffremove(ls->buff,5);
                if(r<0x80)save(ls,(int)r);
                else if(r<0x800){save(ls,0xC0|(r>>6));save(ls,0x80|(r&0x3F));}
                else{save(ls,0xE0|(r>>12));save(ls,0x80|((r>>6)&0x3F));save(ls,0x80|(r&0x3F));}
                goto s_nosave;
            }
            default: jlex_syntaxerror(ls,"invalid escape sequence");
            }
        s_save: next(ls); luaZ_buffremove(ls->buff,1); save(ls,c);
        s_nosave: break;
        }
        default: save_and_next(ls);
        }
    }
    save_and_next(ls);
    seminfo->ts = jlex_newstring(ls, luaZ_buffer(ls->buff)+1, luaZ_bufflen(ls->buff)-2);
}

static int check_next1(JLexState *ls, int c) {
    if (ls->current == c) { next(ls); return 1; }
    return 0;
}

/* ---- main lex ---- */
static int jlex(JLexState *ls, JavaSemInfo *seminfo) {
    luaZ_resetbuffer(ls->buff);
    for (;;) {
        switch (ls->current) {
        case '\n': case '\r': inclinenumber(ls); break;
        case ' ': case '\t': case '\f': case '\v': next(ls); break;
        case '/': next(ls);
            if (ls->current == '/') { next(ls); while(!currIsNewline(ls)&&ls->current!=EOZ)next(ls); break; }
            if (ls->current == '*') { next(ls); for(;;){if(ls->current==EOZ)jlex_syntaxerror(ls,"unfinished comment");if(ls->current=='*'){next(ls);if(ls->current=='/'){next(ls);break;}}else if(currIsNewline(ls))inclinenumber(ls);else next(ls);} break; }
            if (ls->current == '=') { next(ls); return TK_JAVA_DIVEQ; }
            return '/';
        case '=': next(ls); if(check_next1(ls,'='))return TK_JAVA_EQ; return '=';
        case '!': next(ls); if(check_next1(ls,'='))return TK_JAVA_NE; return '!';
        case '<': next(ls); if(check_next1(ls,'='))return TK_JAVA_LE; return '<';
        case '>': next(ls); if(check_next1(ls,'='))return TK_JAVA_GE; return '>';
        case '&': next(ls); if(check_next1(ls,'&'))return TK_JAVA_AND; return '&';
        case '|': next(ls); if(check_next1(ls,'|'))return TK_JAVA_OR; return '|';
        case '+': next(ls); if(check_next1(ls,'+'))return TK_JAVA_INC; if(check_next1(ls,'='))return TK_JAVA_PLUSEQ; return '+';
        case '-': next(ls); if(check_next1(ls,'-'))return TK_JAVA_DEC; if(check_next1(ls,'='))return TK_JAVA_MINEQ; return '-';
        case '*': next(ls); if(check_next1(ls,'='))return TK_JAVA_MULEQ; return '*';
        case '%': next(ls); if(check_next1(ls,'='))return TK_JAVA_MODEQ; return '%';
        case '"': read_string(ls, seminfo); return TK_JAVA_STRLIT;
        case '\'': {
            int c;
            next(ls);
            if (ls->current == '\\') {
                next(ls);
                switch(ls->current){case'n':c='\n';break;case't':c='\t';break;case'r':c='\r';break;case'\\':c='\\';break;case'\'':c='\'';break;case'0':c='\0';break;default:c=ls->current;break;}
            } else c = ls->current;
            next(ls);
            if (ls->current != '\'') jlex_syntaxerror(ls, "unclosed char literal");
            next(ls);
            seminfo->i = c; return TK_JAVA_INTLIT;
        }
        case '.': save_and_next(ls); if(lisdigit(ls->current)) return read_numeral(ls,seminfo); return '.';
        case '0':case'1':case'2':case'3':case'4':case'5':case'6':case'7':case'8':case'9':
            return read_numeral(ls,seminfo);
        case EOZ: return TK_JAVA_EOS;
        default:
            if (lislalpha(ls->current) || ls->current == '_') {
                TString *ts;
                do { save_and_next(ls); } while (lislalnum(ls->current) || ls->current == '_');
                ts = jlex_newstring(ls, luaZ_buffer(ls->buff), luaZ_bufflen(ls->buff));
                seminfo->ts = ts;
                { int kw = jlex_keyword(ts);
                  if (kw != TK_JAVA_NAME) return kw; }
                return TK_JAVA_NAME;
            } else { int c = ls->current; next(ls); return c; }
        }
    }
}

void jlex_setinput(lua_State *L, JLexState *ls, ZIO *z, TString *source, int firstchar) {
    ls->t.token = 0; ls->L = L; ls->current = firstchar; ls->lookahead.token = TK_JAVA_EOS;
    ls->z = z; ls->fs = NULL; ls->linenumber = 1; ls->lastline = 1; ls->source = source;
    ls->envn = luaS_newliteral(L, "_ENV");
    luaZ_resizebuffer(ls->L, ls->buff, LUA_MINBUFFER);
}

void jlex_next(JLexState *ls) {
    ls->lastline = ls->linenumber;
    if (ls->fs && ls->fs->ls)
        ls->fs->ls->lastline = ls->linenumber;
    if (ls->lookahead.token != TK_JAVA_EOS) {
        ls->t = ls->lookahead; ls->lookahead.token = TK_JAVA_EOS;
    } else ls->t.token = jlex(ls, &ls->t.seminfo);
}

int jlex_lookahead(JLexState *ls) {
    lua_assert(ls->lookahead.token == TK_JAVA_EOS);
    ls->lookahead.token = jlex(ls, &ls->lookahead.seminfo);
    return ls->lookahead.token;
}
