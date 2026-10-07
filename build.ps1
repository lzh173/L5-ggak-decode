$env:PATH = "D:\MSYS2\ucrt64\bin;D:\MSYS2\usr\bin;$env:PATH"
$ErrorActionPreference = "Stop"
$buildDir = Join-Path $PSScriptRoot "build-gui"

cmake -S $PSScriptRoot -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build $buildDir --parallel

Write-Host "构建完成: $buildDir\ggak_gui.exe"
