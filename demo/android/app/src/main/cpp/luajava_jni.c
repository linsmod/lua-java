/* ============================================================
 * luajava_jni.c — Android demo 的 JNI 桥接
 *
 * 功能:
 *   1. 在进程内启动 Lua 5.3 VM, 注册 Java->Lua 编译器与运行时库
 *   2. 通过 java_setwriter() 重定向 System.out 输出 (不再依赖
 *      stdout/管道, 适配 Android 等 stdout 被重定向到 /dev/null 的平台)
 *   3. 接收 Java 源码字符串, 编译成 Lua 字节码并执行
 *   4. chunk 只负责注册类, main() 收集在 _MAIN_CANDIDATES 中,
 *      由本文件遍历并调用 (与桌面调试器 dbg_main.cpp 相同约定)
 * ============================================================ */
#define LUA_CORE
#include <jni.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include "jlib.h"
#include "jlex.h"

static lua_State *g_L = NULL;

/* ---- 输出缓冲: java_setwriter 回调把内容追加到这里 ---- */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} OutBuf;

static void outbuf_reset(OutBuf *o) {
    o->len = 0;
    if (o->buf) o->buf[0] = '\0';
}

static void outbuf_append(OutBuf *o, const char *s, size_t n) {
    if (o->len + n + 1 > o->cap) {
        size_t ncap = o->cap ? o->cap : 4096;
        while (ncap < o->len + n + 1) ncap *= 2;
        char *nb = (char *)realloc(o->buf, ncap);
        if (nb == NULL) return;  /* 内存不足: 丢弃后续输出 */
        o->buf = nb;
        o->cap = ncap;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

/* System.out.print/println 重定向回调 */
static void writer_cb(lua_State *L, const char *s, size_t len, void *ud) {
    (void)L;
    outbuf_append((OutBuf *)ud, s, len);
}

/* Lua print() 同样重定向 (演示代码用不到, 但保持行为一致) */
static int l_print(lua_State *L) {
    OutBuf *o = (OutBuf *)lua_touserdata(L, lua_upvalueindex(1));
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1) outbuf_append(o, "\t", 1);
        luaL_tolstring(L, i, NULL);
        size_t len;
        const char *s = lua_tolstring(L, -1, &len);
        outbuf_append(o, s, len);
        lua_pop(L, 1);
    }
    outbuf_append(o, "\n", 1);
    return 0;
}

static OutBuf g_out;

/* 对象创建辅助: jparser 生成的代码依赖这两个宿主函数 (与桌面版 main.c 一致) */
static int c_new_instance(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);  /* class_table */
    lua_newtable(L);                     /* obj = {} */
    lua_newtable(L);                     /* mt = {} */
    lua_pushvalue(L, 1);                /* mt, class_table */
    lua_setfield(L, -2, "__index");      /* mt.__index = class_table */
    lua_setmetatable(L, -2);            /* setmetatable(obj, mt) */
    return 1;
}

static int c_setmetatable(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_setmetatable(L, 1);
    lua_settop(L, 1);
    return 1;
}

/* 懒初始化 Lua VM (与桌面版 main.c 相同的初始化序列) */
static lua_State *get_lua(void) {
    if (g_L == NULL) {
        g_L = luaL_newstate();
        if (g_L == NULL) return NULL;
        luaL_openlibs(g_L);
        jlex_init(g_L);     /* 注册 Java 保留字 */
        java_openlib(g_L);  /* 注册 System.out、基础类型包装等 */
        lua_register(g_L, "_new_instance", c_new_instance);
        lua_register(g_L, "_setmetatable", c_setmetatable);

        /* 重定向输出: System.out.println/print → g_out */
        java_setwriter(g_L, writer_cb, &g_out);
        lua_pushlightuserdata(g_L, &g_out);
        lua_pushcclosure(g_L, l_print, 1);
        lua_setglobal(g_L, "print");
    }
    return g_L;
}

/*
 * chunk 执行完后, 遍历 _MAIN_CANDIDATES 调用 main(String[] args)。
 * 成功返回 main() 的返回值(exit code), 出错时把错误写入 g_out 并返回 -1。
 */
