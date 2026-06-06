#include "audio_engine.h"
#include <functiondiscoverykeys_devpkey.h>
#include <cstring>

// ============================================================================
// MinGW 兼容：手动定义 PKEY_Device_FriendlyName
// （MinGW 的 libuuid.a 可能不导出此符号）
// ============================================================================
EXTERN_C const PROPERTYKEY DECLSPEC_SELECTANY PKEY_Device_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    14
};

// ============================================================================
// 辅助宏：安全释放 COM 接口
// ============================================================================
#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = NULL; } } while(0)

// ============================================================================
// 构造与析构
// ============================================================================

AudioEngine::AudioEngine()
    : m_pEnumerator(NULL)
    , m_pDevice(NULL)
    , m_pAudioClient(NULL)
    , m_pRenderClient(NULL)
    , m_bufferSize(0)
    , m_bytesPerFrame(0)
    , m_totalFrames(0)
    , m_currentFrame(0)
    , m_hEvent(NULL)
    , m_hThread(NULL)
    , m_hNotifyWnd(NULL)
{
    ZeroMemory(&m_mixFormat, sizeof(m_mixFormat));
    m_isPlaying.store(false);
    m_volume.store(1.0f);
    m_state.store(AudioState::Idle);
}

AudioEngine::~AudioEngine() {
    Stop();
    Uninitialize();
}

// ============================================================================
// 设备枚举
// ============================================================================

std::vector<DeviceInfo> AudioEngine::EnumerateRenderDevices() {
    std::vector<DeviceInfo> devices;

    IMMDeviceEnumerator* pEnumerator = NULL;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);

    if (FAILED(hr)) return devices;

    IMMDeviceCollection* pCollection = NULL;
    hr = pEnumerator->EnumAudioEndpoints(
        eRender, DEVICE_STATE_ACTIVE, &pCollection);

    if (FAILED(hr)) {
        SAFE_RELEASE(pEnumerator);
        return devices;
    }

    UINT count = 0;
    pCollection->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice* pDevice = NULL;
        if (FAILED(pCollection->Item(i, &pDevice))) continue;

        // 获取设备名称
        IPropertyStore* pProps = NULL;
        LPWSTR pwszID = NULL;

        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
            PROPVARIANT varName;
            PropVariantInit(&varName);

            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
                // 获取设备端点 ID
                pDevice->GetId(&pwszID);

                DeviceInfo info;
                info.name = varName.pwszVal ? varName.pwszVal : L"未知设备";
                info.id   = pwszID ? pwszID : L"";
                devices.push_back(info);

                if (pwszID) CoTaskMemFree(pwszID);
            }

            PropVariantClear(&varName);
            SAFE_RELEASE(pProps);
        }

        SAFE_RELEASE(pDevice);
    }

    SAFE_RELEASE(pCollection);
    SAFE_RELEASE(pEnumerator);

    return devices;
}

// ============================================================================
// Initialize - 初始化指定音频设备
// ============================================================================

