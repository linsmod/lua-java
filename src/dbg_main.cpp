/* ============================================================
 * dbg_main.cpp - Lua Debugger UI using ImGui + GLFW + OpenGL3
 *
 * A graphical debugger for Lua 5.3.6 scripts with:
 *   - Source code view with breakpoints and current-line highlighting
 *   - Bytecode / disassembly view
 *   - Call stack inspection
 *   - Local variables and upvalues inspection
 *   - Step Over / Step Into / Step Out / Continue / Stop
 *   - Interactive console for evaluating expressions while paused
 *   - File loader dialog
 *
 * Architecture:
 *   Lua scripts run in a coroutine (lua_newthread).
 *   A debug hook (LUA_MASKLINE) yields the coroutine when a breakpoint
 *   or step condition is met.  The main loop detects LUA_YIELD,
 *   renders the debugger UI, and resumes the coroutine on "Continue".
 * ============================================================ */

#define LUA_CORE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <set>
#include <unordered_set>
#include <map>
#include <algorithm>
#include <deque>
#include <sys/stat.h>
#include <sys/types.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lstate.h"
#include "lopcodes.h"
#include "lobject.h"
#include "lstring.h"
}

/* ---- ImGui ---- */
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"
#include <GLFW/glfw3.h>

/* ---- Java bridge declarations ---- */
extern "C" {
LUAI_FUNC void jlex_init    (lua_State *L);
LUAI_FUNC void java_openlib (lua_State *L);
}

/* ============================================================
 *  Debugger state
 * ============================================================ */
enum DbgState {
    DBG_IDLE,       /* no script loaded / execution finished */
    DBG_RUNNING,    /* running freely */
    DBG_STEPPING,   /* step-over / step-into / step-out in progress */
    DBG_PAUSED      /* stopped at a breakpoint or manual pause */
};

static DbgState   g_dbg_state = DBG_IDLE;
static lua_State *g_mainL    = nullptr;  /* main Lua state */
static lua_State *g_co       = nullptr;  /* debuggee coroutine */

/* Step granularity */
enum StepMode { STEP_NONE, STEP_OVER, STEP_INTO, STEP_OUT };
static StepMode g_step_mode    = STEP_NONE;
static int      g_step_target  = 0;       /* stack depth for step-out */

/* "Break on main" – pause on first executable line after load */
static bool g_break_on_main   = true;
static bool g_pause_on_entry  = false;    /* set on load, cleared on first pause */

/* Exit code of last script execution */
static int  g_last_exit_code  = 0;
static bool g_has_exit_code   = false;

/* Recent file list (most recent first, max 10 entries, no duplicates) */
static std::deque<std::string> g_recent_files;
static const int RECENT_MAX = 10;

static void recent_add(const char *path) {
    if (!path || !*path) return;
    std::string s(path);
    /* Remove existing duplicate */
    for (auto it = g_recent_files.begin(); it != g_recent_files.end(); ++it) {
        if (*it == s) { g_recent_files.erase(it); break; }
    }
    g_recent_files.push_front(s);
    while ((int)g_recent_files.size() > RECENT_MAX)
        g_recent_files.pop_back();
}

/* Breakpoints: filename → (line → enabled) */
static std::map<std::string, std::map<int, bool>> g_breakpoints;
/* Selection state for breakpoints panel (per-frame, not persisted) */
static std::map<std::string, std::map<int, bool>> g_bp_select;

/* Currently paused location */
static std::string g_cur_source;
static int         g_cur_line = -1;

/* Locals/upvalues cache — captured once on pause, never queried from
 * the yielded coroutine on subsequent frames (avoids stack corruption). */
struct TableChild {
    std::string key;        /* display key string */
    std::string key_type;   /* "integer", "string", etc. */
    std::string value;      /* display value string */
    std::string value_type; /* "integer", "string", "table", etc. */
    const void *table_ptr;  /* non-null if value is itself a table */
    bool        expanded;
    std::vector<TableChild> children;
};

struct LocalEntry {
    std::string name;
    std::string value;
    const void *table_ptr;          /* non-null if value is a table */
    bool        table_expanded;
    std::vector<TableChild> table_children;
};
static std::vector<LocalEntry> g_locals_cache;
static std::vector<LocalEntry> g_upvals_cache;

/* Call stack cache — captured once on pause */
struct StackEntry {
    std::string name;
    std::string source;
    int         line;
};
static std::vector<StackEntry> g_callstack_cache;
static int g_selected_frame = 0;  /* which call-stack frame to focus */

/* Bytecode view cache — captured once on pause (Proto* is safe: GC stopped) */
static const void *g_bytecode_proto = nullptr;
static int         g_bytecode_curpc = -1;
static const void *g_bytecode_ci    = nullptr;  /* to validate cache validity */

/* Registers cache — captured once on pause */
struct RegisterEntry {
    int         index;
    std::string type;
    std::string value;
    bool        is_top;     /* R(top) marker */
    bool        is_params;  /* parameter registers R0..R(params-1) */
    const void *table_ptr;          /* non-null if value is a table */
    bool        table_expanded;
    std::vector<TableChild> table_children;
};
static std::vector<RegisterEntry> g_registers_cache;

/* Loaded source file cache: filename → vector of lines */
static std::map<std::string, std::vector<std::string>> g_sources;

/* Console output log */
struct ConsoleLine {
    std::string text;
    ImVec4      color;
};
static std::deque<ConsoleLine> g_console;
static const int   CONSOLE_MAX = 500;

/* Console input buffer */
static char g_console_input[4096] = {0};

/* "Load file" dialog */
static char g_filepath_buf[512] = {0};

/* Window visibility toggles (for View menu) */
static bool g_show_source     = true;
static bool g_show_bytecode   = true;
static bool g_show_callstack  = true;
static bool g_show_locals     = true;
static bool g_show_registers  = true;
static bool g_show_console    = true;
static bool g_show_controls    = true;
static bool g_show_breakpoints = true;

/* Pending layout-reset flag (set by menu, handled at dockspace setup) */
static bool g_pending_layout_reset = false;

/* File > Open modal */
static bool g_open_file_modal = false;

/* GLFW window pointer (needed by menu Exit action) */
static GLFWwindow *g_window = nullptr;
static char g_open_file_buf[512] = {0};

/* Native file dialog support (zenity on Linux) */
static bool g_zenity_available = false;
static bool g_zenity_checked   = false;

/*
 * Open a native file selection dialog and return the chosen path.
 * Returns empty string if cancelled or unavailable.
 */
static std::string native_file_dialog() {
    /* Check if zenity is available (once) */
    if (!g_zenity_checked) {
        FILE *test = popen("zenity --version 2>/dev/null", "r");
        if (test) {
            char buf[64] = {0};
            if (fgets(buf, sizeof(buf), test) && strlen(buf) > 0) {
                g_zenity_available = true;
            }
            pclose(test);
        }
        g_zenity_checked = true;
    }

    if (!g_zenity_available) return "";

    /* Build file filter: Lua and Java files */
    const char *cmd =
        "zenity --file-selection"
        " --title=\"Open Script\""
        " --file-filter='Lua/Java scripts (*.lua *.java) | *.lua *.java'"
        " --file-filter='All files | *'"
        " 2>/dev/null";

    FILE *fp = popen(cmd, "r");
    if (!fp) return "";

    char result[1024] = {0};
    if (fgets(result, sizeof(result), fp)) {
        /* Strip trailing newline */
        size_t len = strlen(result);
        while (len > 0 && (result[len-1] == '\n' || result[len-1] == '\r'))
            result[--len] = '\0';
        pclose(fp);
        return std::string(result);
    }
    pclose(fp);
    return "";
}

/* Helper: add a line to console */
static void console_add(const char *s, ImVec4 color = ImVec4(1,1,1,1)) {
    if (g_console.size() >= (size_t)CONSOLE_MAX) g_console.pop_front();
    ConsoleLine cl;
    cl.text  = s ? s : "";
    cl.color = color;
    g_console.push_back(cl);

    /* Also print to stdout for terminal visibility */
    const char *text = s ? s : "";
    if (color.x > 0.9f && color.y < 0.4f && color.z < 0.4f) {
        /* Red-ish = error */
        fprintf(stdout, "[ERROR] %s\n", text);
    } else if (color.x > 0.9f && color.y > 0.7f && color.z < 0.4f) {
        /* Orange/Yellow = warning */
        fprintf(stdout, "[WARN]  %s\n", text);
    } else if (color.x < 0.6f && color.y > 0.7f && color.z > 0.9f) {
        /* Blue-ish = info */
        fprintf(stdout, "[INFO]  %s\n", text);
    } else {
        fprintf(stdout, "%s\n", text);
    }
    fflush(stdout);
}

/* Redirect Lua print() to our console */
static int lua_print_hook(lua_State *L) {
    int n = lua_gettop(L);
    lua_getglobal(L, "tostring");
    std::string line;
    for (int i = 1; i <= n; i++) {
        lua_pushvalue(L, -1);  /* tostring */
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        const char *s = lua_tostring(L, -1);
        if (s) {
            if (i > 1) line += "\t";
            line += s;
        }
        lua_pop(L, 1);
    }
    console_add(line.c_str(), ImVec4(0.7f, 1.0f, 0.7f, 1.0f));
    return 0;
}

/* ============================================================
 *  Breakpoint helpers
 * ============================================================ */
static bool is_breakpoint(const char *source, int line) {
    auto it = g_breakpoints.find(source);
    if (it == g_breakpoints.end()) return false;
    auto jt = it->second.find(line);
    if (jt == it->second.end()) return false;
    return jt->second;  /* true = enabled */
}

