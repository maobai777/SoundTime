#include "mp3_decoder.h"

// Media Foundation 头文件（顺序很重要，先包含基础接口）
#include <windows.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <cstring>

// ============================================================================
// 链接 Media Foundation 库（MinGW 没有 mfreadwrite 导入库，动态加载）
// ============================================================================
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")

// ============================================================================
// MFCreateSourceReaderFromURL 的函数签名
// 该函数在 mfreadwrite.dll 中导出，MinGW 缺少对应的 .a 文件，因此动态获取
// ============================================================================
typedef HRESULT (WINAPI *PFN_MFCreateSourceReaderFromURL)(
    LPCWSTR pwszURL,
    IMFAttributes *pAttributes,
    IMFSourceReader **ppSourceReader
);

// ============================================================================
// 构造与析构
// ============================================================================

MpegDecoder::MpegDecoder() : m_isOpen(false) {
    ZeroMemory(&m_format, sizeof(m_format));
    // 始终输出 48kHz 16-bit 立体声 PCM
    m_format.wFormatTag      = WAVE_FORMAT_PCM;
    m_format.nChannels       = 2;
    m_format.nSamplesPerSec  = 48000;
    m_format.wBitsPerSample  = 16;
    m_format.nBlockAlign     = 4;     // channels * bits/8
    m_format.nAvgBytesPerSec = 192000; // samplerate * blockalign
    m_format.cbSize          = 0;
}

MpegDecoder::~MpegDecoder() {
    Close();
}

// ============================================================================
// Open - 打开并解码 MP3 文件
// ============================================================================

bool MpegDecoder::Open(const wchar_t* filePath) {
    Close();
    m_filePath = filePath;

    HRESULT hr;

    // --- 1. 动态加载 MFCreateSourceReaderFromURL ---
    // 在 Windows 10/11 上 mfreadwrite.dll 始终存在
    HMODULE hMfReadWrite = LoadLibraryW(L"mfreadwrite.dll");
    if (!hMfReadWrite) return false;

    PFN_MFCreateSourceReaderFromURL pfnCreateSourceReader =
        (PFN_MFCreateSourceReaderFromURL)GetProcAddress(
            hMfReadWrite, "MFCreateSourceReaderFromURL");

    if (!pfnCreateSourceReader) {
        FreeLibrary(hMfReadWrite);
        return false;
    }

    // --- 2. 初始化 Media Foundation（可多次调用，引用计数） ---
    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        FreeLibrary(hMfReadWrite);
        return false;
    }

    // --- 3. 创建 Source Reader ---
    IMFSourceReader* pReader = NULL;
    hr = pfnCreateSourceReader(filePath, NULL, &pReader);
    if (FAILED(hr)) {
        FreeLibrary(hMfReadWrite);
        MFShutdown();
        return false;
    }

    // --- 4. 配置输出格式：48000Hz, 16-bit, 立体声 PCM ---
    IMFMediaType* pPcmType = NULL;
    hr = MFCreateMediaType(&pPcmType);
    if (SUCCEEDED(hr)) {
        pPcmType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pPcmType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pPcmType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        pPcmType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        pPcmType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pPcmType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        pPcmType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000);
        pPcmType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

        // 设置 PCM 输出格式（MF 会自动重采样/转声道/转位深）
        hr = pReader->SetCurrentMediaType(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            NULL,
            pPcmType);

        pPcmType->Release();
    }

    if (FAILED(hr)) {
        // 无法设置目标格式，回退失败
        pReader->Release();
        FreeLibrary(hMfReadWrite);
        MFShutdown();
        return false;
    }

    // --- 5. 读取所有采样数据 ---
    bool success = ReadAllSamples(pReader);

    // --- 6. 清理 ---
    pReader->Release();
    FreeLibrary(hMfReadWrite);
    MFShutdown();

    if (!success || m_pcmData.empty()) {
        m_pcmData.clear();
        return false;
    }

    m_isOpen = true;
    return true;
}

// ============================================================================
// Close - 释放资源
// ============================================================================

void MpegDecoder::Close() {
    m_filePath.clear();
    m_pcmData.clear();
    m_isOpen = false;
}

// ============================================================================
// ReadAllSamples - 从 Source Reader 读取所有 PCM 数据
// ============================================================================

bool MpegDecoder::ReadAllSamples(void* pReaderUnsafe) {
    IMFSourceReader* pReader = static_cast<IMFSourceReader*>(pReaderUnsafe);
    HRESULT hr;

    while (true) {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* pSample = NULL;

        hr = pReader->ReadSample(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            0,
            &streamIndex,
            &flags,
            &timestamp,
            &pSample);

        if (FAILED(hr)) break;

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (pSample) pSample->Release();
            break;
        }

        if (flags & MF_SOURCE_READERF_ERROR) {
            if (pSample) pSample->Release();
            continue;  // 跳过错误的采样，继续读取
        }

        if (pSample == NULL) continue;

        // 提取采样中的缓冲区数据
        IMFMediaBuffer* pBuffer = NULL;
        hr = pSample->ConvertToContiguousBuffer(&pBuffer);
        if (SUCCEEDED(hr)) {
            AppendBufferData(pBuffer);
            pBuffer->Release();
        }

        pSample->Release();
    }

    return !m_pcmData.empty();
}

// ============================================================================
// AppendBufferData - 将 Media Buffer 数据追加到 m_pcmData
// ============================================================================

bool MpegDecoder::AppendBufferData(void* pBufferUnsafe) {
    IMFMediaBuffer* pBuffer = static_cast<IMFMediaBuffer*>(pBufferUnsafe);

    BYTE* pData = NULL;
    DWORD dataSize = 0;

    HRESULT hr = pBuffer->Lock(&pData, NULL, &dataSize);
    if (FAILED(hr)) return false;

    // 追加到输出缓冲区
    size_t oldSize = m_pcmData.size();
    m_pcmData.resize(oldSize + dataSize);
    memcpy(m_pcmData.data() + oldSize, pData, dataSize);

    pBuffer->Unlock();
    return true;
}

// ============================================================================
// GetDurationSec - 获取音频时长（秒）
// ============================================================================

double MpegDecoder::GetDurationSec() const {
    if (m_pcmData.empty()) return 0.0;
    DWORD totalFrames = (DWORD)m_pcmData.size() / m_format.nBlockAlign;
    return (double)totalFrames / m_format.nSamplesPerSec;
}
