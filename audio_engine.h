#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <vector>
#include <string>
#include <cstdint>
#include <atomic>
#include <functional>

// ============================================================================
// 链接需要的库
// ============================================================================
#pragma comment(lib, "ole32.lib")

// ============================================================================
// 音频设备信息
// ============================================================================
struct DeviceInfo {
    std::wstring name;       // 设备显示名称（用于 UI）
    std::wstring id;         // 设备端点 ID（用于初始化 WASAPI）
};

// ============================================================================
// 音频引擎状态
// ============================================================================
enum class AudioState {
    Idle,       // 无音频加载或已停止
    Ready,      // 音频已加载，待播放
    Playing,    // 正在播放
};

// ============================================================================
// AudioEngine - WASAPI 音频播放引擎
//
// 功能：
//   1. 枚举系统中的所有 WASAPI 渲染（输出）设备
//   2. 使用共享模式 + 事件驱动方式播放 PCM 音频
//   3. 支持音量控制（通过采样缩放实现）
//   4. 播放完成自动停止，通过窗口消息通知 UI
//
// 使用方式：
//   - 先调用 EnumerateRenderDevices() 获取设备列表
//   - 调用 Initialize(deviceId) 选择输出设备
//   - 调用 LoadPcmData(data, format) 加载解码后的音频
//   - 调用 Play() 开始播放，Stop() 停止
//   - 播放完毕自动回到 Idle 状态
//
// 线程安全：Play/Stop 可从任意线程调用
// ============================================================================

// 自定义消息：播放状态变化
// wParam: (WPARAM)AudioState 新状态
#define WM_AUDIO_STATE_CHANGE  (WM_APP + 0x100)

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // 禁止拷贝
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // --- 设备枚举 ---

    // 枚举所有活动的渲染设备（扬声器、耳机、虚拟电缆等）
    static std::vector<DeviceInfo> EnumerateRenderDevices();

    // --- 初始化 ---

    // 选择并初始化音频渲染设备
    // deviceId: 设备端点 ID 字符串（来自 DeviceInfo::id）
    // hNotifyWnd: 接收播放状态通知的窗口句柄
    bool Initialize(const std::wstring& deviceId, HWND hNotifyWnd);

    // 反初始化，释放 WASAPI 资源（自动在析构时调用）
    void Uninitialize();

    // --- 音频数据 ---

    // 加载 PCM 音频数据（应为 48000Hz, 16-bit, 立体声）
    void LoadPcmData(const BYTE* data, DWORD dataSize,
                     const WAVEFORMATEX& format);

    // 清除已加载的音频数据
    void ClearData();

    // --- 播放控制 ---

    void Play();
    void Stop();
    void SetVolume(float level);   // 0.0 ~ 1.0

    float    GetVolume() const { return m_volume; }
    AudioState GetState() const { return m_state; }
    bool     HasData() const { return !m_pcmData.empty(); }

private:
    // 播放线程入口
    static DWORD WINAPI PlaybackThreadProc(LPVOID param);
    void PlaybackLoop();

    // 向 WASAPI 缓冲区填充音频数据
    // 返回实际填充的帧数，0 表示播放完毕
    UINT32 FillBuffer(BYTE* buffer, UINT32 framesRequested);

    // 通知 UI 状态变化
    void NotifyState(AudioState newState);

    // --- COM 接口 ---
    IMMDeviceEnumerator* m_pEnumerator;
    IMMDevice*           m_pDevice;
    IAudioClient*        m_pAudioClient;
    IAudioRenderClient*  m_pRenderClient;

    // --- 音频格式 ---
    WAVEFORMATEX  m_mixFormat;       // WASAPI 协商后的实际格式
    UINT32        m_bufferSize;      // 缓冲区大小（帧数）
    UINT32        m_bytesPerFrame;   // 每帧字节数 = nBlockAlign

    // --- 播放数据 ---
    std::vector<BYTE> m_pcmData;     // 已解码的 PCM 音频数据
    UINT32            m_totalFrames; // 总帧数
    UINT32            m_currentFrame;// 当前播放位置（帧偏移）

    // --- 线程与同步 ---
    HANDLE        m_hEvent;          // WASAPI 缓冲区事件
    HANDLE        m_hThread;         // 播放线程句柄
    std::atomic<bool> m_isPlaying;   // 播放中标志
    std::atomic<float> m_volume;     // 当前音量

    // --- 状态 ---
    std::atomic<AudioState> m_state;
    HWND m_hNotifyWnd;               // 状态通知目标窗口
};