bool AudioEngine::Initialize(const std::wstring& deviceId, HWND hNotifyWnd) {
    Uninitialize();
    m_hNotifyWnd = hNotifyWnd;

    // --- 1. 创建设备枚举器 ---
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&m_pEnumerator);

    if (FAILED(hr)) return false;

    // --- 2. 获取指定设备 ---
    hr = m_pEnumerator->GetDevice(deviceId.c_str(), &m_pDevice);
    if (FAILED(hr)) return false;

    // --- 3. 激活 IAudioClient ---
    hr = m_pDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&m_pAudioClient);
    if (FAILED(hr)) return false;

    // --- 4. 获取设备 MixFormat ---
    WAVEFORMATEX* pMixFormat = NULL;
    hr = m_pAudioClient->GetMixFormat(&pMixFormat);
    if (FAILED(hr)) return false;

    // 复制 mix format
    memcpy(&m_mixFormat, pMixFormat, sizeof(WAVEFORMATEX));
    CoTaskMemFree(pMixFormat);

    // --- 5. 尝试使用我们的首选格式 (48000Hz, 16-bit, stereo) ---
    WAVEFORMATEX preferredFormat;
    ZeroMemory(&preferredFormat, sizeof(preferredFormat));
    preferredFormat.wFormatTag      = WAVE_FORMAT_PCM;
    preferredFormat.nChannels       = 2;
    preferredFormat.nSamplesPerSec  = 48000;
    preferredFormat.wBitsPerSample  = 16;
    preferredFormat.nBlockAlign     = 4;
    preferredFormat.nAvgBytesPerSec = 192000;
    preferredFormat.cbSize          = 0;

    // 计算约 25ms 的缓冲区
    const REFERENCE_TIME hnsBufferDuration = 250000;  // 25ms in 100ns units
    const REFERENCE_TIME hnsPeriodicity = 0;           // 事件驱动，不需要周期

    // 先尝试首选格式
    hr = m_pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsBufferDuration,
        hnsPeriodicity,
        &preferredFormat,
        NULL);

    if (FAILED(hr)) {
        // 回退到设备 MixFormat
        hr = m_pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            hnsBufferDuration,
            hnsPeriodicity,
            &m_mixFormat,
            NULL);

        if (FAILED(hr)) return false;
    } else {
        // 使用首选格式成功，更新 m_mixFormat
        memcpy(&m_mixFormat, &preferredFormat, sizeof(WAVEFORMATEX));
    }

    m_bytesPerFrame = m_mixFormat.nBlockAlign;

    // --- 6. 获取缓冲区大小 ---
    hr = m_pAudioClient->GetBufferSize(&m_bufferSize);
    if (FAILED(hr)) return false;

    // --- 7. 创建事件句柄 ---
    m_hEvent = CreateEventW(NULL, FALSE, FALSE, NULL);  // auto-reset
    if (m_hEvent == NULL) return false;

    hr = m_pAudioClient->SetEventHandle(m_hEvent);
    if (FAILED(hr)) return false;

    // --- 8. 获取 IAudioRenderClient ---
    hr = m_pAudioClient->GetService(
        __uuidof(IAudioRenderClient), (void**)&m_pRenderClient);
    if (FAILED(hr)) return false;

    return true;
}

// ============================================================================
// Uninitialize - 释放 WASAPI 资源
// ============================================================================

void AudioEngine::Uninitialize() {
    if (m_hEvent) {
        CloseHandle(m_hEvent);
        m_hEvent = NULL;
    }

    SAFE_RELEASE(m_pRenderClient);
    SAFE_RELEASE(m_pAudioClient);
    SAFE_RELEASE(m_pDevice);
    SAFE_RELEASE(m_pEnumerator);

    ZeroMemory(&m_mixFormat, sizeof(m_mixFormat));
}

// ============================================================================
// LoadPcmData - 加载解码后的 PCM 数据
// ============================================================================

void AudioEngine::LoadPcmData(const BYTE* data, DWORD dataSize,
                               const WAVEFORMATEX& format) {
    if (m_isPlaying.load()) {
        Stop();
    }

    m_pcmData.assign(data, data + dataSize);
    m_totalFrames  = dataSize / format.nBlockAlign;
    m_currentFrame = 0;

    // 验证格式兼容性
    if (format.wFormatTag     == m_mixFormat.wFormatTag &&
        format.nChannels      == m_mixFormat.nChannels &&
        format.nSamplesPerSec == m_mixFormat.nSamplesPerSec &&
        format.wBitsPerSample == m_mixFormat.wBitsPerSample) {
        // 格式匹配，直接可用
    }

    AudioState expected = AudioState::Idle;
    m_state.compare_exchange_strong(expected, AudioState::Ready);
    NotifyState(AudioState::Ready);
}

