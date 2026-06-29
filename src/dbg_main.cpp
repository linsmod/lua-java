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

/* Breakpoints: filename → set of line numbers */
static std::map<std::string, std::set<int>> g_breakpoints;

/* Currently paused location */
static std::string g_cur_source;
static int         g_cur_line = -1;

/* Locals/upvalues cache — captured once on pause, never queried from
 * the yielded coroutine on subsequent frames (avoids stack corruption). */
struct LocalEntry {
    std::string name;
    std::string value;
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

/* Bytecode view cache — captured once on pause (Proto* is safe: GC stopped) */
static const void *g_bytecode_proto = nullptr;
static int         g_bytecode_curpc = -1;
static const void *g_bytecode_ci    = nullptr;  /* to validate cache validity */

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
static bool g_show_console    = true;
static bool g_show_breakpoints = true;

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
    return it->second.count(line) > 0;
}

static void toggle_breakpoint(const char *source, int line) {
    auto &s = g_breakpoints[source];
    if (s.count(line)) {
        s.erase(line);
        console_add(("Breakpoint removed at " + std::string(source) +
                     ":" + std::to_string(line)).c_str(),
                    ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
    } else {
        s.insert(line);
        console_add(("Breakpoint set at " + std::string(source) +
                     ":" + std::to_string(line)).c_str(),
                    ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
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
    case LUA_TUSERDATA:   snprintf(buf, bufsz, "userdata(%p)", lua_topointer(L, idx)); return buf;
    case LUA_TTHREAD:     snprintf(buf, bufsz, "thread(%p)", lua_topointer(L, idx)); return buf;
    default:              snprintf(buf, bufsz, "type:%d", t); return buf;
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
        g_locals_cache.push_back({name, val});
        lua_pop(L, 1);
    }

    /* Upvalues */
    lua_getinfo(L, "f", ar);
    int func_idx = lua_gettop(L);
    for (int i = 1; ; i++) {
        const char *name = lua_getupvalue(L, func_idx, i);
        if (!name) break;
        const char *val = lua_val_tostring(L, -1, valbuf, sizeof(valbuf));
        g_upvals_cache.push_back({name, val});
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
            /* Pause when we're back at the same or shallower depth */
            lua_getinfo(L, "nSlt", ar);
            /* Actually, simplest: pause on next line event at same depth */
            (void)ar;
            g_dbg_state = DBG_PAUSED;
            g_cur_source = src_key;
            g_cur_line   = line;
            break;
        }
        case STEP_INTO:
            /* Pause on any next line */
            g_dbg_state = DBG_PAUSED;
            g_cur_source = src_key;
            g_cur_line   = line;
            break;
        case STEP_OUT: {
            /* Count current depth; pause when shallower */
            int depth = 0;
            lua_Debug ar2;
            for (int i = 0; lua_getstack(L, i, &ar2); i++) depth++;
            if (depth < g_step_target) {
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
 *  Load & run a script in the debuggee coroutine
 * ============================================================ */
static bool dbg_load_script(const char *filename) {
    if (!g_co) return false;

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
    case LUA_OK:
        /* Coroutine finished normally */
        g_dbg_state = DBG_IDLE;
        console_add("--- Script finished ---", ImVec4(0.5f, 0.8f, 1.0f, 1.0f));
        lua_sethook(g_co, nullptr, 0, 0);
        return false;

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

            /* Clickable line text */
            char label[64];
            snprintf(label, sizeof(label), "##line%d", lineno);
            ImGui::PushID(lineno);

            if (ImGui::Selectable(label, is_cur,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                /* Click: set cursor; Double-click: toggle breakpoint */
                if (ImGui::IsMouseDoubleClicked(0)) {
                    toggle_breakpoint(g_cur_source.c_str(), lineno);
                }
            }

            ImGui::SameLine(38);
            /* Red dot for breakpoint */
            if (is_bp) {
                ImVec2 pos = ImGui::GetCursorScreenPos();
                float y_center = pos.y + ImGui::GetTextLineHeight() * 0.5f;
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(pos.x - 20, y_center), 4.0f,
                    IM_COL32(220, 50, 50, 255));
            }

            /* Highlight current line */
            if (is_cur) {
                ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    pos,
                    ImVec2(pos.x + ImGui::GetContentRegionAvail().x,
                           pos.y + ImGui::GetTextLineHeight()),
                    IM_COL32(60, 100, 180, 120));
            }

            ImGui::TextUnformatted(lines[i].c_str());
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

    int level = 0;
    for (auto &e : g_callstack_cache) {
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                           "[%d] ", level);
        ImGui::SameLine();
        ImGui::Text("%s  (%s:%d)", e.name.c_str(), e.source.c_str(), e.line);
        level++;
    }

    ImGui::End();
}

/* ============================================================
 *  UI: Local Variables  (reads from cache — never touches g_co)
 * ============================================================ */
static void draw_locals() {
    ImGui::Begin("Locals");

    if (!g_co || g_dbg_state != DBG_PAUSED) {
        ImGui::TextDisabled("(not paused)");
        ImGui::End();
        return;
    }

    ImGui::Columns(2, "locals_cols", true);
    ImGui::SetColumnWidth(0, 150);
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Name");
    ImGui::NextColumn();
    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Value");
    ImGui::NextColumn();
    ImGui::Separator();

    for (auto &e : g_locals_cache) {
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::NextColumn();
        ImGui::TextUnformatted(e.value.c_str());
        ImGui::NextColumn();
    }

    /* Upvalues */
    if (!g_upvals_cache.empty()) {
        ImGui::Separator();
        ImGui::NextColumn();
        ImGui::NextColumn();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Upvalues");
        ImGui::NextColumn();
        ImGui::NextColumn();
        ImGui::Separator();

        for (auto &e : g_upvals_cache) {
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::NextColumn();
            ImGui::TextUnformatted(e.value.c_str());
            ImGui::NextColumn();
        }
    }

    ImGui::Columns(1);
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

    if (auto_scroll)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();

    /* Input area */
    ImGui::PushItemWidth(-1);
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
        }
    }
    ImGui::PopItemWidth();

    /* Set keyboard focus to input when clicked */
    if (ImGui::IsItemHovered() ||
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
         !ImGui::IsAnyItemActive() && !ImGui::IsMouseClicked(0))) {
        ImGui::SetKeyboardFocusHere(-1);
    }

    ImGui::End();
}