static void toggle_breakpoint(const char *source, int line) {
    auto &file_bps = g_breakpoints[source];
    auto it = file_bps.find(line);
    if (it != file_bps.end()) {
        /* Exists: remove it */
        file_bps.erase(it);
        if (file_bps.empty())
            g_breakpoints.erase(source);
        console_add(("Breakpoint removed at " + std::string(source) +
                     ":" + std::to_string(line)).c_str(),
                    ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
    } else {
        /* New: add enabled */
        file_bps[line] = true;
        console_add(("Breakpoint set at " + std::string(source) +
                     ":" + std::to_string(line)).c_str(),
                    ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
    }
}

/* Enable/disable a single breakpoint */
static void bp_set_enabled(const std::string &file, int line, bool on) {
    auto it = g_breakpoints.find(file);
    if (it != g_breakpoints.end()) {
        auto jt = it->second.find(line);
        if (jt != it->second.end())
            jt->second = on;
    }
}

/* Delete a single breakpoint */
static void bp_delete(const std::string &file, int line) {
    auto it = g_breakpoints.find(file);
    if (it != g_breakpoints.end()) {
        it->second.erase(line);
        if (it->second.empty())
            g_breakpoints.erase(it);
    }
}

/* ============================================================
 *  Snapshot locals & upvalues (called once from debug_hook
 *  before yielding — never from the main-loop draw functions)
 * ============================================================ */
static const char *lua_val_tostring(lua_State *L, int idx, char *buf, size_t bufsz) {
    int t = lua_type(L, idx);
    switch (t) {
    case LUA_TNIL:        return "nil";
    case LUA_TBOOLEAN:    return lua_toboolean(L, idx) ? "true" : "false";
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx))
            snprintf(buf, bufsz, "%lld", (long long)lua_tointeger(L, idx));
        else
            snprintf(buf, bufsz, "%g", lua_tonumber(L, idx));
        return buf;
    case LUA_TSTRING:     snprintf(buf, bufsz, "\"%s\"", lua_tostring(L, idx)); return buf;
    case LUA_TTABLE:      snprintf(buf, bufsz, "table(%p)", lua_topointer(L, idx)); return buf;
    case LUA_TFUNCTION:
        if (lua_iscfunction(L, idx))
            snprintf(buf, bufsz, "function(C:%p)", lua_topointer(L, idx));
        else
            snprintf(buf, bufsz, "function(%p)", lua_topointer(L, idx));
        return buf;
    case LUA_TUSERDATA:       snprintf(buf, bufsz, "userdata(%p)", lua_topointer(L, idx)); return buf;
    case LUA_TLIGHTUSERDATA:  snprintf(buf, bufsz, "lightuserdata(%p)", lua_topointer(L, idx)); return buf;
    case LUA_TTHREAD:         snprintf(buf, bufsz, "thread(%p)", lua_topointer(L, idx)); return buf;
    default:                  snprintf(buf, bufsz, "type:%d", t); return buf;
    }
}

static void snapshot_locals(lua_State *L, lua_Debug *ar) {
    g_locals_cache.clear();
    g_upvals_cache.clear();

    char valbuf[512];

    /* Locals */
    for (int i = 1; ; i++) {
        const char *name = lua_getlocal(L, ar, i);
        if (!name) break;
        const char *val = lua_val_tostring(L, -1, valbuf, sizeof(valbuf));
        LocalEntry e;
        e.name            = name;
        e.value           = val;
        e.table_ptr       = (lua_type(L, -1) == LUA_TTABLE) ? lua_topointer(L, -1) : nullptr;
        e.table_expanded  = false;
        g_locals_cache.push_back(e);
        lua_pop(L, 1);
    }

    /* Upvalues */
    lua_getinfo(L, "f", ar);
    int func_idx = lua_gettop(L);
    for (int i = 1; ; i++) {
        const char *name = lua_getupvalue(L, func_idx, i);
        if (!name) break;
        const char *val = lua_val_tostring(L, -1, valbuf, sizeof(valbuf));
        LocalEntry e;
        e.name            = name;
        e.value           = val;
        e.table_ptr       = (lua_type(L, -1) == LUA_TTABLE) ? lua_topointer(L, -1) : nullptr;
        e.table_expanded  = false;
        g_upvals_cache.push_back(e);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);  /* pop function */
}

static void snapshot_callstack(lua_State *L) {
    g_callstack_cache.clear();

    lua_Debug ar;
    for (int level = 0; lua_getstack(L, level, &ar); level++) {
        lua_getinfo(L, "nSl", &ar);
        g_callstack_cache.push_back({
            ar.name ? ar.name : "?",
            ar.short_src[0] ? ar.short_src : "?",
            ar.currentline
        });
    }
}

static void snapshot_bytecode(lua_State *L) {
    g_bytecode_proto = nullptr;
    g_bytecode_curpc = -1;
    g_bytecode_ci    = nullptr;

    if (!L->ci || !isLua(L->ci)) return;
    g_bytecode_ci = L->ci;

    LClosure *cl = clLvalue(L->ci->func);
    const Proto *p = cl->p;
    g_bytecode_proto = p;

    int curpc = (int)(L->ci->u.l.savedpc - p->code) - 1;
    g_bytecode_curpc = curpc;
}

/* ---- Helper: format a raw TValue into a display string ---- */
static std::string tvalue_tostring(const TValue *o) {
    char buf[256];
    if (ttisnil(o)) {
        return "nil";
    } else if (ttisboolean(o)) {
        return bvalue(o) ? "true" : "false";
    } else if (ttisinteger(o)) {
        snprintf(buf, sizeof(buf), "%lld", (long long)ivalue(o));
        return buf;
    } else if (ttisfloat(o)) {
        snprintf(buf, sizeof(buf), "%.14g", fltvalue(o));
        return buf;
    } else if (ttisstring(o)) {
        const char *s = getstr(tsvalue(o));
        size_t len = tsslen(tsvalue(o));
        if (len > 80) {
            snprintf(buf, sizeof(buf), "\"%.80s...\"", s);
        } else {
            snprintf(buf, sizeof(buf), "\"%s\"", s);
        }
        return buf;
    } else if (ttistable(o)) {
        snprintf(buf, sizeof(buf), "table(%p)", (void *)hvalue(o));
        return buf;
    } else if (ttisLclosure(o)) {
        snprintf(buf, sizeof(buf), "function(%p)", (void *)clLvalue(o));
        return buf;
    } else if (ttisCclosure(o)) {
        snprintf(buf, sizeof(buf), "C-function(%p)", (void *)clCvalue(o));
        return buf;
    } else if (ttislcf(o)) {
        snprintf(buf, sizeof(buf), "C-function(%p)", (void *)fvalue(o));
        return buf;
    } else if (ttisfulluserdata(o)) {
        snprintf(buf, sizeof(buf), "userdata(%p)", uvalue(o));
        return buf;
    } else if (ttislightuserdata(o)) {
        snprintf(buf, sizeof(buf), "lightuserdata(%p)", pvalue(o));
        return buf;
    } else if (ttisthread(o)) {
        snprintf(buf, sizeof(buf), "thread(%p)", (void *)thvalue(o));
        return buf;
    } else {
        snprintf(buf, sizeof(buf), "type:%d", rttype(o));
        return buf;
    }
}

static const char *tvalue_typename(const TValue *o) {
    if (ttisnil(o))           return "nil";
    if (ttisboolean(o))       return "boolean";
    if (ttisinteger(o))       return "integer";
    if (ttisfloat(o))         return "number";
    if (ttisstring(o))        return "string";
    if (ttistable(o))         return "table";
    if (ttisLclosure(o))      return "function";
    if (ttisCclosure(o))      return "C-func";
    if (ttislcf(o))           return "C-func";
    if (ttisfulluserdata(o))  return "userdata";
    if (ttislightuserdata(o)) return "lightud";
    if (ttisthread(o))        return "thread";
    return "???";
}

/* ---- table expansion helpers (direct memory read — GC is stopped during pause) ---- */
static const int MAX_TABLE_ENTRIES = 128;
static const int MAX_TABLE_DEPTH   = 3;

/* forward-declare for recursion */
static void read_table_entries(const void *tptr, std::vector<TableChild> &out,
                                std::unordered_set<const void*> &visited,
                                int depth);
static void expand_table_child(TableChild &child, int depth);

static void read_table_entries(const void *tptr, std::vector<TableChild> &out,
                                std::unordered_set<const void*> &visited,
                                int depth) {
    const Table *t = (const Table*)tptr;
    if (!t || depth <= 0) return;
    if (visited.count(tptr)) return;
    visited.insert(tptr);

    int count = 0;

    /* ---- array part: indices 1..sizearray ---- */
    for (unsigned int i = 0; i < t->sizearray && count < MAX_TABLE_ENTRIES; i++) {
        TValue *v = &t->array[i];
        if (ttisnil(v)) continue;

        TableChild ch;
        char kbuf[32];
        snprintf(kbuf, sizeof(kbuf), "%u", i + 1);
        ch.key        = kbuf;
        ch.key_type   = "integer";
        ch.value_type = tvalue_typename(v);
        ch.value      = tvalue_tostring(v);
        ch.table_ptr  = ttistable(v) ? (const void*)hvalue(v) : nullptr;
        ch.expanded   = false;
        out.push_back(ch);
        count++;
    }

    /* ---- hash part ---- */
    int node_count = 1 << (int)t->lsizenode;
    for (int i = 0; i < node_count && count < MAX_TABLE_ENTRIES; i++) {
        Node *n = (Node*)&t->node[i];
        TValue *k = &n->i_key.tvk;
        if (ttisnil(k)) continue;  /* free slot */

        TableChild ch;
        ch.key_type = tvalue_typename(k);

        if (ttisinteger(k)) {
            char kbuf[32];
            snprintf(kbuf, sizeof(kbuf), "%lld", (long long)ivalue(k));
            ch.key = kbuf;
        } else if (ttisstring(k)) {
            const char *s = getstr(tsvalue(k));
            size_t slen = tsslen(tsvalue(k));
            if (slen > 40) {
                char kbuf[64];
                snprintf(kbuf, sizeof(kbuf), "\"%.40s...\"", s);
                ch.key = kbuf;
            } else {
                ch.key = "\"";
                ch.key += s;
                ch.key += "\"";
            }
        } else {
            ch.key = "[" + tvalue_tostring(k) + "]";
        }

        TValue *v = &n->i_val;
        ch.value_type = tvalue_typename(v);
        ch.value      = tvalue_tostring(v);
        ch.table_ptr  = ttistable(v) ? (const void*)hvalue(v) : nullptr;
        ch.expanded   = false;
        out.push_back(ch);
        count++;
    }
}

