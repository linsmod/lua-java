cmake_minimum_required(VERSION 3.10)
project(lua_test C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================
# 从 apt 下载的 Lua 5.3.6 源码编译
# ============================================================
set(LUA_SRC_DIR ${CMAKE_SOURCE_DIR}/lua-5.3.6/src)

# Lua core + lib 源文件 (排除 lua.c / luac.c 这两个 standalone)
set(LUA_SOURCES
    # Core
    lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c
    lgc.c llex.c lmem.c lobject.c lopcodes.c lparser.c
    lstate.c lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c
    # Lib
    lauxlib.c lbaselib.c lbitlib.c lcorolib.c ldblib.c liolib.c
    lmathlib.c loslib.c lstrlib.c ltablib.c lutf8lib.c loadlib.c linit.c
    # Java→Lua compiler
    jlex.c jparser.c
    # Java runtime library
    jlib.c
)

# 添加前缀路径
foreach(src ${LUA_SOURCES})
    list(APPEND LUA_FULL_SOURCES ${LUA_SRC_DIR}/${src})
endforeach()

# 编译为静态库
add_library(lua_source STATIC ${LUA_FULL_SOURCES})
target_include_directories(lua_source PUBLIC ${LUA_SRC_DIR})
target_compile_options(lua_source PRIVATE
    -std=gnu99
    -Wall -Wextra
    -DLUA_COMPAT_5_2
    -DLUA_USE_LINUX
    -fsanitize=address
    -fno-omit-frame-pointer
)

# ============================================================
# 我们的测试程序
# ============================================================
add_executable(lua_test src/main.c)
target_link_libraries(lua_test PRIVATE lua_source m dl)
target_compile_options(lua_test PRIVATE
    -fsanitize=address
    -fno-omit-frame-pointer
)
target_link_options(lua_test PRIVATE
    -fsanitize=address
)

# ============================================================
# ImGui
# ============================================================
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_options(imgui PRIVATE
    -fsanitize=address
    -fno-omit-frame-pointer
)

# ---- 查找 GLFW3 + OpenGL ---- */
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW3 REQUIRED glfw3)
pkg_check_modules(GL    REQUIRED gl)

# ============================================================
# Lua Debugger (C++ with ImGui)
# ============================================================
add_executable(lua_dbg src/dbg_main.cpp)
target_include_directories(lua_dbg PRIVATE
    ${LUA_SRC_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${GLFW3_INCLUDE_DIRS}
    ${GL_INCLUDE_DIRS}
)
target_link_libraries(lua_dbg PRIVATE
    lua_source
    imgui
    m dl
    ${GLFW3_LIBRARIES}
    ${GL_LIBRARIES}
)
target_compile_options(lua_dbg PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -fsanitize=address
    -fno-omit-frame-pointer
)
target_link_options(lua_dbg PRIVATE
    -fsanitize=address
)

# 测试
enable_testing()
add_test(NAME lua_test COMMAND lua_test)
