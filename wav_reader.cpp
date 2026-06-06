#include "wav_reader.h"
#include <stdio.h>
#include <cstring>

// ============================================================================
// 构造与析构
// ============================================================================

WavReader::WavReader() : m_isOpen(false) {
    ZeroMemory(&m_targetFormat, sizeof(m_targetFormat));
    BuildTargetFormat(m_targetFormat);
}

WavReader::~WavReader() {
    Close();
}

// ============================================================================
// 构建目标格式：48000Hz, 16-bit, 立体声 PCM
// ============================================================================

void WavReader::BuildTargetFormat(WAVEFORMATEX& fmt) {
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = 2;
    fmt.nSamplesPerSec  = 48000;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = fmt.nChannels * (fmt.wBitsPerSample / 8);   // 4
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;       // 192000
    fmt.cbSize          = 0;
}

// ============================================================================
// Open - 打开并解码 WAV 文件
// ============================================================================

bool WavReader::Open(const wchar_t* filePath) {
    Close();
    m_filePath = filePath;

    // --- 1. 读取整个文件到内存 ---
    HANDLE hFile = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize < 44) {  // WAV 文件头至少 44 字节
        CloseHandle(hFile);
        return false;
    }

    std::vector<BYTE> fileData(fileSize);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, fileData.data(), fileSize, &bytesRead, NULL) ||
        bytesRead != fileSize) {
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);

    // --- 2. 验证 RIFF 头 ---
    if (fileSize < 12 ||
        memcmp(fileData.data(), "RIFF", 4) != 0 ||
        memcmp(fileData.data() + 8, "WAVE", 4) != 0) {
        return false;
    }

    // --- 3. 解析 chunks ---
    if (!ParseRiffChunks(fileData)) {
        return false;
    }

    m_isOpen = true;
    return true;
}

// ============================================================================
// Close - 释放资源
// ============================================================================

void WavReader::Close() {
    m_filePath.clear();
    m_convertedData.clear();
    m_isOpen = false;
}

// ============================================================================
// ParseRiffChunks - 遍历 RIFF chunks 找到 fmt 和 data
// ============================================================================

bool WavReader::ParseRiffChunks(const std::vector<BYTE>& fileData) {
    DWORD offset = 12;  // 跳过 "RIFF" + size + "WAVE"
    DWORD fileSize = (DWORD)fileData.size();

    WAVEFORMATEX srcFormat = {};
    ZeroMemory(&srcFormat, sizeof(srcFormat));

    const BYTE* srcPcmData = nullptr;
    DWORD srcPcmDataSize = 0;

    bool foundFmt  = false;
    bool foundData = false;

    while (offset + 8 <= fileSize) {
        DWORD chunkId   = *(DWORD*)(fileData.data() + offset);
        DWORD chunkSize = *(DWORD*)(fileData.data() + offset + 4);

        offset += 8;

        if (offset + chunkSize > fileSize) {
            break;  // 数据损坏
        }

        if (chunkId == 0x20746D66) {  // "fmt " (little-endian)
            // 读取 WAVEFORMAT 结构
            if (chunkSize >= 16) {
                srcFormat.wFormatTag      = *(WORD*)(fileData.data() + offset);
                srcFormat.nChannels       = *(WORD*)(fileData.data() + offset + 2);
                srcFormat.nSamplesPerSec  = *(DWORD*)(fileData.data() + offset + 4);
                srcFormat.nAvgBytesPerSec = *(DWORD*)(fileData.data() + offset + 8);
                srcFormat.nBlockAlign     = *(WORD*)(fileData.data() + offset + 12);
                srcFormat.wBitsPerSample  = *(WORD*)(fileData.data() + offset + 14);
                srcFormat.cbSize          = 0;

                // 处理扩展的 WAVEFORMATEX (PCM 格式不会有扩展)
                if (chunkSize >= 18) {
                    WORD extraSize = *(WORD*)(fileData.data() + offset + 16);
                    if (extraSize > 0 && chunkSize >= (DWORD)(18 + extraSize)) {
                        srcFormat.cbSize = extraSize;
                    }
                }

                foundFmt = true;
            }
        }
        else if (chunkId == 0x61746164) {  // "data" (little-endian)
            srcPcmData     = fileData.data() + offset;
            srcPcmDataSize = chunkSize;
            foundData = true;
        }

        if (foundFmt && foundData) break;

        // 移动到下一个 chunk（按 WORD 对齐）
        offset += chunkSize;
        if (chunkSize % 2) offset++;
    }

    if (!foundFmt || !foundData || srcPcmDataSize == 0) {
        return false;
    }

    // 只支持 PCM 格式
    if (srcFormat.wFormatTag != WAVE_FORMAT_PCM) {
        return false;  // 非 PCM 格式暂不支持（如 ADPCM, MP3-in-WAV 等）
    }

    // --- 4. 使用 ACM 转换为目标格式 ---
    if (!ConvertWithAcm(srcFormat, srcPcmData, srcPcmDataSize)) {
        return false;
    }

    return true;
}

