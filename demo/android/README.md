# Lua-Java Android Demo

在 Android 上把 **Java 源码直接编译成 Lua 5.3 字节码并执行** 的演示应用。

## 原理

```
Java 源码 (EditText 输入)
   │  JNI (luajava_jni.c)
   ▼
luaL_loadbufferx(L, src, len, "Demo.java", "j")   ← "j" 模式触发 Java→Lua 编译器
   │  jlex.c / jparser.c  →  Lua 5.3 字节码
   ▼
chunk 执行: 只负责注册类; static main() 收集进全局表 _MAIN_CANDIDATES
   │  宿主遍历 _MAIN_CANDIDATES 并调用 main() (与桌面调试器同一约定)
   ▼
Lua VM 执行 (jlib.c 提供 System.out、ArrayList、HashMap 等运行时)
   │  System.out 输出经 java_setwriter() 回调直接进内存缓冲 (不走 stdout)
   ▼
结果显示在界面下方
```

### 库 API (jlib.h)

```c
/* 重定向 System.out.print/println 的输出目标;
 * 不设置时默认写 stdout (桌面行为不变) */
typedef void (*jlib_writer_t)(lua_State *L, const char *s, size_t len, void *ud);
void java_setwriter(lua_State *L, jlib_writer_t w, void *ud);
```

宿主集成三件套 (除 luaL_openlibs 外):

```c
jlex_init(L);              /* Java 保留字 */
java_openlib(L);           /* System.out / 集合 / 基础类型 */
java_setwriter(L, cb, ud); /* 可选: 输出重定向 */
lua_register(L, "_new_instance", ...);  /* 对象创建辅助, jparser 生成代码依赖 */
lua_register(L, "_setmetatable", ...);
```

## 工程结构

```
demo/android/
├── settings.gradle / build.gradle / gradle.properties
├── local.properties                 # SDK 路径 (已指向 H:\AndroidSdk\Sdk)
└── app/
    ├── build.gradle                 # externalNativeBuild → CMake
    └── src/main/
        ├── AndroidManifest.xml
        ├── cpp/
        │   ├── CMakeLists.txt       # 直接把仓库里 Lua 源码编进 .so
        │   └── luajava_jni.c        # JNI 桥接 + stdout 捕获
        ├── java/.../MainActivity.java
        └── res/
```

> 说明: demo 不链接 `build/install/` 里的预编译库 —— 因为 Lua 内部符号
> 是 hidden 可见性，跨 .so 链接会缺符号，所以这里直接从源码编入。

## 构建运行

1. 用 Android Studio 打开 `demo/android` 目录；
2. 首次同步若提示缺少 Gradle Wrapper，让 Android Studio 自动生成即可
   （`gradle-wrapper.properties` 已指定 Gradle 8.7，配合 AGP 8.5.2）；
3. 连接设备或启动模拟器（`abiFilters`: arm64-v8a / armeabi-v7a / x86_64）；
4. Run 'app'。

NDK 版本锁定为 `29.0.13113456`（在 `app/build.gradle` 中，可按已安装版本调整）。

## Demo 功能

- 四个内置示例按钮: Hello、循环、集合 (ArrayList/HashMap)、类与对象 (构造器 + 实例方法)
- 源码可自由编辑, 点 “编译并运行” 即可
- 编译错误 / 运行错误 / 程序输出分别显示
