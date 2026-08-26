# ============================================================
# Demo App 的 native 构建:
# 直接把 Lua 5.3.6 + Java->Lua 前端源码编进 demo 的 .so,
# 不依赖外部预编译产物 (Lua 内部符号是 hidden 可见性,
# 链接预编译库会找不到符号)。
# ============================================================
cmake_minimum_required(VERSION 3.18)
project(luajava_demo C CXX)

# 仓库根目录: demo/android/app/src/main/cpp -> 上溯 6 层 (cpp/main/src/app/android/demo)
get_filename_component(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../ ABSOLUTE)
set(LUA_SRC_DIR ${REPO_ROOT}/lua-5.3.6/src)

set(LUA_SOURCES
    # Core
    lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c
    lgc.c llex.c lmem.c lobject.c lopcodes.c lparser.c
    lstate.c lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c
    # Lib
    lauxlib.c lbaselib.c lbitlib.c lcorolib.c ldblib.c liolib.c
    lmathlib.c loslib.c lstrlib.c ltablib.c lutf8lib.c loadlib.c linit.c
    # Java->Lua compiler + Java runtime library
    jlex.c jparser.c jlib.c
)

foreach(src ${LUA_SOURCES})
    list(APPEND LUA_FULL_SOURCES ${LUA_SRC_DIR}/${src})
endforeach()

add_library(luajava_demo SHARED
    luajava_jni.c
    ${LUA_FULL_SOURCES}
)

target_include_directories(luajava_demo PRIVATE ${LUA_SRC_DIR})

target_compile_definitions(luajava_demo PRIVATE
    LUA_COMPAT_5_2
    LUA_USE_LINUX
)

target_compile_options(luajava_demo PRIVATE
    -Wall
    -O2
)

target_link_libraries(luajava_demo PRIVATE m log)
