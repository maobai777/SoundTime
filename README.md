# SoundTime 🎵

本地音频虚拟麦克风播放器 —— 将 WAV/MP3 音频文件播放到任意音频输出设备，配合 VB-Cable 等虚拟音频电缆实现"音频注入麦克风"效果。

## ✨ 功能

- 🎧 支持 **WAV** 和 **MP3** 音频文件播放
- 🔉 可选择任意 WASAPI 渲染设备作为输出目标
- 🎚️ 实时音量调节
- 🔌 配合 [VB-Cable](https://vb-audio.com/Cable/) 等虚拟音频电缆，可将音频注入麦克风输入
- 🪟 极简 Win32 原生界面，无额外依赖，启动即用
- ⚡ 事件驱动的低延迟音频播放引擎

## 🖥️ 系统要求

- Windows 10/11 64 位
- 无需安装，单文件运行

## 🔧 构建

```bash
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

## 🚀 使用说明

1. 下载安装 [VB-Cable 虚拟音频电缆](https://vb-audio.com/Cable/)（如需麦克风注入功能）
2. 启动 `SoundTime.exe`
3. 点击「打开文件」选择 WAV 或 MP3 音频
4. 在设备下拉列表中选择输出设备（如 **CABLE Input**）
5. 点击播放，音频将通过所选设备输出

> 将 VB-Cable 设为系统默认麦克风后，其他应用（如游戏语音、会议软件）即可接收到 SoundTime 播放的音频。

## 🛠️ 技术栈

- **语言**: C++17
- **UI**: 纯 Win32 API (CreateWindow)
- **音频**: Windows Core Audio APIs (WASAPI / MMDevice)
- **解码**: ACM (WAV) + Media Foundation (MP3)

## 📁 项目结构

```
SoundTime/
├── main.cpp           # Win32 界面 + 主逻辑
├── audio_engine.cpp/h # WASAPI 音频播放引擎
├── wav_reader.cpp/h   # WAV 文件解码器
├── mp3_decoder.cpp/h  # MP3 解码器 (Media Foundation)
└── CMakeLists.txt
```

## 📄 许可

MIT License
