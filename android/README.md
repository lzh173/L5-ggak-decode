# GGAK Android

Android 版本采用与 SatDump 类似的 NativeActivity、EGL、OpenGL ES 3 和 Dear ImGui 组合。

用 Android Studio 打开本目录后运行 `app`，或在已安装 Android SDK/NDK 和 Gradle 8.7 的环境中执行：

```powershell
gradle :app:assembleDebug
```

GitHub Actions 会固定安装 Java 17、Android SDK 35、NDK 27.0.12077973、CMake 3.22.1 和 Gradle 8.7，构建结果以 `ggak-android-debug` artifact 上传。

当前版本支持系统文件选择器、离线 CADU 解码、概览、可触摸缩放/拖动的仪器曲线，以及连接 SatDump
`network_server` 的 NNG SUB 实时输入。实时模式使用与桌面端相同的协议：地址默认为
`127.0.0.1:8888`，一个 NNG message 中可以提取多个 224-byte CADU；全 `0x33` 的 idle message
会单独统计，不会被误算成有效数据帧。