static int run_main_candidates(lua_State *L) {
    lua_getglobal(L, "_MAIN_CANDIDATES");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0;  /* 没有 main, 视为正常 (纯类定义) */
    }

    int exit_code = 0;
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        if (lua_isfunction(L, -1)) {
            lua_newtable(L);  /* args: 空的 String[] */
            if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
                outbuf_append(&g_out, "[RUNTIME ERROR] ", 16);
                outbuf_append(&g_out, lua_tostring(L, -1),
                              strlen(lua_tostring(L, -1)));
                outbuf_append(&g_out, "\n", 1);
                lua_pop(L, 1);
                exit_code = -1;
            } else {
                if (lua_isinteger(L, -1))
                    exit_code = (int)lua_tointeger(L, -1);
                lua_pop(L, 1);
            }
            break;  /* demo: 只跑第一个 main() */
        }
        lua_pop(L, 1);  /* 弹出值, 继续遍历 */
    }
    lua_pop(L, 1);  /* _MAIN_CANDIDATES 表 */
    return exit_code;
}

/*
 * 编译并运行一段 Java 源码。
 * 返回: 程序输出 + 状态行; 编译/运行错误以 [COMPILE ERROR]/[RUNTIME ERROR] 标出。
 */
JNIEXPORT jstring JNICALL
Java_com_example_luajavademo_MainActivity_nativeRunJava(JNIEnv *env, jobject thiz,
                                                        jstring source) {
    (void)thiz;
    const char *src = (*env)->GetStringUTFChars(env, source, NULL);
    if (src == NULL) return NULL;

    lua_State *L = get_lua();
    if (L == NULL) {
        (*env)->ReleaseStringUTFChars(env, source, src);
        return (*env)->NewStringUTF(env, "[ERROR] 无法创建 Lua VM");
    }

    outbuf_reset(&g_out);
    int exit_code = 0;

    int status = luaL_loadbufferx(L, src, strlen(src), "Demo.java", "j");
    if (status != LUA_OK) {
        outbuf_append(&g_out, "[COMPILE ERROR] ", 16);
        outbuf_append(&g_out, lua_tostring(L, -1), strlen(lua_tostring(L, -1)));
        outbuf_append(&g_out, "\n", 1);
        lua_pop(L, 1);
        exit_code = -1;
    } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        /* chunk 阶段出错 (import 失败等) */
        outbuf_append(&g_out, "[RUNTIME ERROR] ", 16);
        outbuf_append(&g_out, lua_tostring(L, -1), strlen(lua_tostring(L, -1)));
        outbuf_append(&g_out, "\n", 1);
        lua_pop(L, 1);
        exit_code = -1;
    } else {
        exit_code = run_main_candidates(L);
    }

    if (g_out.len == 0)
        outbuf_append(&g_out, "(无输出)\n", strlen("(无输出)\n"));

    char tail[160];
    snprintf(tail, sizeof(tail), "\n—— exit code: %d | %s ——\n",
             exit_code, LUA_RELEASE);
    outbuf_append(&g_out, tail, strlen(tail));

    jstring result = (*env)->NewStringUTF(env, g_out.buf ? g_out.buf : "");
    (*env)->ReleaseStringUTFChars(env, source, src);
    return result;
}

JNIEXPORT jstring JNICALL
Java_com_example_luajavademo_MainActivity_nativeVersion(JNIEnv *env, jobject thiz) {
    (void)thiz;
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s + Java->Lua compiler", LUA_RELEASE);
    return (*env)->NewStringUTF(env, tmp);
}

JNIEXPORT void JNICALL
Java_com_example_luajavademo_MainActivity_nativeClose(JNIEnv *env, jobject thiz) {
    (void)env;
    (void)thiz;
    if (g_L != NULL) {
        lua_close(g_L);
        g_L = NULL;
    }
    free(g_out.buf);
    g_out.buf = NULL;
    g_out.len = g_out.cap = 0;
}
