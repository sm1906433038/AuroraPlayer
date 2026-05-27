# 安装清单 — 从零到能跑

> 你机器当前已**没有** Visual Studio、Qt、CMake、libmpv。下面按**最小工作量**顺序，预计总耗时 30~60 分钟（大部分是下载等待）。

---

## 0. 先决条件

| 项 | 必要？ | 用途 |
|---|---|---|
| 磁盘空间 | 必要 | 预留 ~15 GB（VS 占大头） |
| Windows 10/11 64-bit | 必要 | Qt 6 不支持 32-bit |
| 7-Zip | **可选** | 解 libmpv 的 .7z 包；如果没装，脚本会自动下载官方的便携版 7zr.exe（约 600 KB） |

---

## 1. Visual Studio 2022 Community（C++ 工具链）

**用途**：MSVC 编译器（`cl.exe`）、`lib.exe`、`dumpbin.exe`、CMake、Windows SDK。

1. 下载：<https://visualstudio.microsoft.com/zh-hans/vs/community/>
2. 安装器里**必须勾选**：
   - ✅ **使用 C++ 的桌面开发** (`Desktop development with C++`)

   保持默认子组件即可。Windows SDK、CMake、MSBuild 都会一并装上。
3. 安装完不需要重启，但要等左下角"Microsoft Visual Studio Installer"显示"已修改"才算完。

**验证**：开始菜单里能找到 **"x64 Native Tools Command Prompt for VS 2022"** 就成功了。

---

## 2. Qt 6（图形界面框架）

**用途**：UI、OpenGL widget、windeployqt。

1. 下载在线安装器：<https://www.qt.io/download-qt-installer-oss>
   （免费 LGPL 版，按提示用邮箱注册一个账号即可）
2. 安装路径**保持默认** `C:\Qt`，方便脚本自动定位。
3. 在 "Select Components" 这一步：
   - 展开 **Qt** → **Qt 6.7.x**（或当前最新 6.x）
   - 勾选 ✅ **MSVC 2022 64-bit**
   - 其它（Android / WebAssembly / sources / Creator）**全部不勾**，节省 ~10 GB
4. 等待下载安装（约 2–5 GB）。

**验证**：`C:\Qt\6.7.x\msvc2022_64\bin\windeployqt.exe` 存在。

---

## 3. libmpv（音视频内核）

**用途**：解码、渲染、HDR、硬解。

**就一行命令**：

```powershell
cd D:\AuroraPlayer
.\scripts\setup-libmpv.ps1
```

脚本会自动：
1. 从 SourceForge 下载最新 `mpv-dev-x86_64.7z`
2. 用 7-Zip 解压
3. 调用 VS 的 `dumpbin` + `lib.exe` 生成 MSVC 兼容的 `libmpv-2.lib`
4. 把 headers / DLL / lib 放到 `D:\AuroraPlayer\third_party\mpv\` 下

**如果脚本报错**："Could not bring lib.exe into PATH"，那么改用 **"x64 Native Tools Command Prompt for VS 2022"** 打开后再运行同样的命令即可。

---

## 4. 编译

```powershell
cd D:\AuroraPlayer
.\scripts\build.ps1
```

脚本会自动：
1. 检测 Qt 安装位置
2. 检测 VS 安装位置
3. 用 CMake 生成 VS 2022 解决方案到 `build/`
4. 编译 Release
5. 自动把 Qt + libmpv 的 DLL 拷贝到 `build/bin/Release/`

第一次配置约 20 秒，编译约 1–3 分钟。

**运行**：
```powershell
.\build\bin\Release\AuroraPlayer.exe "D:\some\video.mkv"
```

或者一条龙：
```powershell
.\scripts\build.ps1 -Run
```

常用参数：
```powershell
.\scripts\build.ps1 -Config Debug       # 调试构建
.\scripts\build.ps1 -Clean              # 清理后重新编译
.\scripts\build.ps1 -QtRoot "D:\Qt\6.7.0\msvc2022_64"   # 手动指定 Qt
```

---

## 5. 在 PyCharm 里也能开（可选）

PyCharm Professional 通过插件**可以**编辑 C++，但比 CLion / VS Code 难用。
建议：用 **VS Code + C/C++ + CMake Tools 扩展**，打开 `D:\AuroraPlayer` 即可获得：
- IntelliSense（CMakeLists.txt 自动识别）
- 一键 Build / Debug
- 断点调试 mpv 内部都行

或者直接用 **Visual Studio 2022 打开 `build/AuroraPlayer.sln`**，体验最原生。

---

## 故障排查速查

| 现象 | 原因 | 解法 |
|---|---|---|
| `setup-libmpv.ps1` 找不到 lib.exe | 没在 VS 开发者环境里 | 改用"x64 Native Tools Command Prompt"打开 |
| `setup-libmpv.ps1` 下载 7zr.exe 失败 | 网络问题 | 手动从 https://www.7-zip.org/a/7zr.exe 下载，放到 `D:\AuroraPlayer\.cache\7zr.exe` |
| `build.ps1` 找不到 Qt | 不在标准路径 | `$env:QT_ROOT="D:\Qt\6.7.0\msvc2022_64"` 再跑 |
| 运行时报 "0xc000007b" | Qt DLL 缺失 | 删 build 重新跑 `build.ps1`，windeployqt 会重新部署 |
| 黑屏不出画面 | 显卡驱动太老 / Optimus 切换问题 | 显卡驱动更新到最新；右键 AuroraPlayer.exe → 用独显运行 |
| 编译 warning C4127 等 | MSVC 严格警告 | 不影响产物，先忽略 |