static void expand_table_child(TableChild &child, int depth) {
    if (!child.table_ptr || !child.children.empty()) return;
    std::unordered_set<const void*> visited;
    read_table_entries(child.table_ptr, child.children, visited, depth);
}

static void expand_entry(void *tptr, std::vector<TableChild> &children, int depth) {
    if (!tptr || !children.empty()) return;
    std::unordered_set<const void*> visited;
    read_table_entries(tptr, children, visited, depth);
}

static void snapshot_registers(lua_State *L) {
    g_registers_cache.clear();

    CallInfo *ci = L->ci;
    if (!ci) return;
    if (!isLua(ci)) {
        /* When ci is not a Lua frame (e.g. C call or yield boundary),
         * try the next ci down the chain — the hook may fire while
         * the topmost ci is a C frame wrapping the actual Lua function. */
        ci = ci->previous;
        if (!ci || !isLua(ci)) return;
    }
    LClosure *cl = clLvalue(ci->func);
    const Proto *p = cl->p;
    StkId base = ci->u.l.base;

    /* Use top-base as the real register count when maxstacksize
     * is too small (Java-compiled functions often have maxstacksize=0
     * because the compiler resets freereg at function end). */
    int nregs = p->maxstacksize;
    int top_nregs = (int)(L->top - base);
    if (top_nregs > nregs) nregs = top_nregs;
    /* At minimum, show parameter registers. maxstacksize=0 doesn't
     * mean there are no parameters — Lua fills param slots from caller. */
    if (nregs < (int)p->numparams) nregs = (int)p->numparams;

    for (int i = 0; i < nregs; i++) {
        TValue *reg = &base[i];
        RegisterEntry e;
        e.index          = i;
        e.type           = tvalue_typename(reg);
        e.value          = tvalue_tostring(reg);
        e.is_top         = (reg == L->top);
        e.is_params      = (i < p->numparams);
        e.table_ptr      = ttistable(reg) ? (const void*)hvalue(reg) : nullptr;
        e.table_expanded = false;
        g_registers_cache.push_back(e);
    }
}

/* ============================================================
 *  Lua debug hook – called on every source line
 * ============================================================ */
static void debug_hook(lua_State *L, lua_Debug *ar) {
    /* Only act on LINE events */
    if (ar->event != LUA_HOOKLINE) return;

    lua_getinfo(L, "Sl", ar);
    const char *src = ar->source ? ar->source : "?";
    int line = ar->currentline;

    /* If we encounter a new source, cache it + strip leading '@' */
    std::string src_key = (src[0] == '@') ? (src + 1) : src;

    switch (g_dbg_state) {
    case DBG_RUNNING:
        /* "Break on main": pause on the very first line executed */
        if (g_pause_on_entry) {
            g_pause_on_entry = false;
            g_dbg_state = DBG_PAUSED;
            g_cur_source = src_key;
            g_cur_line   = line;
            console_add(("Entry point reached — " + src_key + ":" +
                         std::to_string(line)).c_str(),
                        ImVec4(0.5f, 0.8f, 1.0f, 1.0f));
            break;
        }
        /* Check breakpoints */
        if (is_breakpoint(src_key.c_str(), line) ||
            is_breakpoint(src, line)) {
            g_dbg_state = DBG_PAUSED;
            g_cur_source = src_key;
            g_cur_line   = line;
        }
        break;

    case DBG_STEPPING:
        switch (g_step_mode) {
        case STEP_OVER: {
            /* Pause at same/shallower depth, OR at any breakpoint deeper */
            int depth = 0;
            lua_Debug ar2;
            for (int i = 0; lua_getstack(L, i, &ar2); i++) depth++;
            if (depth <= g_step_target ||
                is_breakpoint(src_key.c_str(), line) ||
                is_breakpoint(src, line)) {
                g_dbg_state = DBG_PAUSED;
                g_cur_source = src_key;
                g_cur_line   = line;
            }
            break;
        }
        case STEP_INTO:
            /* Pause on any next line */
            g_dbg_state = DBG_PAUSED;
            g_cur_source = src_key;
            g_cur_line   = line;
            break;
        case STEP_OUT: {
            /* Pause when shallower than target depth, OR at breakpoint */
            int depth = 0;
            lua_Debug ar2;
            for (int i = 0; lua_getstack(L, i, &ar2); i++) depth++;
            if (depth < g_step_target ||
                is_breakpoint(src_key.c_str(), line) ||
                is_breakpoint(src, line)) {
                g_dbg_state = DBG_PAUSED;
                g_cur_source = src_key;
                g_cur_line   = line;
            }
            break;
        }
        default:
            g_dbg_state = DBG_PAUSED;
            g_cur_source = src_key;
            g_cur_line   = line;
            break;
        }
        break;

    case DBG_PAUSED:
        /* Stay paused – yield the coroutine */
        break;

    case DBG_IDLE:
        break;
    }

    /* Yield if paused (inside coroutine) */
    if (g_dbg_state == DBG_PAUSED) {
        /* Snapshot locals & callstack once before yielding — draw functions
         * read from cache so the yielded coroutine's stack is never touched
         * during rendering (avoids heap-buffer-overflow / use-after-free). */
        lua_getstack(L, 0, ar);  /* ensure ar->i_ci is valid */
        snapshot_locals(L, ar);
        snapshot_callstack(L);
        snapshot_bytecode(L);
        snapshot_registers(L);
        lua_yield(L, 0);
    }
}

/* ============================================================
 *  Source file loading & caching
 * ============================================================ */
static void load_source_file(const char *filename) {
    if (g_sources.count(filename)) return;  /* already loaded */

    FILE *f = fopen(filename, "r");
    if (!f) return;

    std::vector<std::string> lines;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';
        lines.push_back(std::string(buf));
    }
    fclose(f);
    g_sources[filename] = lines;
}

/* ============================================================
 *  Forward declarations
 * ============================================================ */
static void dbg_stop();

/* ============================================================
 *  Load & run a script in the debuggee coroutine
 * ============================================================ */
