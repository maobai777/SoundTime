#pragma once

#include <windows.h>
#include <mmreg.h>
#include <msacm.h>
#include <vector>
#include <string>
#include <cstdint>

// ============================================================================
// WavReader - 读取并解码 WAV 音频文件
//
// 功能：
//   1. 解析标准 RIFF/WAVE 文件头，提取音频格式和数据
//   2. 使用 Windows ACM (Audio Compression Manager) 将任意 PCM 格式
//      转换为统一的目标格式：48000Hz, 16-bit, 立体声
//   3. 支持 8/16/24/32-bit PCM，单声道/立体声，常见采样率
//
// 使用方式：
//   WavReader reader;
//   if (reader.Open(L"sound.wav")) {
//       const auto& data = reader.GetPcmData();   // 转换后的 PCM
//       const auto& fmt  = reader.GetFormat();     // WAVEFORMATEX
//   }
// ============================================================================

class WavReader {
public:
    WavReader();
    ~WavReader();

    // 禁止拷贝
    WavReader(const WavReader&) = delete;
    WavReader& operator=(const WavReader&) = delete;

    // 打开 WAV 文件，解析头信息并转换为目标 PCM 格式
    bool Open(const wchar_t* filePath);

    // 关闭文件，释放所有资源
    void Close();

    // 获取转换后的 PCM 音频数据（48000Hz, 16-bit, 立体声）
    const std::vector<BYTE>& GetPcmData() const { return m_convertedData; }

    // 获取转换后的音频格式描述
    const WAVEFORMATEX& GetFormat() const { return m_targetFormat; }

    // 文件是否已成功打开
    bool IsOpen() const { return m_isOpen; }

    // 获取文件路径
    const std::wstring& GetFilePath() const { return m_filePath; }

    // 获取音频总时长（秒）
    double GetDurationSec() const;

private:
    // 解析 RIFF/WAVE 文件结构
    bool ParseRiffChunks(const std::vector<BYTE>& fileData);

    // 使用 ACM 将源格式转换为目标格式
    bool ConvertWithAcm(const WAVEFORMATEX& srcFormat,
                        const BYTE* srcData, DWORD srcDataSize);

    // 构建目标格式描述 (48000Hz, 16-bit, stereo)
    static void BuildTargetFormat(WAVEFORMATEX& fmt);

    std::wstring      m_filePath;
    std::vector<BYTE> m_convertedData;
    WAVEFORMATEX      m_targetFormat;
    bool              m_isOpen;
};