// ============================================================================
// ClearData - 清除音频数据
// ============================================================================

void AudioEngine::ClearData() {
    if (m_isPlaying.load()) {
        Stop();
    }
    m_pcmData.clear();
    m_totalFrames  = 0;
    m_currentFrame = 0;
    m_state.store(AudioState::Idle);
    NotifyState(AudioState::Idle);
}

// ============================================================================
// Play - 开始播放
// ============================================================================

void AudioEngine::Play() {
    if (m_isPlaying.load()) return;
    if (m_pcmData.empty()) return;
    if (!m_pAudioClient || !m_pRenderClient) return;

    // 从头开始播放
    m_currentFrame = 0;
    m_isPlaying.store(true);
    m_state.store(AudioState::Playing);
    NotifyState(AudioState::Playing);

    // 创建播放线程
    m_hThread = CreateThread(NULL, 0, PlaybackThreadProc, this, 0, NULL);
    if (!m_hThread) {
        m_isPlaying.store(false);
        m_state.store(AudioState::Ready);
        NotifyState(AudioState::Ready);
        return;
    }

    // 启动 WASAPI 音频客户端
    HRESULT hr = m_pAudioClient->Start();
    if (FAILED(hr)) {
        m_isPlaying.store(false);
        m_state.store(AudioState::Ready);
        NotifyState(AudioState::Ready);
        return;
    }
}

// ============================================================================
// Stop - 停止播放
// ============================================================================

void AudioEngine::Stop() {
    if (!m_isPlaying.load()) return;

    m_isPlaying.store(false);

    // 停止 WASAPI
    if (m_pAudioClient) {
        m_pAudioClient->Stop();
        m_pAudioClient->Reset();
    }

    // 等待播放线程退出
    if (m_hThread) {
        WaitForSingleObject(m_hThread, 2000);
        CloseHandle(m_hThread);
        m_hThread = NULL;
    }

    m_currentFrame = 0;
    AudioState expected = AudioState::Playing;
    m_state.compare_exchange_strong(expected, AudioState::Ready);
    NotifyState(AudioState::Ready);
}

// ============================================================================
// SetVolume - 设置音量 (0.0 ~ 1.0)
// ============================================================================

void AudioEngine::SetVolume(float level) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    m_volume.store(level);
}

// ============================================================================
// PlaybackThreadProc - 播放线程入口
// ============================================================================

DWORD WINAPI AudioEngine::PlaybackThreadProc(LPVOID param) {
    AudioEngine* engine = static_cast<AudioEngine*>(param);
    engine->PlaybackLoop();
    return 0;
}

// ============================================================================
// PlaybackLoop - 播放循环（在独立线程中运行）
// ============================================================================

void AudioEngine::PlaybackLoop() {
    // 预填充缓冲区以减少启动延迟
    if (m_isPlaying.load()) {
        UINT32 padding = 0;
        m_pAudioClient->GetCurrentPadding(&padding);
        UINT32 framesToWrite = m_bufferSize - padding;

        if (framesToWrite > 0) {
            BYTE* pData = NULL;
            HRESULT hr = m_pRenderClient->GetBuffer(framesToWrite, &pData);
            if (SUCCEEDED(hr)) {
                UINT32 written = FillBuffer(pData, framesToWrite);
                m_pRenderClient->ReleaseBuffer(written, 0);
            }
        }
    }

    // 主循环：等待事件 → 填充数据
    while (m_isPlaying.load()) {
        DWORD waitResult = WaitForSingleObject(m_hEvent, 500);
        if (waitResult == WAIT_TIMEOUT) continue;
        if (waitResult != WAIT_OBJECT_0) break;
        if (!m_isPlaying.load()) break;

        UINT32 padding = 0;
        HRESULT hr = m_pAudioClient->GetCurrentPadding(&padding);
        if (FAILED(hr)) break;

        UINT32 framesAvailable = m_bufferSize - padding;

        if (framesAvailable > 0) {
            BYTE* pData = NULL;
            hr = m_pRenderClient->GetBuffer(framesAvailable, &pData);
            if (FAILED(hr)) break;

            UINT32 written = FillBuffer(pData, framesAvailable);

            DWORD flags = (written < framesAvailable) ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
            m_pRenderClient->ReleaseBuffer(written, flags);
        }
    }

    // 播放结束，停止 WASAPI
    if (m_pAudioClient) {
        m_pAudioClient->Stop();
    }

    m_isPlaying.store(false);
    m_currentFrame = 0;
    AudioState expected = AudioState::Playing;
    m_state.compare_exchange_strong(expected, AudioState::Idle);
    NotifyState(AudioState::Idle);
}