static bool dbg_load_script(const char *filename) {
    /* Always stop old execution first — loading a new chunk onto a yielded
     * coroutine would leave stale CallInfo pointers to freed Proto objects,
     * causing heap-buffer-overflow on next lua_resume. */
    dbg_stop();

    /* Load the file */
    int status;
    size_t len = strlen(filename);
    bool is_java = (len > 5 && strcmp(filename + len - 5, ".java") == 0);

    if (is_java) {
        status = luaL_loadjava(g_co, filename);
    } else {
        status = luaL_loadfile(g_co, filename);
    }

    if (status != LUA_OK) {
        console_add(lua_tostring(g_co, -1), ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        lua_pop(g_co, 1);
        g_dbg_state = DBG_IDLE;
        return false;
    }

    /* Cache source file for display */
    load_source_file(filename);

    /* Add to recent files list */
    recent_add(filename);

    console_add(("Loaded: " + std::string(filename)).c_str(),
                ImVec4(0.5f, 0.8f, 1.0f, 1.0f));

    /* If "break on main" is enabled, pause on first executable line */
    g_pause_on_entry = g_break_on_main;

    /* Set the debug hook on the coroutine (LINE only to avoid flooding locals) */
    lua_sethook(g_co, debug_hook,
                LUA_MASKLINE, 0);

    g_dbg_state = DBG_RUNNING;
    g_cur_source.clear();
    g_cur_line = -1;
    g_selected_frame = 0;
    g_has_exit_code = false;
    return true;
}

/* ============================================================
 *  Execution control: resume the coroutine for one "tick"
 *  Returns true if the coroutine is still alive.
 * ============================================================ */
static bool dbg_tick() {
    if (!g_co || g_dbg_state == DBG_IDLE) return false;

    int status = lua_resume(g_co, g_mainL, 0);

    switch (status) {
    case LUA_OK: {
        /* Coroutine finished normally */
        g_dbg_state = DBG_IDLE;
        lua_sethook(g_co, nullptr, 0, 0);

        /* Dump script return values for info, then discard them. */
        int nres = lua_gettop(g_mainL);
        for (int i = 1; i <= nres; i++) {
            if (!lua_isnoneornil(g_mainL, i)) {
                const char *r = luaL_tolstring(g_mainL, i, nullptr);
                if (r) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "return[%d]: %s", i, r);
                    console_add(buf, ImVec4(0.7f, 1.0f, 0.7f, 1.0f));
                }
                lua_pop(g_mainL, 1);
            }
        }

        /* Call global main() for the real exit code. */
        bool have_ec = false;
        int  ec      = 0;
        lua_getglobal(g_mainL, "main");
        if (lua_isfunction(g_mainL, -1)) {
            console_add("Calling main()...",
                        ImVec4(0.5f, 0.8f, 1.0f, 1.0f));
            if (lua_pcall(g_mainL, 0, 1, 0) == LUA_OK) {
                have_ec = true;
                if (!lua_isnoneornil(g_mainL, -1)) {
                    if (lua_isinteger(g_mainL, -1))
                        ec = (int)lua_tointeger(g_mainL, -1);
                    /* else: non-integer return → exit 0 */
                    const char *r = luaL_tolstring(g_mainL, -1, nullptr);
                    console_add(r ? r : "(nil return)",
                                ImVec4(0.7f, 1.0f, 0.7f, 1.0f));
                    lua_pop(g_mainL, 1);
                }
                lua_pop(g_mainL, 1);
            } else {
                const char *err = lua_tostring(g_mainL, -1);
                console_add(err ? err : "Unknown error",
                            ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                luaL_traceback(g_mainL, g_mainL, err ? err : "error", 1);
                console_add(lua_tostring(g_mainL, -1),
                            ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                lua_pop(g_mainL, 2);
            }
        } else {
            lua_pop(g_mainL, 1);
        }

        g_has_exit_code = have_ec;
        g_last_exit_code = ec;
        {
            char buf[80];
            if (have_ec)
                snprintf(buf, sizeof(buf), "--- Script finished (exit %d) ---", ec);
            else
                snprintf(buf, sizeof(buf), "--- Script finished ---");
            console_add(buf, ImVec4(0.5f, 0.8f, 1.0f, 1.0f));
        }

        lua_settop(g_mainL, 0);
        return false;
    }

    case LUA_YIELD:
        /* Coroutine yielded (paused at breakpoint/step) */
        return true;

    case LUA_ERRRUN:
    case LUA_ERRSYNTAX:
    case LUA_ERRMEM:
    case LUA_ERRERR: {
        const char *msg = lua_tostring(g_co, -1);
        console_add(msg ? msg : "Unknown error",
                    ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        /* print traceback */
        luaL_traceback(g_co, g_co, msg ? msg : "error", 1);
        console_add(lua_tostring(g_co, -1),
                    ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        lua_pop(g_co, 1);
        g_dbg_state = DBG_IDLE;
        lua_sethook(g_co, nullptr, 0, 0);
        return false;
    }
    default:
        return false;
    }
}

/* ============================================================
 *  Debugger actions (called from ImGui buttons)
 * ============================================================ */
static void dbg_continue() {
    if (g_dbg_state == DBG_PAUSED) {
        g_dbg_state = DBG_RUNNING;
        g_step_mode = STEP_NONE;
    }
}

static void dbg_step_over() {
    if (g_dbg_state == DBG_PAUSED) {
        /* Remember current stack depth; only pause at same or shallower depth */
        int depth = 0;
        lua_Debug ar;
        for (int i = 0; lua_getstack(g_co, i, &ar); i++) depth++;
        g_step_target = depth;
        g_dbg_state = DBG_STEPPING;
        g_step_mode = STEP_OVER;
    }
}

static void dbg_step_into() {
    if (g_dbg_state == DBG_PAUSED) {
        g_dbg_state = DBG_STEPPING;
        g_step_mode = STEP_INTO;
    }
}

static void dbg_step_out() {
    if (g_dbg_state == DBG_PAUSED) {
        int depth = 0;
        lua_Debug ar;
        for (int i = 0; lua_getstack(g_co, i, &ar); i++) depth++;
        g_step_target = depth - 1;
        if (g_step_target < 0) g_step_target = 0;
        g_dbg_state = DBG_STEPPING;
        g_step_mode = STEP_OUT;
    }
}

static void dbg_pause() {
    if (g_dbg_state == DBG_RUNNING) {
        g_dbg_state = DBG_STEPPING;
        g_step_mode = STEP_INTO;  /* will break on next line */
    }
}

static void dbg_stop() {
    g_dbg_state = DBG_IDLE;
    g_step_mode = STEP_NONE;
    /* Reset the coroutine by creating a new one */
    if (g_co) {
        lua_sethook(g_co, nullptr, 0, 0);
    }
    if (g_mainL) {
        g_co = lua_newthread(g_mainL);
        /* Keep reference in registry so GC doesn't collect it */
        lua_setfield(g_mainL, LUA_REGISTRYINDEX, "_dbg_co");
    }
    /* Clear all display caches — stale data from old run must not linger */
    g_locals_cache.clear();
    g_upvals_cache.clear();
    g_callstack_cache.clear();
    g_bytecode_proto = nullptr;
    g_bytecode_curpc = -1;
    g_bytecode_ci    = nullptr;
    g_registers_cache.clear();
    g_cur_source.clear();
    g_cur_line = -1;
    g_selected_frame = 0;
    console_add("--- Execution stopped ---", ImVec4(1.0f, 0.5f, 0.3f, 1.0f));
}

/* ============================================================
 *  Evaluate expression in the paused coroutine's context
 * ============================================================ */
static void dbg_eval(const char *expr) {
    if (!g_co || g_dbg_state != DBG_PAUSED) {
        console_add("Error: not paused", ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        return;
    }

    /* Get current function's environment */
    lua_Debug ar;
    if (!lua_getstack(g_co, 0, &ar)) return;

    /* Try to evaluate in the context: create a closure that runs the expression */
    std::string code = std::string("return ") + expr;
    int status = luaL_loadstring(g_co, code.c_str());
    if (status != LUA_OK) {
        /* Try without 'return' */
        lua_pop(g_co, 1);
        status = luaL_loadstring(g_co, expr);
    }
    if (status != LUA_OK) {
        console_add(lua_tostring(g_co, -1), ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        lua_pop(g_co, 1);
        return;
    }

    /* Set upvalues so the closure sees local variables */
    lua_getinfo(g_co, "f", &ar);
    for (int i = 1; ; i++) {
        const char *name = lua_getlocal(g_co, &ar, i);
        if (!name) break;
        /* Keep on stack for debug */
        lua_pop(g_co, 1);  /* we only needed to know it exists */
    }
    /* Re-get info since we may have called other functions */
    lua_getinfo(g_co, "f", &ar);

    /* Call the expression */
    status = lua_pcall(g_co, 0, 1, 0);
    if (status != LUA_OK) {
        console_add(lua_tostring(g_co, -1), ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        lua_pop(g_co, 1);
        return;
    }

    /* Result is on stack */
    const char *result = luaL_tolstring(g_co, -1, nullptr);
    console_add(result, ImVec4(0.7f, 1.0f, 1.0f, 1.0f));
    lua_pop(g_co, 2);  /* result + tostring copy */
}

/* ============================================================
 *  Helper: get a short filename from a full path
 * ============================================================ */
static const char *short_name(const char *path) {
    const char *s = strrchr(path, '/');
#ifdef _WIN32
    if (!s) s = strrchr(path, '\\');
#endif
    return s ? s + 1 : path;
}

/* ============================================================
 *  UI: Source Code View
 * ============================================================ */
static void draw_source_view() {
    ImGui::Begin("Source Code");

    if (g_cur_source.empty()) {
        ImGui::TextDisabled("No source loaded. Load a Lua/Java script first.");
        ImGui::End();
        return;
    }

    /* Ensure source is cached */
    load_source_file(g_cur_source.c_str());
    auto it = g_sources.find(g_cur_source);
    if (it == g_sources.end()) {
        ImGui::Text("Source file not found: %s", g_cur_source.c_str());
        ImGui::End();
        return;
    }

    ImGui::Text("File: %s", short_name(g_cur_source.c_str()));

    bool paused = (g_dbg_state == DBG_PAUSED);

    /* Scroll to current line when paused */
    if (paused && g_cur_line > 0) {
        float line_h = ImGui::GetTextLineHeight();
        ImGui::SetScrollY((g_cur_line - 5) * line_h);
    }

    ImGui::BeginChild("SourceLines", ImVec2(0, 0), false,
                       ImGuiWindowFlags_AlwaysVerticalScrollbar);

    const auto &lines = it->second;
    ImGuiListClipper clipper;
    clipper.Begin((int)lines.size());

    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
            int lineno = i + 1;
            bool is_cur = paused && (lineno == g_cur_line);
            bool is_bp  = is_breakpoint(g_cur_source.c_str(), lineno);

            /* Line number */
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "%4d ", lineno);
            ImGui::SameLine(38);

            /* Clickable line: label includes line text so full width is clickable */
            ImGui::PushID(lineno);

            char line_label[320];
            int n = snprintf(line_label, sizeof(line_label),
                             "%s", lines[i].c_str());
            /* Pad short lines to ensure a reasonable click target width */
            while (n < 20 && n < (int)sizeof(line_label) - 1)
                line_label[n++] = ' ';
            line_label[n] = '\0';

            if (ImGui::Selectable(line_label, is_cur,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                /* Click: set cursor; Double-click: toggle breakpoint */
                if (ImGui::IsMouseDoubleClicked(0)) {
                    toggle_breakpoint(g_cur_source.c_str(), lineno);
                }
            }

            /* Red dot for breakpoint (in gutter, vertically centered on line) */
            if (is_bp) {
                ImVec2 pos_min = ImGui::GetItemRectMin();
                ImVec2 pos_max = ImGui::GetItemRectMax();
                float y_center = (pos_min.y + pos_max.y) * 0.5f;
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(pos_min.x + 5, y_center), 4.0f,
                    IM_COL32(220, 50, 50, 255));
            }

            /* Highlight current line */
            if (is_cur) {
                ImVec2 rect_min = ImGui::GetItemRectMin();
                ImVec2 rect_max = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    rect_min, rect_max,
                    IM_COL32(60, 100, 180, 120));
            }
            ImGui::PopID();
        }
    }

    ImGui::EndChild();
    ImGui::End();
}

/* ============================================================
 *  UI: Bytecode / Disassembly View
 * ============================================================ */
static void draw_bytecode_view() {
    ImGui::Begin("Bytecode");

    if (!g_co || g_dbg_state != DBG_PAUSED) {
        ImGui::TextDisabled("Pause execution to see bytecode.");
        ImGui::End();
        return;
    }

    if (!g_bytecode_proto) {
        ImGui::TextDisabled("No Lua function at current frame.");
        ImGui::End();
        return;
    }

    const Proto *p = (const Proto *)g_bytecode_proto;
    int curpc = g_bytecode_curpc;

    ImGui::Text("Proto: %p  params=%d  stack=%d  code=%d  consts=%d  protos=%d",
                (void*)p, p->numparams, p->maxstacksize,
                p->sizecode, p->sizek, p->sizep);
    ImGui::Separator();

    if (p->sizecode > 0) {
        ImGui::BeginChild("CodeScroll", ImVec2(0, 0), false,
                           ImGuiWindowFlags_AlwaysVerticalScrollbar);

        if (curpc >= 0) {
            float line_h = ImGui::GetTextLineHeight();
            ImGui::SetScrollY((curpc - 3) * line_h);
        }

        for (int i = 0; i < p->sizecode; i++) {
            Instruction inst = p->code[i];
            OpCode op = GET_OPCODE(inst);

            /* Arrow for current PC */
            if (i == curpc) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), ">");
                ImGui::SameLine();
            } else {
                ImGui::TextUnformatted(" ");
                ImGui::SameLine();
            }

            char buf[256];
            int off = snprintf(buf, sizeof(buf), "%4d  %-9s A=%-2d",
                               i, luaP_opnames[op], GETARG_A(inst));

            switch (getOpMode(op)) {
            case iABC: {
                off += snprintf(buf + off, sizeof(buf) - off,
                                " B=%-2d C=%-4d",
                                GETARG_B(inst), GETARG_C(inst));
                if (GETARG_C(inst) & BITRK) {
                    int k = GETARG_C(inst) & ~BITRK;
                    if (k < p->sizek) {
                        TValue *kv = &p->k[k];
                        if (ttisstring(kv))
                            off += snprintf(buf + off, sizeof(buf) - off,
                                            " ; \"%s\"", getstr(tsvalue(kv)));
                        else if (ttisinteger(kv))
                            off += snprintf(buf + off, sizeof(buf) - off,
                                            " ; %lld", (long long)ivalue(kv));
                        else if (ttisfloat(kv))
                            off += snprintf(buf + off, sizeof(buf) - off,
                                            " ; %g", fltvalue(kv));
                    }
                }
                if (op == OP_CLOSURE) {
                    int pi = GETARG_Bx(inst);
                    off += snprintf(buf + off, sizeof(buf) - off,
                                    "  -- proto[%d]", pi);
                }
                if (op == OP_CALL) {
                    off += snprintf(buf + off, sizeof(buf) - off,
                                    "  -- call(args=%d, rets=%d)",
                                    GETARG_B(inst), GETARG_C(inst));
                }
                break;
            }
            case iABx:
                off += snprintf(buf + off, sizeof(buf) - off,
                                " Bx=%-4d", GETARG_Bx(inst));
                if (op == OP_CLOSURE)
                    off += snprintf(buf + off, sizeof(buf) - off,
                                    " ; proto[%d]", GETARG_Bx(inst));
                break;
            case iAsBx:
                off += snprintf(buf + off, sizeof(buf) - off,
                                " sBx=%-4d", GETARG_sBx(inst));
                break;
            case iAx:
                off += snprintf(buf + off, sizeof(buf) - off,
                                " Ax=%-4d", GETARG_Ax(inst));
                break;
            }
            (void)off;

            if (p->lineinfo && i < p->sizelineinfo) {
                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
                         "  [line %d]", p->lineinfo[i]);
            }

            if (i == curpc) {
                ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "%s", buf);
            } else {
                ImGui::TextUnformatted(buf);
            }
        }

        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("No bytecode in this function.");
    }

    ImGui::End();
}

