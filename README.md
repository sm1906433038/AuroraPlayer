# 晨曦影音 · AuroraPlayer

一个基于 **libmpv + Qt 6 + C++20** 的专业级影音播放器。

> 项目地址：<https://github.com/sm1906433038/AuroraPlayer>
>
> 中文名：**晨曦影音**　·　英文名：**AuroraPlayer**

## 设计目标

| 维度 | 选择 | 原因 |
|---|---|---|
| 解码 / 渲染内核 | **libmpv** | 支持 VP9 / AV1 / HEVC / 8K，硬解栈 (D3D11VA / NVDEC / QSV)，HDR tonemap |
| GUI 框架 | **Qt 6** (Widgets + OpenGL) | 性能、原生外观、跨平台 |
| 渲染管线 | `mpv_render_context` + `QOpenGLWidget` | mpv 直接渲染到我们的 OpenGL FBO，零拷贝 |
| 缩略图预览 | 第二个无头 mpv 实例 + `screenshot-raw` | 比 PotPlayer 更快、更准、按需异步 |
| VR / 360 | mpv 内置 `video-pan / video-zoom` + 立体声裁切 | 鼠标拖拽看视角 |
| 编译 | CMake ≥ 3.21 | 现代化，跨平台 |

## 一、安装依赖（Windows）

### 1. Visual Studio 2022 (Community 即可)

勾选 **"使用 C++ 的桌面开发"** 工作负载。CMake 也会一并装上。

### 2. Qt 6 (≥ 6.5，推荐 6.7+)

从 https://www.qt.io/download-qt-installer 下载在线安装器。

勾选：`Qt 6.7.x` → `MSVC 2022 64-bit`

安装后假设路径为 `C:\Qt\6.7.0\msvc2022_64`。

### 3. libmpv

最简单 — **直接下载预编译 SDK** ：

1. 去 https://sourceforge.net/projects/mpv-player-windows/files/libmpv/ 下载最新 `mpv-dev-x86_64-*.7z`
2. 解压后你会得到：
   ```
   mpv-dev/
   ├── include/mpv/   (头文件)
   ├── libmpv-2.dll   (运行时)
   └── libmpv.dll.a   (导入库, 重命名为 libmpv-2.lib 给 MSVC 用)
   ```
3. MSVC 需要 .lib，不能直接用 mingw 的 .dll.a，**生成 .lib**：

   在 `x64 Native Tools Command Prompt for VS 2022` 里：
   ```cmd
   cd path\to\mpv-dev
   dumpbin /exports libmpv-2.dll > exports.txt
   ```
   用一个文本编辑器把 `exports.txt` 中函数名整理成 `libmpv-2.def`：
   ```
   EXPORTS
       mpv_create
       mpv_initialize
       ... (所有 mpv_* 函数)
   ```
   然后：
   ```cmd
   lib /def:libmpv-2.def /name:libmpv-2.dll /out:libmpv-2.lib /machine:x64
   ```

4. 把整理好的目录布置到 `third_party/mpv/`：
   ```
   AuroraPlayer/third_party/mpv/
   ├── include/mpv/*.h
   ├── lib/libmpv-2.lib
   └── lib/libmpv-2.dll
   ```

> **或者**：用 vcpkg：`vcpkg install mpv:x64-windows`，然后给 CMake 传 `-DCMAKE_TOOLCHAIN_FILE=...vcpkg.cmake`。

---

## 二、编译

```powershell
cd D:\Projects\AuroraPlayer
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_PREFIX_PATH="C:/Qt/6.7.0/msvc2022_64"
cmake --build build --config Release -j
```

产物：`build/bin/Release/AuroraPlayer.exe`，运行时所需的所有 DLL 已经由 `windeployqt` 自动 copy 到同目录。

---

## 三、键位

| 键 | 功能 |
|---|---|
| `Space` | 播放 / 暂停 |
| `←` / `→` | 后退 / 前进 5 秒 |
| `↓` / `↑` | 后退 / 前进 60 秒 |
| `F` | 全屏切换 |
| `Esc` | 退出全屏 |
| `M` | 静音切换 |
| `Ctrl+O` | 打开文件 |
| `Ctrl+U` | 打开 URL（网络流） |
| `Ctrl+S` | 截图 |
| `Home` | 重置 VR 视角 |
| 鼠标双击视频区 | 全屏 |
| 鼠标滚轮 | 调节音量（VR 模式下变焦） |
| VR 模式 + 左键拖动 | 旋转视角 |

---

## 四、项目结构

```
AuroraPlayer/
├── CMakeLists.txt          # 顶层构建脚本
├── cmake/FindMpv.cmake     # 定位 libmpv
├── third_party/mpv/        # libmpv SDK（自己放进去）
├── resources/              # Qt 资源（图标 / 着色器）
└── src/
    ├── main.cpp
    ├── core/               # libmpv 封装
    │   └── MpvPlayer.{h,cpp}
    ├── ui/                 # 所有 Qt 控件
    │   ├── MainWindow.{h,cpp}
    │   ├── VideoWidget.{h,cpp}
    │   ├── ControlBar.{h,cpp}
    │   ├── SeekBar.{h,cpp}      # ← 解决拖动预览痛点
    │   └── ThumbnailWorker.{h,cpp}
    └── vr/
        └── VrController.{h,cpp}
```

---

## 五、关键设计点

### 1. 渲染：零拷贝 GPU 路径

`MpvPlayer::createRenderContext` 用 OpenGL backend 创建 mpv 渲染上下文；
`VideoWidget::paintGL` 把当前 QOpenGLWidget 的 FBO 句柄传给 mpv，
mpv 直接在 GPU 上完成解码、滤镜、scaler、tonemap 后把像素写进我们的 FBO。
**没有任何 CPU 拷贝**，画质和性能等价于原生 mpv。

### 2. 拖动进度条丝滑：双 mpv 实例 + LRU 缓存

`ThumbnailWorker` 在独立线程里维护一个 `vo=null, audio=no` 的 mpv，
通过 `seek absolute+keyframes` + `screenshot-raw` 在内存中直接拿到 BGRA 帧，
转成 `QImage` 通过 queued signal 推回 UI。最近请求覆盖旧请求 — 拖动越快越好。
1 秒粒度的 LRU 缓存（64 项）让来回拖动几乎瞬时。

### 3. VR / 360：内置投影 + 鼠标拖拽

`VrController` 通过 mpv 的 `video-pan-x / video-pan-y / video-zoom` 模拟 360 漫游。
立体视频用 `vf=lavfi=[crop=…]` 裁切左眼。
*MVP 版本*：完整 equirect → 透视投影计划用 mpv user shader 实现，见 `docs/VR.md`（TODO）。

---

## 六、Roadmap

- [ ] mpv user shader：完整 equirect-to-perspective（替代 pan/zoom 近似）
- [ ] 缩略图栏（PotPlayer 风格的滚动 timeline 预览）
- [ ] 字幕样式自定义 + 在线下载（OpenSubtitles）
- [ ] 音频/字幕轨道下拉切换
- [ ] mpris / SMTC 媒体键集成
- [ ] 配置面板（GPU shader、tonemap、HDR）
- [ ] 播放列表
- [ ] DeoVR-style VR HMD 输出（OpenXR）

---

## License

MIT (待加 LICENSE 文件)
