# GGAK Android

Android 版本采用与 SatDump 类似的 NativeActivity、EGL、OpenGL ES 3 和 Dear ImGui 组合。

用 Android Studio 打开本目录后运行 `app`，或在已安装 Android SDK/NDK 和 Gradle 8.7 的环境中执行：

```powershell
gradle :app:assembleDebug
```

GitHub Actions 会固定安装 Java 17、Android SDK 35、NDK 27.0.12077973、CMake 3.22.1 和 Gradle 8.7，构建结果以 `ggak-android-debug` artifact 上传。

首个版本支持系统文件选择器、离线 CADU 解码、概览以及基础仪器曲线。实时 NNG 输入将在 NNG Android ABI 构建接入后启用。