/* ============================================================
 *  UI: Call Stack
 * ============================================================ */
static void draw_call_stack() {
    ImGui::Begin("Call Stack");

    if (!g_co || g_dbg_state != DBG_PAUSED) {
        ImGui::TextDisabled("(not paused)");
        ImGui::End();
        return;
    }

    /* Clamp selected frame to valid range */
    if (g_selected_frame >= (int)g_callstack_cache.size())
        g_selected_frame = 0;

    for (int level = 0; level < (int)g_callstack_cache.size(); level++) {
        auto &e = g_callstack_cache[level];
        int abs_level = (int)g_callstack_cache.size() - 1 - level;  /* topmost first */

        char label[256];
        snprintf(label, sizeof(label), "%s (%s:%d)##cs%d",
                 e.name.c_str(), short_name(e.source.c_str()),
                 e.line, level);

        bool is_sel = (abs_level == g_selected_frame);
        if (ImGui::Selectable(label, is_sel)) {
            g_selected_frame = abs_level;
            /* Navigate source to selected frame */
            if (!e.source.empty() && e.source != "?") {
                std::string src = e.source;
                if (src[0] == '@') src = src.substr(1);
                g_cur_source = src;
                g_cur_line   = e.line;
                load_source_file(src.c_str());
            }
        }

        /* Frame number badge */
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30);
        ImGui::TextDisabled("[%d]", abs_level);
    }

    ImGui::End();
}

/* ============================================================
 *  UI: Local Variables  (reads from cache — never touches g_co)
 * ============================================================ */

/* ---- recursive table-row helpers (forward declare) ---- */
static void render_locals_child(TableChild &ch, int depth);

