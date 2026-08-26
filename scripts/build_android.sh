#!/usr/bin/env bash
# ============================================================
# 使用 Android NDK 交叉编译 Lua 5.3.6 + Java->Lua 前端
# 产物: build/install/<ABI>/{liblua.a, liblua.so, include/*.h}
# ============================================================
set -euo pipefail

cd "$(dirname "$0")/.."

NDK="${NDK:-/mnt/h/AndroidSdk/Sdk/ndk/29.0.13113456}"
# 也可覆盖: NDK=... ABI="arm64-v8a" ./build_android.sh
ABIS="${ABI:-arm64-v8a armeabi-v7a x86_64}"
PLATFORM="${ANDROID_PLATFORM:-android-21}"

if [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    echo "错误: 找不到 NDK toolchain: $NDK" >&2
    exit 1
fi

for abi in $ABIS; do
    echo "======== 构建 ABI: $abi ========"
    cmake -S android -B "build/android-$abi" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM="$PLATFORM" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$(pwd)/build/install/$abi"
    cmake --build "build/android-$abi" -j"$(nproc)"
    cmake --install "build/android-$abi"
done

echo "======== 完成, 产物列表 ========"
find build/install -type f | sort
