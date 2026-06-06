#pragma once

#include <windows.h>
#include <mmreg.h>
#include <vector>
#include <string>
#include <cstdint>

// ============================================================================
// MpegDecoder - MP3 音频解码器（基于 Windows Media Foundation）
//
// 功能：
//   1. 使用 Media Foundation Source Reader 解码 MP3 文件
//   2. 直接输出 48000Hz, 16-bit, 立体声 PCM（MF 自动重采样）
//   3. 接口与 WavReader 保持一致，调用方式相同
//
// 依赖：
//   - mf.lib / mfplat.lib (Media Foundation)
//   - Windows 10/11 自带，无需额外安装
//
// 使用方式：
//   MpegDecoder decoder;
//   if (decoder.Open(L"music.mp3")) {
//       const auto& data = decoder.GetPcmData();
//       const auto& fmt  = decoder.GetFormat();
//   }
// ============================================================================

class MpegDecoder {
public:
    MpegDecoder();
    ~MpegDecoder();

    // 禁止拷贝
    MpegDecoder(const MpegDecoder&) = delete;
    MpegDecoder& operator=(const MpegDecoder&) = delete;

    // 打开 MP3 文件，解码为 PCM
    bool Open(const wchar_t* filePath);

    // 关闭文件，释放资源
    void Close();

    // 获取解码后的 PCM 数据（48000Hz, 16-bit, 立体声）
    const std::vector<BYTE>& GetPcmData() const { return m_pcmData; }

    // 获取音频格式（始终为 48000Hz 16-bit 立体声 PCM）
    const WAVEFORMATEX& GetFormat() const { return m_format; }

    // 文件是否已成功打开并解码
    bool IsOpen() const { return m_isOpen; }

    // 获取文件路径
    const std::wstring& GetFilePath() const { return m_filePath; }

    // 获取音频总时长（秒）
    double GetDurationSec() const;

private:
    // 从 Source Reader 读取所有 PCM 数据
    bool ReadAllSamples(void* pSourceReader);

    // 提取 IMFMediaBuffer 中的数据追加到 m_pcmData
    bool AppendBufferData(void* pMediaBuffer);

    std::wstring      m_filePath;
    std::vector<BYTE> m_pcmData;
    WAVEFORMATEX      m_format;
    bool              m_isOpen;
};