static void render_local_entry(LocalEntry &e) {
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(e.name.c_str());

    ImGui::TableSetColumnIndex(1);
    if (e.table_ptr) {
        ImGui::PushID((int)(intptr_t)&e);
        if (ImGui::SmallButton(e.table_expanded ? "[-]" : "[+]")) {
            e.table_expanded = !e.table_expanded;
            if (e.table_expanded && e.table_children.empty())
                expand_entry((void*)e.table_ptr, e.table_children, MAX_TABLE_DEPTH);
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(e.value.c_str());

    if (e.table_expanded) {
        for (auto &ch : e.table_children)
            render_locals_child(ch, 1);
    }
}

static void render_locals_child(TableChild &ch, int depth) {
    ImGui::TableNextRow();

    /* indentation in Name column — add one level on top of current indent */
    ImGui::TableSetColumnIndex(0);
    ImGui::Indent(15.0f);
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", ch.key.c_str());
    ImGui::Unindent(15.0f);

    ImGui::TableSetColumnIndex(1);
    if (ch.table_ptr) {
        ImGui::PushID((int)(intptr_t)&ch);
        if (ImGui::SmallButton(ch.expanded ? "[-]" : "[+]")) {
            ch.expanded = !ch.expanded;
            if (ch.expanded && ch.children.empty())
                expand_table_child(ch, MAX_TABLE_DEPTH - depth);
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                       "%s  %s", ch.value_type.c_str(), ch.value.c_str());

    if (ch.expanded) {
        for (auto &nested : ch.children)
            render_locals_child(nested, depth + 1);
    }
}

static void draw_locals() {
    ImGui::Begin("Locals");

    if (!g_co || g_dbg_state != DBG_PAUSED) {
        ImGui::TextDisabled("(not paused)");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable("locals_tbl", 2,
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (auto &e : g_locals_cache)
            render_local_entry(e);

        /* Upvalues */
        if (!g_upvals_cache.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Upvalues");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted("");

            for (auto &e : g_upvals_cache)
                render_local_entry(e);
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

/* ============================================================
 *  UI: Registers  (reads from cache — never touches g_co)
 * ============================================================ */

static void render_regs_child(TableChild &ch, int depth);

static void render_reg_entry(RegisterEntry &e, int depth) {
    ImGui::TableNextRow();

    char regname[16];
    snprintf(regname, sizeof(regname), "R%d", e.index);
    ImVec4 color = ImVec4(1, 1, 1, 1);
    if (e.is_top) color = ImVec4(1.0f, 0.75f, 0.25f, 1.0f);

    /* Reg column */
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(color, "%s", regname);

    /* Type column */
    ImGui::TableSetColumnIndex(1);
    ImGui::TextColored(color, "%s", e.type.c_str());

    /* Value column — with [+]/[-] button if table */
    ImGui::TableSetColumnIndex(2);
    if (e.table_ptr) {
        ImGui::PushID((int)(intptr_t)&e);
        if (ImGui::SmallButton(e.table_expanded ? "[-]" : "[+]")) {
            e.table_expanded = !e.table_expanded;
            if (e.table_expanded && e.table_children.empty())
                expand_entry((void*)e.table_ptr, e.table_children, MAX_TABLE_DEPTH);
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::TextColored(color, "%s", e.value.c_str());

    /* Note column */
    ImGui::TableSetColumnIndex(3);
    if (e.is_top && e.is_params)
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.3f, 1.0f), "top|param");
    else if (e.is_top)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "top");
    else if (e.is_params)
        ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "param");

    if (e.table_expanded) {
        for (auto &ch : e.table_children)
            render_regs_child(ch, depth + 1);
    }
}

static void render_regs_child(TableChild &ch, int depth) {
    ImGui::TableNextRow();
    ImVec4 child_color = ImVec4(0.6f, 0.9f, 0.6f, 1.0f);

    /* Key (indented, in Reg column) — add one level on top of current indent */
    ImGui::TableSetColumnIndex(0);
    ImGui::Indent(12.0f);
    ImGui::TextColored(child_color, "%s", ch.key.c_str());
    ImGui::Unindent(12.0f);

    /* Type */
    ImGui::TableSetColumnIndex(1);
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", ch.value_type.c_str());

    /* Value */
    ImGui::TableSetColumnIndex(2);
    if (ch.table_ptr) {
        ImGui::PushID((int)(intptr_t)&ch);
        if (ImGui::SmallButton(ch.expanded ? "[-]" : "[+]")) {
            ch.expanded = !ch.expanded;
            if (ch.expanded && ch.children.empty())
                expand_table_child(ch, MAX_TABLE_DEPTH - depth);
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", ch.value.c_str());

    /* Note column (empty for children) */
    ImGui::TableSetColumnIndex(3);

    if (ch.expanded) {
        for (auto &nested : ch.children)
            render_regs_child(nested, depth + 1);
    }
}

static void draw_registers() {
    ImGui::Begin("Registers");

    if (!g_co || g_dbg_state != DBG_PAUSED) {
        ImGui::TextDisabled("(not paused)");
        ImGui::End();
        return;
    }

    if (g_registers_cache.empty()) {
        ImGui::TextDisabled("No register data available.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable("regs_tbl", 4,
            ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(0, -1))) {
        ImGui::TableSetupColumn("Reg",   ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("Type",  ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Note",  ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableHeadersRow();

        for (auto &e : g_registers_cache)
            render_reg_entry(e, 0);

        ImGui::EndTable();
    }

    ImGui::End();
}

/* ============================================================
 *  UI: Console
 * ============================================================ */
static void draw_console() {
    ImGui::Begin("Console");

    /* Output area */
    ImGui::BeginChild("ConsoleOutput", ImVec2(0, -30), false,
                       ImGuiWindowFlags_AlwaysVerticalScrollbar);

    /* Auto-scroll */
    static bool auto_scroll = true;
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5.0f)
        auto_scroll = true;
    else if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 20.0f)
        auto_scroll = false;

    for (auto &cl : g_console) {
        ImGui::TextColored(cl.color, "%s", cl.text.c_str());
    }

    /* Right-click context menu: Clear */
    if (ImGui::BeginPopupContextWindow("ConsoleCtx", ImGuiPopupFlags_MouseButtonRight)) {
        if (ImGui::MenuItem("Clear")) {
            g_console.clear();
        }
        ImGui::EndPopup();
    }

    if (auto_scroll)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();

    /* Input area with Clear button */
    float clear_btn_w = ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - clear_btn_w - 10);
    bool reclaim_focus = false;
    if (ImGui::InputText("##console_input", g_console_input,
                          sizeof(g_console_input),
                          ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (strlen(g_console_input) > 0) {
            /* Echo to console */
            console_add(("> " + std::string(g_console_input)).c_str(),
                        ImVec4(0.5f, 0.9f, 0.5f, 1.0f));
            /* Evaluate in debuggee context */
            dbg_eval(g_console_input);
            g_console_input[0] = '\0';
            reclaim_focus = true;
        }
    }
    /* Reclaim focus after Enter — keep cursor in input box */
    if (reclaim_focus)
        ImGui::SetKeyboardFocusHere(-1);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(clear_btn_w, 0))) {
        g_console.clear();
    }

    /* Click anywhere in console content area → focus input */
    static bool g_console_input_focused_once = false;
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsAnyItemActive() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::SetKeyboardFocusHere(-1);
        g_console_input_focused_once = true;
    }
    /* First time the console gets focus at all, focus input */
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !g_console_input_focused_once &&
        !ImGui::IsAnyItemActive()) {
        ImGui::SetKeyboardFocusHere(-1);
        g_console_input_focused_once = true;
    }

    ImGui::End();
}

/* ============================================================
 *  UI: Main Menu Bar + Compact Toolbar
 * ============================================================ */
/* ============================================================
 *  Build the default docking layout (shared by first-run init
 *  and Reset-to-Default).  Caller must ensure the dockspace
 *  node does NOT exist yet (or has been removed).
 * ============================================================ */
static void build_default_dock_layout(ImGuiID dockspace_id) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

    /* Split: main area (left 65%), right sidebar (35%) */
    ImGuiID dock_main, dock_right;
    ImGui::DockBuilderSplitNode(
        dockspace_id, ImGuiDir_Left, 0.65f, &dock_main, &dock_right);

    /* Split right sidebar: 3 equal-ish zones */
    ImGuiID dock_right_mid, dock_right_bot;
    ImGui::DockBuilderSplitNode(
        dock_right, ImGuiDir_Up, 0.33f, &dock_right_mid, &dock_right_bot);
    ImGuiID dock_right_top, dock_right_mid2;
    ImGui::DockBuilderSplitNode(
        dock_right_mid, ImGuiDir_Up, 0.50f, &dock_right_top, &dock_right_mid2);

    /* Split main area: top 75% (source), bottom 25% (console) */
    ImGuiID dock_center, dock_console;
    ImGui::DockBuilderSplitNode(
        dock_main, ImGuiDir_Down, 0.25f, &dock_console, &dock_center);

    /* Dock windows into their respective zones */
    ImGui::DockBuilderDockWindow("Source Code", dock_center);
    ImGui::DockBuilderDockWindow("Console", dock_console);
    ImGui::DockBuilderDockWindow("Call Stack", dock_right_top);
    ImGui::DockBuilderDockWindow("Locals", dock_right_top);
    ImGui::DockBuilderDockWindow("Registers", dock_right_mid2);
    ImGui::DockBuilderDockWindow("Bytecode", dock_right_mid2);
    ImGui::DockBuilderDockWindow("Breakpoints", dock_right_bot);

    ImGui::DockBuilderFinish(dockspace_id);
}

/* ============================================================
 *  Reset layout to default docking arrangement
 * ============================================================ */
static void reset_to_default_layout() {
    g_pending_layout_reset = true;
    g_show_source      = true;
    g_show_callstack   = true;
    g_show_locals      = true;
    g_show_registers   = true;
    g_show_bytecode    = true;
    g_show_console     = true;
    g_show_breakpoints = true;
}

/* ============================================================
 *  Save / Load layout to/from disk
 * ============================================================ */
static std::string config_dir();  /* forward declaration */

static std::string layout_path() {
    return config_dir() + "/layout.ini";
}

static void save_layout() {
    std::string dir = config_dir();
    mkdir(dir.c_str(), 0755);
    ImGui::SaveIniSettingsToDisk(layout_path().c_str());
}

static void load_layout() {
    struct stat st;
    if (stat(layout_path().c_str(), &st) != 0) return;  /* file missing */
    ImGui::LoadIniSettingsFromDisk(layout_path().c_str());
}

static void draw_main_menu() {
    bool can_step   = (g_dbg_state == DBG_PAUSED);
    bool can_cont   = (g_dbg_state == DBG_PAUSED);
    bool has_script = (g_dbg_state != DBG_IDLE);

    /* ---- Modal: File > Open ---- */
    if (g_open_file_modal) {
        g_open_file_modal = false;  /* Consume the flag */
        /* Try native dialog first */
        std::string chosen = native_file_dialog();
        if (!chosen.empty()) {
            /* Native dialog returned a file — load it directly */
            snprintf(g_filepath_buf, sizeof(g_filepath_buf),
                     "%s", chosen.c_str());
            dbg_load_script(chosen.c_str());
        } else if (g_zenity_available) {
            /* User cancelled native dialog — do nothing */
        } else {
            /* Zenity not available — fall back to text input modal */
            ImGui::OpenPopup("Open File (manual)");
        }
    }
    /* Fallback manual file path input (when zenity unavailable) */
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Open File (manual)", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter file path (.lua or .java):");
        ImGui::SetNextItemWidth(400);
        ImGui::InputText("##open_path", g_open_file_buf,
                         sizeof(g_open_file_buf));
        ImGui::Separator();
        if (ImGui::Button("Load", ImVec2(120, 0))) {
            if (strlen(g_open_file_buf) > 0) {
                snprintf(g_filepath_buf, sizeof(g_filepath_buf),
                         "%s", g_open_file_buf);
                dbg_load_script(g_open_file_buf);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* ---- Main Menu Bar ---- */
    if (ImGui::BeginMainMenuBar()) {

        /* File */
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                g_open_file_modal = true;
            }
            if (ImGui::MenuItem("Reload", "Ctrl+R", false,
                                strlen(g_filepath_buf) > 0)) {
                dbg_stop();  /* dbg_stop already creates a new thread */
                dbg_load_script(g_filepath_buf);
            }
            /* Recent Files */
            ImGui::Separator();
            if (!g_recent_files.empty()) {
                ImGui::TextDisabled("Recent Files");
                int idx = 1;
                for (auto it = g_recent_files.begin();
                     it != g_recent_files.end(); ++it, ++idx) {
                    char label[32];
                    snprintf(label, sizeof(label), "%d: %s",
                             idx, short_name(it->c_str()));
                    if (ImGui::MenuItem(label, nullptr, false, true)) {
                        snprintf(g_filepath_buf, sizeof(g_filepath_buf),
                                 "%s", it->c_str());
                        dbg_stop();  /* dbg_stop already creates a new thread */
                        dbg_load_script(it->c_str());
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", it->c_str());
                }
                if (ImGui::MenuItem("Clear Recent")) {
                    g_recent_files.clear();
                }
                ImGui::Separator();
            }
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                if (g_window) glfwSetWindowShouldClose(g_window, true);
            }
            ImGui::EndMenu();
        }

        /* Debug */
        if (ImGui::BeginMenu("Debug")) {
            if (ImGui::MenuItem("Continue", "F5", false, can_cont))
                dbg_continue();
            ImGui::Separator();
            if (ImGui::MenuItem("Step Over", "F10", false, can_step))
                dbg_step_over();
            if (ImGui::MenuItem("Step Into", "F11", false, can_step))
                dbg_step_into();
            if (ImGui::MenuItem("Step Out", "Shift+F11", false, can_step))
                dbg_step_out();
            ImGui::Separator();
            if (ImGui::MenuItem("Pause", nullptr, false, has_script))
                dbg_pause();
            if (ImGui::MenuItem("Stop", "Ctrl+F2", false, has_script))
                dbg_stop();
            ImGui::Separator();
            ImGui::MenuItem("Break on Main", nullptr, &g_break_on_main);
            ImGui::EndMenu();
        }

        /* View */
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Source Code",   nullptr, &g_show_source);
            ImGui::MenuItem("Bytecode",      nullptr, &g_show_bytecode);
            ImGui::MenuItem("Call Stack",    nullptr, &g_show_callstack);
            ImGui::MenuItem("Locals",        nullptr, &g_show_locals);
            ImGui::MenuItem("Registers",     nullptr, &g_show_registers);
            ImGui::MenuItem("Console",       nullptr, &g_show_console);
            ImGui::MenuItem("Breakpoints",   nullptr, &g_show_breakpoints);
            ImGui::Separator();
            ImGui::MenuItem("Debug Toolbar",  nullptr, &g_show_controls);
            if (ImGui::MenuItem("Save Layout"))    save_layout();
            if (ImGui::MenuItem("Load Layout"))    load_layout();
            ImGui::Separator();
            if (ImGui::MenuItem("Reset to Default Layout")) {
                reset_to_default_layout();
            }
            ImGui::EndMenu();
        }

        /* Help */
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                ImGui::OpenPopup("About##Popup");
            }
            ImGui::EndMenu();
        }

        /* ---- Status indicator on the right side ---- */
        const char *status_text;
        ImVec4 status_color;
        switch (g_dbg_state) {
        case DBG_IDLE:    status_text="IDLE";    status_color=ImVec4(0.5f,0.5f,0.5f,1.f); break;
        case DBG_RUNNING: status_text="RUNNING"; status_color=ImVec4(0.3f,1.f,0.3f,1.f); break;
        case DBG_STEPPING:status_text="STEPPING";status_color=ImVec4(1.f,1.f,0.3f,1.f); break;
        case DBG_PAUSED:  status_text="PAUSED";  status_color=ImVec4(1.f,0.5f,0.2f,1.f); break;
        default:          status_text="???";     status_color=ImVec4(1,1,1,1); break;
        }

        float menu_width = ImGui::GetWindowWidth();
        float status_w = ImGui::CalcTextSize(status_text).x + 20;
        char exit_buf[32] = "";
        if (g_dbg_state == DBG_IDLE && g_has_exit_code) {
            snprintf(exit_buf, sizeof(exit_buf), "exit %d", g_last_exit_code);
            status_w += ImGui::CalcTextSize(exit_buf).x + 10;
        }
        if (g_dbg_state == DBG_PAUSED && g_cur_line > 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s:%d",
                     short_name(g_cur_source.c_str()), g_cur_line);
            status_w += ImGui::CalcTextSize(buf).x + 10;
        }
        ImGui::SetCursorPosX(menu_width - status_w - 10);
        ImGui::TextColored(status_color, "[%s]", status_text);
        if (g_dbg_state == DBG_IDLE && g_has_exit_code) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                               "%s", exit_buf);
        }
        if (g_dbg_state == DBG_PAUSED && g_cur_line > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f),
                               "%s:%d",
                               short_name(g_cur_source.c_str()), g_cur_line);
        }

        ImGui::EndMainMenuBar();
    }

    /* ---- "About" popup ---- */
    ImVec2 about_center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(about_center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("About##Popup", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Lua Debugger");
        ImGui::Separator();
        ImGui::Text("Built with Dear ImGui + Lua 5.3.6");
        ImGui::Text("Supports .lua and .java (Java-to-Lua) scripts");
        ImGui::Spacing();
        ImGui::Text("Shortcuts:");
        ImGui::BulletText("F5          - Continue");
        ImGui::BulletText("F10         - Step Over");
        ImGui::BulletText("F11         - Step Into");
        ImGui::BulletText("Shift+F11   - Step Out");
        ImGui::BulletText("Ctrl+O      - Open File");
        ImGui::BulletText("Ctrl+R      - Reload");
        ImGui::BulletText("Ctrl+F2     - Stop");
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

}

/* ============================================================
 *  UI: Debug Toolbar — horizontal strip below menu bar
 *      Modeled after VS Code / IntelliJ IDEA debug toolbar
 * ============================================================ */
static const float TOOLBAR_HEIGHT = 34.0f;

static void draw_controls_toolbar() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    /* Toolbar window: anchored below menu bar, full width, no decorations */
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, TOOLBAR_HEIGHT));

    ImGuiWindowFlags tb_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.14f, 0.14f, 0.15f, 1.0f));

    ImGui::Begin("##DebugToolbar", nullptr, tb_flags);

    bool can_cont   = (g_dbg_state == DBG_PAUSED);
    bool can_step   = (g_dbg_state == DBG_PAUSED);
    bool has_script = (g_dbg_state != DBG_IDLE);

    /* ---- File path input ---- */
    ImGui::PushItemWidth(200);
    ImGui::InputTextWithHint("##filepath_tb", "script.lua / .java",
                              g_filepath_buf, sizeof(g_filepath_buf));
    ImGui::PopItemWidth();
    ImGui::SameLine();

    if (ImGui::Button("Browse...")) {
        std::string chosen = native_file_dialog();
        if (!chosen.empty()) {
            snprintf(g_filepath_buf, sizeof(g_filepath_buf), "%s", chosen.c_str());
            dbg_load_script(chosen.c_str());
        } else if (!g_zenity_available) {
            g_open_file_modal = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        if (strlen(g_filepath_buf) > 0) dbg_load_script(g_filepath_buf);
    }

    /* ---- Separator ---- */
    ImGui::SameLine();
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(p.x + 4, p.y + 1),
            ImVec2(p.x + 4, p.y + TOOLBAR_HEIGHT - 8),
            IM_COL32(80, 80, 80, 255), 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 12, p.y));
    }

    /* ---- Execution buttons (IDE order: Continue → Over → Into → Out) ---- */
    ImVec2 btn_size(28, 24);

    if (!can_cont) ImGui::BeginDisabled();
    if (ImGui::Button("▶", btn_size)) dbg_continue();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Continue (F5)");
    if (!can_cont) ImGui::EndDisabled();
    ImGui::SameLine();

    if (!can_step) ImGui::BeginDisabled();
    if (ImGui::Button("→", btn_size)) dbg_step_over();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Step Over (F10)");
    ImGui::SameLine();
    if (ImGui::Button("↓", btn_size)) dbg_step_into();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Step Into (F11)");
    ImGui::SameLine();
    if (ImGui::Button("↑", btn_size)) dbg_step_out();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Step Out (Shift+F11)");
    if (!can_step) ImGui::EndDisabled();

    ImGui::SameLine();
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(p.x + 2, p.y + 1),
            ImVec2(p.x + 2, p.y + TOOLBAR_HEIGHT - 8),
            IM_COL32(80, 80, 80, 255), 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 8, p.y));
    }

    if (!has_script) ImGui::BeginDisabled();
    if (ImGui::Button("⏸", btn_size)) dbg_pause();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Pause");
    ImGui::SameLine();
    if (ImGui::Button("■", btn_size)) dbg_stop();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Stop (Ctrl+F2)");
    if (!has_script) ImGui::EndDisabled();

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

