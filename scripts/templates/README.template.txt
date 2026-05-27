晨曦影音 · AuroraPlayer  v{{VERSION}}  (win64-{{VARIANT}})
项目地址: https://github.com/sm1906433038/AuroraPlayer

【运行】
    双击 AuroraPlayer.exe 即可。也可以把视频文件拖到 AuroraPlayer.exe 上。

【系统要求】
  - Windows 10 1903+ 或 Windows 11 (x64)
  - 如出现 "找不到 vcruntime140.dll" 等错误，请双击 vc_redist.x64.exe
    安装 Visual C++ 运行时（一次性，以后所有 MSVC 程序都不用再装）。
{{CUDA_REQUIREMENTS}}
【AI 字幕模型】
    首次使用 "AI 字幕 → 字幕生成" 功能时，需要下载 whisper 模型。
    程序会自动下载到:
        %AppData%\AuroraPlayer\AuroraPlayer\models\
    推荐先下 "small" (244 MB) 试用，再按需升级到 medium / large-v3。

【常见问题】
  Q: 启动时窗口闪一下就消失。
     A: 多半是 MSVC 运行时缺失，装一下 vc_redist.x64.exe。

  Q: AI 字幕跑得很慢 / 找不到 GPU。
     A: 检查显卡驱动是否够新；任务管理器看 nvcuda.dll 是否被加载。

  Q: HDR 视频颜色不对。
     A: 显示设置开启 HDR；或在播放器内右键选择 tone mapping。

【许可证】
    参见 THIRD-PARTY-LICENSES.txt。本程序基于 libmpv (LGPLv2.1+)、Qt 6
    (LGPLv3)、whisper.cpp (MIT)、FFmpeg (LGPLv2.1+) 等开源组件构建。
