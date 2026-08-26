# ============================================================
# Android 交叉编译配置 (Lua 5.3.6 + Java->Lua 前端)
#
# 用法:
#   cmake -S android -B build/android-arm64-v8a \
#         -DCMAKE_TOOLCHAIN_FILE=/mnt/h/AndroidSdk/Sdk/ndk/29.0.13113456/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21
#   cmake --build build/android-arm64-v8a -j
# ============================================================
cmake_minimum_required(VERSION 3.18)

project(lua_android C)

if(NOT ANDROID)
    message(FATAL_ERROR
        "android/CMakeLists.txt 仅用于交叉编译, 请通过 -DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake 配置")
endif()

set(CMAKE_C_STANDARD 11)

set(LUA_SRC_DIR ${CMAKE_SOURCE_DIR}/../lua-5.3.6/src)

# Lua core + lib 源文件 (排除 lua.c / luac.c 这两个 standalone)
set(LUA_SOURCES
    # Core
    lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c
    lgc.c llex.c lmem.c lobject.c lopcodes.c lparser.c
    lstate.c lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c
    # Lib
    lauxlib.c lbaselib.c lbitlib.c lcorolib.c ldblib.c liolib.c
    lmathlib.c loslib.c lstrlib.c ltablib.c lutf8lib.c loadlib.c linit.c
    # Java->Lua compiler
    jlex.c jparser.c
    # Java runtime library
    jlib.c
)

foreach(src ${LUA_SOURCES})
    list(APPEND LUA_FULL_SOURCES ${LUA_SRC_DIR}/${src})
endforeach()

# 静态库 + 共享库共用同一批源文件
add_library(lua_static STATIC ${LUA_FULL_SOURCES})
add_library(lua_shared SHARED ${LUA_FULL_SOURCES})

set(LUA_PUBLIC_DEFINES
    -DLUA_COMPAT_5_2
    -DLUA_USE_LINUX
)
set(LUA_PUBLIC_FLAGS -Wall -Wextra -O2)

foreach(tgt lua_static lua_shared)
    target_include_directories(${tgt} PUBLIC ${LUA_SRC_DIR})
    target_compile_definitions(${tgt} PUBLIC ${LUA_PUBLIC_DEFINES})
    target_compile_options(${tgt} PRIVATE ${LUA_PUBLIC_FLAGS})
endforeach()

# 共享库产物: liblua.so
set_target_properties(lua_shared PROPERTIES OUTPUT_NAME lua)
target_link_libraries(lua_shared PRIVATE m)

# 安装规则 (默认装到 CMAKE_INSTALL_PREFIX, 可用 cmake --install 指定目录)
install(TARGETS lua_static lua_shared
    ARCHIVE DESTINATION lib/${ANDROID_ABI}
    LIBRARY DESTINATION lib/${ANDROID_ABI}
)
install(FILES
    ${LUA_SRC_DIR}/lua.h
    ${LUA_SRC_DIR}/luaconf.h
    ${LUA_SRC_DIR}/lualib.h
    ${LUA_SRC_DIR}/lauxlib.h
    ${LUA_SRC_DIR}/jlib.h
    DESTINATION include
)