/* ============================================================
 *  UI: Breakpoints List
 * ============================================================ */
static void draw_breakpoints() {
    ImGui::Begin("Breakpoints");

    if (g_breakpoints.empty()) {
        ImGui::TextDisabled("No breakpoints set.");
        ImGui::End();
        return;
    }

    /* ---- Action buttons ---- */
    if (ImGui::Button("Select All")) {
        for (auto &fe : g_breakpoints)
            for (auto &bp : fe.second)
                g_bp_select[fe.first][bp.first] = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Deselect All")) {
        g_bp_select.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Enable Sel")) {
        for (auto &sf : g_bp_select) {
            for (auto &sl : sf.second) {
                if (sl.second) bp_set_enabled(sf.first, sl.first, true);
            }
        }
        g_bp_select.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Disable Sel")) {
        for (auto &sf : g_bp_select) {
            for (auto &sl : sf.second) {
                if (sl.second) bp_set_enabled(sf.first, sl.first, false);
            }
        }
        g_bp_select.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Sel")) {
        for (auto &sf : g_bp_select) {
            for (auto &sl : sf.second) {
                if (sl.second) bp_delete(sf.first, sl.first);
            }
        }
        g_bp_select.clear();
    }
    ImGui::Separator();

    /* ---- Breakpoint list ---- */
    ImGui::BeginChild("BpList", ImVec2(0, 0), false,
                       ImGuiWindowFlags_AlwaysVerticalScrollbar);

    /* Collect flat sorted list for display */
    struct BpDisplay {
        std::string file;
        int         line;
        bool        enabled;
    };
    std::vector<BpDisplay> bps;
    for (auto &fe : g_breakpoints) {
        for (auto &bp : fe.second) {
            bps.push_back({fe.first, bp.first, bp.second});
        }
    }
    std::sort(bps.begin(), bps.end(), [](const BpDisplay &a, const BpDisplay &b) {
        if (a.file != b.file) return a.file < b.file;
        return a.line < b.line;
    });

    for (auto &bp : bps) {
        char id[320];

        /* Selection checkbox */
        snprintf(id, sizeof(id), "##sel_%s_%d", bp.file.c_str(), bp.line);
        bool sel = g_bp_select[bp.file][bp.line];
        if (ImGui::Checkbox(id, &sel)) {
            g_bp_select[bp.file][bp.line] = sel;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select for batch ops");
        }
        ImGui::SameLine();

        /* Enabled checkbox */
        snprintf(id, sizeof(id), "##en_%s_%d", bp.file.c_str(), bp.line);
        bool en = bp.enabled;
        if (ImGui::Checkbox(id, &en)) {
            bp_set_enabled(bp.file, bp.line, en);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enable/disable");
        }
        ImGui::SameLine();

        /* File:line info + delete */
        if (bp.enabled)
            ImGui::Text("%s:%d", short_name(bp.file.c_str()), bp.line);
        else
            ImGui::TextDisabled("%s:%d (disabled)",
                                short_name(bp.file.c_str()), bp.line);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", bp.file.c_str());

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.4f, 0.1f, 0.1f, 1.0f));
        snprintf(id, sizeof(id), "X##del_%s_%d", bp.file.c_str(), bp.line);
        if (ImGui::SmallButton(id))
            bp_delete(bp.file, bp.line);
        ImGui::PopStyleColor();
    }

    ImGui::EndChild();
    ImGui::End();
}

/* ============================================================
 *  Config persistence: ~/.luad/config
 *
 *  Format (line-based):
 *    recent /path/to/file
 *    break_on_main 1|0
 *    bp /path/to/file line
 * ============================================================ */
static std::string config_dir() {
    const char *home = getenv("HOME");
    if (!home) home = getenv("USERPROFILE");
    if (!home) return ".luad";
    return std::string(home) + "/.luad";
}

static std::string config_path() {
    return config_dir() + "/config";
}

static void config_save() {
    std::string dir = config_dir();
    mkdir(dir.c_str(), 0755);

    FILE *f = fopen(config_path().c_str(), "w");
    if (!f) return;

    for (auto &rf : g_recent_files)
        fprintf(f, "recent %s\n", rf.c_str());
    fprintf(f, "break_on_main %d\n", g_break_on_main ? 1 : 0);
    fprintf(f, "show_toolbar %d\n", g_show_controls ? 1 : 0);
    for (auto &fe : g_breakpoints) {
        for (auto &bp : fe.second)
            fprintf(f, "bp %s %d %d\n", fe.first.c_str(), bp.first,
                    bp.second ? 1 : 0);
    }
    fclose(f);
}