// ============================================================================
// ConvertWithAcm - 使用 ACM 将源格式转换为目标格式
// ============================================================================

bool WavReader::ConvertWithAcm(const WAVEFORMATEX& srcFormat,
                                const BYTE* srcData, DWORD srcDataSize) {
    WAVEFORMATEX targetFormat;
    BuildTargetFormat(targetFormat);

    // 如果源格式和目标格式完全一致，直接复制
    if (srcFormat.wFormatTag      == targetFormat.wFormatTag &&
        srcFormat.nChannels       == targetFormat.nChannels &&
        srcFormat.nSamplesPerSec  == targetFormat.nSamplesPerSec &&
        srcFormat.wBitsPerSample  == targetFormat.wBitsPerSample) {
        m_convertedData.assign(srcData, srcData + srcDataSize);
        return true;
    }

    // --- 打开 ACM 转换流 ---
    HACMSTREAM hAcmStream = NULL;
    MMRESULT mmr = acmStreamOpen(&hAcmStream, NULL,
                                  (LPWAVEFORMATEX)&srcFormat,
                                  &targetFormat,
                                  NULL, 0, 0,
                                  ACM_STREAMOPENF_NONREALTIME);
    if (mmr != MMSYSERR_NOERROR) {
        // ACM 转换失败 — 尝试直接使用原始数据
        // WASAPI 共享模式通常能处理常见的 PCM 格式
        return false;
    }

    // --- 计算输出缓冲区大小 ---
    DWORD outputBufferSize = 0;
    mmr = acmStreamSize(hAcmStream, srcDataSize, &outputBufferSize,
                         ACM_STREAMSIZEF_SOURCE);
    if (mmr != MMSYSERR_NOERROR || outputBufferSize == 0) {
        // 如果无法计算，按最大可能估算
        DWORD srcSamples = srcDataSize / srcFormat.nBlockAlign;
        outputBufferSize = srcSamples * targetFormat.nBlockAlign * 2;
    }

    m_convertedData.resize(outputBufferSize);

    // --- 准备 ACM 流头 ---
    ACMSTREAMHEADER acmHeader;
    ZeroMemory(&acmHeader, sizeof(acmHeader));
    acmHeader.cbStruct    = sizeof(ACMSTREAMHEADER);
    acmHeader.pbSrc       = (LPBYTE)srcData;
    acmHeader.cbSrcLength = srcDataSize;
    acmHeader.pbDst       = m_convertedData.data();
    acmHeader.cbDstLength = outputBufferSize;

    mmr = acmStreamPrepareHeader(hAcmStream, &acmHeader, 0);
    if (mmr != MMSYSERR_NOERROR) {
        acmStreamClose(hAcmStream, 0);
        return false;
    }

    // --- 执行转换 ---
    mmr = acmStreamConvert(hAcmStream, &acmHeader, 0);
    if (mmr != MMSYSERR_NOERROR) {
        acmStreamUnprepareHeader(hAcmStream, &acmHeader, 0);
        acmStreamClose(hAcmStream, 0);
        return false;
    }

    // --- 裁剪到实际转换大小 ---
    m_convertedData.resize(acmHeader.cbDstLengthUsed);

    // --- 清理 ---
    acmStreamUnprepareHeader(hAcmStream, &acmHeader, 0);
    acmStreamClose(hAcmStream, 0);

    return true;
}

// ============================================================================
// GetDurationSec - 获取音频时长（秒）
// ============================================================================

double WavReader::GetDurationSec() const {
    if (m_convertedData.empty()) return 0.0;
    DWORD totalFrames = (DWORD)m_convertedData.size() / m_targetFormat.nBlockAlign;
    return (double)totalFrames / m_targetFormat.nSamplesPerSec;
}
