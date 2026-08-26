# ============================================================
# 使用 Android NDK (Windows 版) 交叉编译 Lua 5.3.6 + Java->Lua 前端
# 用法 (PowerShell):  .\scripts\build_android.ps1
#   可选环境变量覆盖:
#     $env:NDK   = "H:\AndroidSdk\Sdk\ndk\29.0.13113456"
#     $env:ABI   = "arm64-v8a"                    # 仅编译单个 ABI
#     $env:ANDROID_PLATFORM = "android-21"
# 产物: build\install\<ABI>\{liblua.a, liblua.so, include\*.h}
# ============================================================
$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

$NDK = if ($env:NDK) { $env:NDK } else { "H:\AndroidSdk\Sdk\ndk\29.0.13113456" }
$Platform = if ($env:ANDROID_PLATFORM) { $env:ANDROID_PLATFORM } else { "android-21" }
$ABIs = if ($env:ABI) { $env:ABI -split '\s+' | Where-Object { $_ } } else { @("arm64-v8a", "armeabi-v7a", "x86_64") }

$Toolchain = Join-Path $NDK "build\cmake\android.toolchain.cmake"
if (-not (Test-Path $Toolchain)) {
    Write-Error "错误: 找不到 NDK toolchain: $Toolchain"
}

# ---- 查找 Ninja (NDK toolchain 要求 Ninja 生成器) ----
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $ninja) {
    # 尝试 Visual Studio 自带的 Ninja
    $vsNinja = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vsNinja) {
        $env:PATH = "$($vsNinja.DirectoryName);$env:PATH"
        Write-Host "使用 VS 自带 Ninja: $($vsNinja.FullName)" -ForegroundColor Yellow
    } else {
        Write-Error "未找到 ninja.exe, 请先安装 (如: winget install Ninja-build.Ninja)"
    }
}

$Jobs = if ($env:NUMBER_OF_PROCESSORS) { $env:NUMBER_OF_PROCESSORS } else { 4 }

foreach ($abi in $ABIs) {
    Write-Host "======== 构建 ABI: $abi ========" -ForegroundColor Cyan
    $buildDir = Join-Path (Get-Location) "build\android-$abi"
    $installDir = Join-Path (Get-Location) "build\install\$abi"

    cmake -S android -B $buildDir --fresh -G Ninja -DCMAKE_TOOLCHAIN_FILE="$Toolchain" -DANDROID_ABI="$abi" -DANDROID_PLATFORM="$Platform" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$installDir"
    if ($LASTEXITCODE -ne 0) { Write-Error "configure 失败: $abi" }

    cmake --build $buildDir -j $Jobs
    if ($LASTEXITCODE -ne 0) { Write-Error "编译失败: $abi" }

    cmake --install $buildDir
    if ($LASTEXITCODE -ne 0) { Write-Error "install 失败: $abi" }
}

Write-Host "======== 完成, 产物列表 ========" -ForegroundColor Green
Get-ChildItem -Recurse -File build\install | Select-Object -ExpandProperty FullName