// ============================================================================
// FillBuffer - 填充 WASAPI 缓冲区（应用音量缩放）
// ============================================================================

UINT32 AudioEngine::FillBuffer(BYTE* buffer, UINT32 framesRequested) {
    if (m_pcmData.empty()) {
        memset(buffer, 0, framesRequested * m_bytesPerFrame);
        return 0;
    }

    // 计算可用的剩余帧数
    UINT32 framesRemaining = m_totalFrames - m_currentFrame;
    UINT32 framesToWrite = (framesRequested < framesRemaining)
                           ? framesRequested : framesRemaining;

    float volume = m_volume.load();

    if (m_mixFormat.wBitsPerSample == 16 && m_mixFormat.nChannels == 2) {
        // --- 16-bit 立体声：最常见的格式，快速路径 ---
        INT16* dst = (INT16*)buffer;
        const INT16* src = (const INT16*)m_pcmData.data() + m_currentFrame * 2;

        for (UINT32 i = 0; i < framesToWrite * 2; i++) {
            dst[i] = (INT16)(src[i] * volume);
        }
    }
    else if (m_mixFormat.wBitsPerSample == 16) {
        // --- 16-bit 单声道 ---
        INT16* dst = (INT16*)buffer;
        const INT16* src = (const INT16*)m_pcmData.data() + m_currentFrame * m_mixFormat.nChannels;

        UINT32 channels = m_mixFormat.nChannels;
        for (UINT32 i = 0; i < framesToWrite * channels; i++) {
            dst[i] = (INT16)(src[i] * volume);
        }
    }
    else if (m_mixFormat.wBitsPerSample == 32) {
        // --- 32-bit float ---
        float* dst = (float*)buffer;
        const float* src = (const float*)m_pcmData.data() + m_currentFrame * m_mixFormat.nChannels;

        UINT32 channels = m_mixFormat.nChannels;
        for (UINT32 i = 0; i < framesToWrite * channels; i++) {
            dst[i] = src[i] * volume;
        }
    }
    else {
        // --- 通用路径：逐字节拷贝 ---
        UINT32 copyBytes = framesToWrite * m_bytesPerFrame;
        memcpy(buffer, m_pcmData.data() + m_currentFrame * m_bytesPerFrame, copyBytes);
    }

    // 如果数据不够，剩余部分填充静音
    if (framesToWrite < framesRequested) {
        UINT32 silenceBytes = (framesRequested - framesToWrite) * m_bytesPerFrame;
        memset(buffer + framesToWrite * m_bytesPerFrame, 0, silenceBytes);

        // 播放完成，标记停止
        m_isPlaying.store(false);
    }

    m_currentFrame += framesToWrite;
    return framesToWrite;
}

// ============================================================================
// NotifyState - 发送状态变化通知到 UI 窗口
// ============================================================================

void AudioEngine::NotifyState(AudioState newState) {
    if (m_hNotifyWnd && IsWindow(m_hNotifyWnd)) {
        PostMessageW(m_hNotifyWnd, WM_AUDIO_STATE_CHANGE,
                     (WPARAM)newState, 0);
    }
}