static void config_load() {
    FILE *f = fopen(config_path().c_str(), "r");
    if (!f) return;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        char type[64] = {0};
        char arg1[2048] = {0};
        int arg2 = 0;
        if (sscanf(line, "%63s", type) != 1) continue;

        if (strcmp(type, "recent") == 0) {
            const char *rest = line + strlen("recent");
            while (*rest == ' ' || *rest == '\t') rest++;
            if (*rest) {
                g_recent_files.push_back(rest);
                if ((int)g_recent_files.size() > RECENT_MAX)
                    g_recent_files.pop_back();
            }
        } else if (strcmp(type, "break_on_main") == 0) {
            sscanf(line, "%*s %d", &arg2);
            g_break_on_main = (arg2 != 0);
        } else if (strcmp(type, "show_toolbar") == 0) {
            sscanf(line, "%*s %d", &arg2);
            g_show_controls = (arg2 != 0);
        } else if (strcmp(type, "bp") == 0) {
            int arg3 = 1;
            int n = sscanf(line, "%*s %2047s %d %d", arg1, &arg2, &arg3);
            if (n >= 2 && arg2 > 0)
                g_breakpoints[arg1][arg2] = (arg3 != 0);
        }
    }
    fclose(f);
}

/* ============================================================
 *  Main
 * ============================================================ */
static void glfw_error_callback(int error, const char *description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

int main(int argc, char *argv[]) {
    /* ---- Parse arguments for an optional file to load ---- */
    const char *load_file = nullptr;
    if (argc > 1) load_file = argv[1];

    /* ---- 1. Init Lua ---- */
    g_mainL = luaL_newstate();
    if (!g_mainL) {
        fprintf(stderr, "Failed to create Lua state\n");
        return 1;
    }

    luaL_openlibs(g_mainL);
    jlex_init(g_mainL);
    java_openlib(g_mainL);
    lua_gc(g_mainL, LUA_GCSTOP, 0);  /* stop GC for debugging stability */

    /* Build argv as a Lua table with 0-based indexing (Java convention).
     * args[0] = script file name, args[1..] = extra command-line args */
    {
        int ntotal = 0;
        lua_createtable(g_mainL, 0, 0);

        /* args[0] = script filename */
        if (load_file) {
            lua_pushstring(g_mainL, load_file);
            lua_rawseti(g_mainL, -2, 0);
            ntotal++;
        }

        /* args[1..] = extra command-line args (skip program name + script) */
        int start_idx = load_file ? 2 : 1;
        for (int i = start_idx; i < argc; i++) {
            lua_pushstring(g_mainL, argv[i]);
            lua_rawseti(g_mainL, -2, ntotal);
            ntotal++;
        }

        lua_setglobal(g_mainL, "argv");
        lua_pushinteger(g_mainL, ntotal);
        lua_setglobal(g_mainL, "argc");
    }

    /* Load persisted config (recent files, break-on-main, breakpoints) */
    config_load();

    /* Override print() to redirect to our console */
    lua_register(g_mainL, "print", lua_print_hook);

    /* Register some C functions available to the debuggee */
    lua_register(g_mainL, "c_add", [](lua_State *L) -> int {
        double a = luaL_checknumber(L, 1);
        double b = luaL_checknumber(L, 2);
        lua_pushnumber(L, a + b);
        return 1;
    });
    lua_register(g_mainL, "c_uppercase", [](lua_State *L) -> int {
        const char *s = luaL_checkstring(L, 1);
        char buf[256];
        size_t len = strlen(s);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        for (size_t i = 0; i < len; i++)
            buf[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
        buf[len] = '\0';
        lua_pushstring(L, buf);
        return 1;
    });

    /* _setmetatable: needed by constructor to assign __index → class table */
    lua_register(g_mainL, "_setmetatable", [](lua_State *L) -> int {
        luaL_checktype(L, 1, LUA_TTABLE);
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_setmetatable(L, 1);
        lua_settop(L, 1);
        return 1;
    });

    /* ---- 2. Create debuggee coroutine ---- */
    g_co = lua_newthread(g_mainL);
    /* Keep a reference in the registry so GC doesn't collect it */
    lua_pushvalue(g_mainL, -1);
    lua_setfield(g_mainL, LUA_REGISTRYINDEX, "_dbg_co");
    lua_pop(g_mainL, 1);

    /* ---- 3. Init GLFW + ImGui ---- */
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        fprintf(stderr, "Failed to init GLFW\n");
        lua_close(g_mainL);
        return 1;
    }

    const char *glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    g_window = glfwCreateWindow(1280, 900,
                                 "Lua Debugger - ImGui",
                                 nullptr, nullptr);
    if (!g_window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        lua_close(g_mainL);
        return 1;
    }
    GLFWwindow *window = g_window;

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    /* Init ImGui */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    /* Disable default imgui.ini — use our own layout file instead.
     * Must also clear any settings that were loaded during CreateContext()
     * (imgui.ini in cwd may contain stale dock data for old window IDs). */
    io.IniFilename = nullptr;
    ImGui::LoadIniSettingsFromMemory("", 0);

    /* Load CJK-capable font as primary; fall back to default if not found */
    {
        const char *cjk_paths[] = {
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
            "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        };
        bool loaded = false;
        for (const char *path : cjk_paths) {
            FILE *fp = fopen(path, "rb");
            if (fp) {
                fclose(fp);
                ImFontConfig cfg;
                /* FontNo=2 selects the SC (Simplified Chinese) face inside
                 * NotoSansCJK TTC; it is harmless but ignored for plain TTF. */
                bool is_ttc = (strstr(path, ".ttc") || strstr(path, ".TTC"));
                if (is_ttc) cfg.FontNo = 2;
                io.Fonts->AddFontFromFileTTF(path, 16.0f, &cfg,
                    io.Fonts->GetGlyphRangesChineseFull());
                loaded = true;
                break;
            }
        }
        if (!loaded)
            io.Fonts->AddFontDefault();
    }

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    /* Load saved window layout from ~/.luad/layout.ini if present */
    load_layout();

    /* ---- Auto-load a file if provided ---- */
    if (load_file) {
        snprintf(g_filepath_buf, sizeof(g_filepath_buf), "%s", load_file);
        dbg_load_script(load_file);
    }

    console_add("=== Lua Debugger ===", ImVec4(0.3f, 0.7f, 1.0f, 1.0f));
    console_add("Load a .lua or .java file to begin debugging.",
                ImVec4(0.7f, 0.7f, 0.7f, 1.0f));

    /* ---- 4. Main loop ---- */
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        /* Start ImGui frame */
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        /* ---- Draw main menu ---- */
        draw_main_menu();

        /* ---- Global keyboard shortcuts (active when not editing text) ---- */
        {
            bool can_step   = (g_dbg_state == DBG_PAUSED);
            bool can_cont   = (g_dbg_state == DBG_PAUSED);
            bool has_script = (g_dbg_state != DBG_IDLE);

            if (!ImGui::IsAnyItemActive()) {
                if (ImGui::IsKeyPressed(ImGuiKey_F5) && can_cont)  dbg_continue();
                if (ImGui::IsKeyPressed(ImGuiKey_F10) && can_step) dbg_step_over();
                if (ImGui::IsKeyPressed(ImGuiKey_F11) && can_step) {
                    if (ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
                        ImGui::IsKeyDown(ImGuiKey_RightShift))
                        dbg_step_out();
                    else
                        dbg_step_into();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_F2) && has_script &&
                    ImGui::IsKeyDown(ImGuiKey_LeftCtrl))
                    dbg_stop();
                if (ImGui::IsKeyPressed(ImGuiKey_O) &&
                    ImGui::IsKeyDown(ImGuiKey_LeftCtrl))
                    g_open_file_modal = true;
                if (ImGui::IsKeyPressed(ImGuiKey_R) &&
                    ImGui::IsKeyDown(ImGuiKey_LeftCtrl) &&
                    strlen(g_filepath_buf) > 0) {
                    dbg_stop();
                    dbg_load_script(g_filepath_buf);
                }
            }
        }

        /* ---- Draw debug toolbar (below menu bar, above dockspace) ---- */
        if (g_show_controls)
            draw_controls_toolbar();

        /* ---- Create dockspace (fills area below toolbar) ---- */
        {
            ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");

            /* Handle pending layout reset (deferred from menu callback) */
            if (g_pending_layout_reset) {
                g_pending_layout_reset = false;
                if (ImGui::DockBuilderGetNode(dockspace_id) != nullptr)
                    ImGui::DockBuilderRemoveNode(dockspace_id);
            }

            /* Build default layout on first frame (or after reset above) */
            if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr)
                build_default_dock_layout(dockspace_id);

            /* Host the dockspace node — fills area below toolbar */
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 dock_pos  = viewport->WorkPos;
            ImVec2 dock_size = viewport->WorkSize;
            if (g_show_controls) {
                dock_pos.y  += TOOLBAR_HEIGHT;
                dock_size.y -= TOOLBAR_HEIGHT;
            }
            ImGui::SetNextWindowPos(dock_pos);
            ImGui::SetNextWindowSize(dock_size);
            ImGui::SetNextWindowViewport(viewport->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGuiWindowFlags dock_flags = ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking;
            ImGui::Begin("MainDockSpaceWindow", nullptr, dock_flags);
            ImGui::PopStyleVar(2);
            ImGui::DockSpace(dockspace_id, ImVec2(0, 0),
                             ImGuiDockNodeFlags_PassthruCentralNode);
            ImGui::End();
        }

        /* ---- Draw all dockable windows (no hardcoded positions) ---- */
        if (g_show_source)       draw_source_view();
        if (g_show_callstack)    draw_call_stack();
        if (g_show_locals)       draw_locals();
        if (g_show_registers)    draw_registers();
        if (g_show_bytecode)     draw_bytecode_view();
        if (g_show_console)      draw_console();
        if (g_show_breakpoints)  draw_breakpoints();

        /* ---- Render ---- */
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.12f, 0.12f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        /* ---- Drive the debuggee coroutine ---- */
        if (g_dbg_state == DBG_RUNNING || g_dbg_state == DBG_STEPPING) {
            dbg_tick();
        }
    }

    /* ---- 5. Cleanup ---- */
    /* Save config before exit */
    config_save();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    lua_close(g_mainL);

    return 0;
}