/* ============================================================
 *  UI: Main Menu Bar + Compact Toolbar
 * ============================================================ */
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
            ImGui::MenuItem("Console",       nullptr, &g_show_console);
            ImGui::MenuItem("Breakpoints",   nullptr, &g_show_breakpoints);
            ImGui::EndMenu();
        }

        /* Help */
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) {
                ImGui::OpenPopup("About##Popup");
            }
            ImGui::EndMenu();
        }

        /* ---- Keyboard shortcuts (active when not editing text) ---- */
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
            if (ImGui::IsKeyPressed(ImGuiKey_O) &&
                ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) {
                g_open_file_modal = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_R) &&
                ImGui::IsKeyDown(ImGuiKey_LeftCtrl) &&
                strlen(g_filepath_buf) > 0) {
                dbg_stop();  /* dbg_stop already creates a new thread */
                dbg_load_script(g_filepath_buf);
            }
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
        if (g_dbg_state == DBG_PAUSED && g_cur_line > 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s:%d",
                     short_name(g_cur_source.c_str()), g_cur_line);
            status_w += ImGui::CalcTextSize(buf).x + 10;
        }
        ImGui::SetCursorPosX(menu_width - status_w - 10);
        ImGui::TextColored(status_color, "[%s]", status_text);
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

    /* ---- Compact Toolbar (below menu bar) ---- */
    ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetFrameHeight()));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x,
                                     ImGui::GetFrameHeight() * 2 + 12));
    ImGui::Begin("##Toolbar", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoSavedSettings);

    /* File path input */
    ImGui::PushItemWidth(280);
    ImGui::InputTextWithHint("##filepath", "script.lua / script.java",
                              g_filepath_buf, sizeof(g_filepath_buf));
    ImGui::PopItemWidth();
    ImGui::SameLine();

    /* Browse button — opens native file dialog, falls back to modal */
    if (ImGui::Button("Browse...")) {
        std::string chosen = native_file_dialog();
        if (!chosen.empty()) {
            snprintf(g_filepath_buf, sizeof(g_filepath_buf),
                     "%s", chosen.c_str());
            dbg_load_script(chosen.c_str());
        } else if (!g_zenity_available) {
            /* Fallback: show text input modal */
            if (strlen(g_filepath_buf) == 0)
                g_filepath_buf[0] = '\0';
            g_open_file_modal = true;
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Open file via system dialog");
    ImGui::SameLine();

    if (ImGui::Button("Load")) {
        if (strlen(g_filepath_buf) > 0) dbg_load_script(g_filepath_buf);
    }
    ImGui::SameLine();

    /* Execution buttons */
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    if (!can_cont) ImGui::BeginDisabled();
    if (ImGui::ArrowButton("##continue", ImGuiDir_Right)) dbg_continue();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Continue (F5)");
    if (!can_cont) ImGui::EndDisabled();
    ImGui::SameLine();

    if (!can_step) ImGui::BeginDisabled();
    if (ImGui::Button("v", ImVec2(0, 0))) dbg_step_into();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Step Into (F11)");
    ImGui::SameLine();
    if (ImGui::Button(">", ImVec2(0, 0))) dbg_step_over();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Step Over (F10)");
    ImGui::SameLine();
    if (ImGui::Button("^", ImVec2(0, 0))) dbg_step_out();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Step Out (Shift+F11)");
    if (!can_step) ImGui::EndDisabled();
    ImGui::SameLine();

    if (!has_script) ImGui::BeginDisabled();
    if (ImGui::Button("||", ImVec2(0, 0))) dbg_pause();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Pause");
    ImGui::SameLine();
    if (ImGui::Button("[]", ImVec2(0, 0))) dbg_stop();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Stop (Ctrl+F2)");
    if (!has_script) ImGui::EndDisabled();

    ImGui::End();
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

    /* We need a copy to safely erase while iterating */
    std::vector<std::pair<std::string, int>> to_remove;

    for (auto &fentry : g_breakpoints) {
        const std::string &file = fentry.first;
        auto &lines = fentry.second;

        std::vector<int> sorted_lines(lines.begin(), lines.end());
        std::sort(sorted_lines.begin(), sorted_lines.end());

        for (int line : sorted_lines) {
            char label[256];
            snprintf(label, sizeof(label), "%s:%d", file.c_str(), line);
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
            char btn_label[64];
            snprintf(btn_label, sizeof(btn_label), "X##%s_%d",
                     file.c_str(), line);
            if (ImGui::SmallButton(btn_label)) {
                to_remove.push_back({file, line});
            }
        }
    }

    for (auto &r : to_remove) {
        auto it = g_breakpoints.find(r.first);
        if (it != g_breakpoints.end()) {
            it->second.erase(r.second);
            if (it->second.empty())
                g_breakpoints.erase(it);
        }
    }

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
    for (auto &fe : g_breakpoints) {
        for (int ln : fe.second)
            fprintf(f, "bp %s %d\n", fe.first.c_str(), ln);
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
        } else if (strcmp(type, "bp") == 0) {
            int n = sscanf(line, "%*s %2047s %d", arg1, &arg2);
            if (n == 2 && arg2 > 0)
                g_breakpoints[arg1].insert(arg2);
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
    io.Fonts->AddFontDefault();

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

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

        /* ---- Draw main menu + toolbar ---- */
        draw_main_menu();

        /* Menu bar ~20px + toolbar ~35px = ~55px offset */
        const float TOP_Y = 58.0f;

        /* Source code takes the left side */
        if (g_show_source) {
            ImGui::SetNextWindowPos(ImVec2(0, TOP_Y), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(640, 490), ImGuiCond_FirstUseEver);
            draw_source_view();
        }

        /* Call stack on the right */
        if (g_show_callstack) {
            ImGui::SetNextWindowPos(ImVec2(640, TOP_Y), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(300, 200), ImGuiCond_FirstUseEver);
            draw_call_stack();
        }

        /* Locals on the right */
        if (g_show_locals) {
            ImGui::SetNextWindowPos(ImVec2(640, 260 + TOP_Y - 50),
                                    ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(300, 290), ImGuiCond_FirstUseEver);
            draw_locals();
        }

        /* Bytecode on the right */
        if (g_show_bytecode) {
            ImGui::SetNextWindowPos(ImVec2(940, TOP_Y), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(340, 490), ImGuiCond_FirstUseEver);
            draw_bytecode_view();
        }

        /* Console at the bottom */
        if (g_show_console) {
            ImGui::SetNextWindowPos(ImVec2(0, 550), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(940, 320), ImGuiCond_FirstUseEver);
            draw_console();
        }

        /* Breakpoints panel */
        if (g_show_breakpoints) {
            ImGui::SetNextWindowPos(ImVec2(940, 550), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(340, 320), ImGuiCond_FirstUseEver);
            draw_breakpoints();
        }

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
