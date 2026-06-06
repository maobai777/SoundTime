// ============================================================================
// SoundTime - 本地音频虚拟麦克风播放器
//
// 功能：将本地 WAV/MP3 音频文件通过 WASAPI 播放到选定音频输出设备。
//       配合 VB-Cable 等虚拟音频电缆，可实现"音频注入麦克风"效果。
//
// 技术栈：C++17 + Win32 API + WASAPI + ACM
// 最低系统要求：Windows 10/11 64位
// ============================================================================

// 启用 Windows 10+ API
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <string>
#include <vector>

#include "audio_engine.h"
#include "wav_reader.h"
#include "mp3_decoder.h"

// ============================================================================
// ComCtl 视觉样式 (Windows XP+ 扁平化控件)
// ============================================================================
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#pragma comment(lib, "comctl32.lib")

// ============================================================================
// 控件 ID
// ============================================================================
#define IDC_FILE_PATH      1001
#define IDC_BTN_OPEN       1002
#define IDC_BTN_PLAY       1003
#define IDC_BTN_STOP       1004
#define IDC_CB_DEVICE      1005
#define IDC_SLIDER_VOLUME  1006
#define IDC_LINK_CABLE     1007
#define IDC_LINK_SOUND     1008
#define IDC_STATIC_STATUS  1009
#define IDC_LABEL_FILE     1010
#define IDC_LABEL_DEVICE   1011
#define IDC_LABEL_VOLUME   1012

// ============================================================================
// 全局变量
// ============================================================================
static AudioEngine  g_audioEngine;
static WavReader    g_wavReader;
static MpegDecoder  g_mp3Decoder;
static HFONT        g_hFont = NULL;
static HFONT        g_hLinkFont = NULL;
static HBRUSH       g_hBgBrush = NULL;

// 当前加载的音频类型
enum class AudioFileType { Unknown, Wav, Mp3 };
static AudioFileType g_fileType = AudioFileType::Unknown;

// 缓存的设备列表（用于 combo box 查找设备 ID）
static std::vector<DeviceInfo> g_devices;

// ============================================================================
// 辅助函数
// ============================================================================

// 获取 combo box 当前选中项的关联数据（设备 ID）
static std::wstring GetSelectedDeviceId(HWND hCombo) {
    int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_devices.size()) return L"";
    return g_devices[sel].id;
}

// 更新状态栏文本
static void SetStatus(HWND hWnd, const wchar_t* text) {
    SetDlgItemTextW(hWnd, IDC_STATIC_STATUS, text);
}

// 更新按钮状态
static void UpdateButtonStates(HWND hWnd, AudioState state) {
    BOOL hasData = g_audioEngine.HasData();
    BOOL isPlaying = (state == AudioState::Playing);

    EnableWindow(GetDlgItem(hWnd, IDC_BTN_PLAY),  hasData && !isPlaying);
    EnableWindow(GetDlgItem(hWnd, IDC_BTN_STOP),  isPlaying);
}

// 初始化播放引擎（选择默认设备）
static bool InitAudioEngine(HWND hWnd) {
    // 获取设备列表
    g_devices = AudioEngine::EnumerateRenderDevices();

    HWND hCombo = GetDlgItem(hWnd, IDC_CB_DEVICE);
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);

    if (g_devices.empty()) {
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"未找到音频设备");
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
        EnableWindow(hCombo, FALSE);
        return false;
    }

    // 填充设备列表
    for (size_t i = 0; i < g_devices.size(); i++) {
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)g_devices[i].name.c_str());
    }

    // 默认选择第一个设备
    SendMessageW(hCombo, CB_SETCURSEL, 0, 0);

    // 用第一个设备初始化引擎
    bool ok = g_audioEngine.Initialize(g_devices[0].id, hWnd);

    // 检测是否安装了虚拟音频电缆
    bool hasCable = false;
    for (const auto& dev : g_devices) {
        std::wstring nameLower = dev.name;
        for (auto& c : nameLower) c = towlower(c);
        if (nameLower.find(L"cable") != std::wstring::npos ||
            nameLower.find(L"vb-audio") != std::wstring::npos ||
            nameLower.find(L"virtual") != std::wstring::npos ||
            nameLower.find(L"虚拟") != std::wstring::npos) {
            hasCable = true;
            break;
        }
    }

    if (!hasCable) {
        // 首次运行提示：引导安装虚拟电缆
        MessageBoxW(hWnd,
            L"提示：\n\n"
            L"要让对方在通话中听到你播放的音频，需要安装\n"
            L"免费的虚拟音频电缆 (VB-Cable)。\n\n"
            L"安装后：\n"
            L"1. 在本软件中选择 \"CABLE Input\" 作为输出设备\n"
            L"2. 在通话软件中将 \"CABLE Output\" 设为麦克风\n\n"
            L"点击下方「安装虚拟电缆」链接即可下载。\n"
            L"未安装时音频只会播放到扬声器/耳机。",
            L"SoundTime - 使用提示",
            MB_OK | MB_ICONINFORMATION);
    }

    return ok;
}

// 根据文件扩展名判断音频类型
static AudioFileType DetectFileType(const wchar_t* filePath) {
    const wchar_t* ext = wcsrchr(filePath, L'.');
    if (!ext) return AudioFileType::Unknown;

    if (_wcsicmp(ext, L".wav") == 0) return AudioFileType::Wav;
    if (_wcsicmp(ext, L".mp3") == 0) return AudioFileType::Mp3;

    return AudioFileType::Unknown;
}

// 获取当前活跃的解码器
// 返回 true 表示有可用的解码器，通过引用参数返回数据
static bool GetActiveDecoderData(const BYTE*& outData, DWORD& outSize,
                                  const WAVEFORMATEX*& outFormat) {
    if (g_wavReader.IsOpen()) {
        outData   = g_wavReader.GetPcmData().data();
        outSize   = (DWORD)g_wavReader.GetPcmData().size();
        outFormat = &g_wavReader.GetFormat();
        return true;
    }
    if (g_mp3Decoder.IsOpen()) {
        outData   = g_mp3Decoder.GetPcmData().data();
        outSize   = (DWORD)g_mp3Decoder.GetPcmData().size();
        outFormat = &g_mp3Decoder.GetFormat();
        return true;
    }
    return false;
}

// 获取当前活跃解码器的时长
static double GetActiveDecoderDuration() {
    if (g_wavReader.IsOpen())   return g_wavReader.GetDurationSec();
    if (g_mp3Decoder.IsOpen())  return g_mp3Decoder.GetDurationSec();
    return 0.0;
}

// 关闭所有解码器
static void CloseAllDecoders() {
    g_wavReader.Close();
    g_mp3Decoder.Close();
    g_fileType = AudioFileType::Unknown;
}

// 打开并加载音频文件（自动识别 WAV / MP3）
static bool LoadAudioFile(HWND hWnd, const wchar_t* filePath) {
    // 先停止当前播放
    g_audioEngine.Stop();

    // 根据扩展名选择解码器
    AudioFileType type = DetectFileType(filePath);
    bool loaded = false;

    switch (type) {
    case AudioFileType::Wav:
        loaded = g_wavReader.Open(filePath);
        if (loaded) g_mp3Decoder.Close();
        break;

    case AudioFileType::Mp3:
        loaded = g_mp3Decoder.Open(filePath);
        if (loaded) g_wavReader.Close();
        break;

    default:
        SetStatus(hWnd, L"错误：不支持的文件格式（仅支持 WAV / MP3）");
        return false;
    }

    if (!loaded) {
        SetStatus(hWnd, L"错误：无法打开或解码该音频文件");
        CloseAllDecoders();
        return false;
    }

    g_fileType = type;

    // 将解码后的 PCM 数据加载到音频引擎
    const BYTE* pcmData = nullptr;
    DWORD dataSize = 0;
    const WAVEFORMATEX* format = nullptr;

    if (GetActiveDecoderData(pcmData, dataSize, format)) {
        g_audioEngine.LoadPcmData(pcmData, dataSize, *format);
    }

    // 更新 UI
    SetDlgItemTextW(hWnd, IDC_FILE_PATH, filePath);

    const wchar_t* typeName = (type == AudioFileType::Mp3) ? L"MP3" : L"WAV";
    wchar_t status[256];
    swprintf_s(status, L"就绪 [%s] - %s (%.1f秒)",
               typeName,
               wcsrchr(filePath, L'\\') ? wcsrchr(filePath, L'\\') + 1 : filePath,
               GetActiveDecoderDuration());
    SetStatus(hWnd, status);

    UpdateButtonStates(hWnd, AudioState::Ready);
    return true;
}

// ============================================================================
// 窗口过程
// ============================================================================

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    // --- 窗口创建 ---
    case WM_CREATE: {
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE);

        // 创建 UI 字体 (Segoe UI 9pt — Windows 默认现代字体)
        g_hFont = CreateFontW(
            -MulDiv(9, GetDeviceCaps(GetDC(hWnd), LOGPIXELSY), 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

        // 创建背景画刷 (浅灰色，用于状态栏区域)
        g_hBgBrush = CreateSolidBrush(RGB(240, 240, 240));

        // --- 第 1 行：文件路径 ---
        CreateWindowW(L"STATIC", L"选择音频文件:",
                      WS_CHILD | WS_VISIBLE,
                      10, 8, 100, 18,
                      hWnd, (HMENU)IDC_LABEL_FILE, hInst, NULL);

        CreateWindowW(L"EDIT", L"",
                      WS_CHILD | WS_VISIBLE | WS_BORDER |
                      ES_READONLY | ES_AUTOHSCROLL,
                      10, 28, 235, 22,
                      hWnd, (HMENU)IDC_FILE_PATH, hInst, NULL);

        CreateWindowW(L"BUTTON", L"打开文件",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      252, 27, 78, 24,
                      hWnd, (HMENU)IDC_BTN_OPEN, hInst, NULL);

        // --- 第 2 行：播放/停止按钮 ---
        CreateWindowW(L"BUTTON", L"▶ 播放",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      10, 60, 80, 26,
                      hWnd, (HMENU)IDC_BTN_PLAY, hInst, NULL);

        CreateWindowW(L"BUTTON", L"■ 停止",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      98, 60, 80, 26,
                      hWnd, (HMENU)IDC_BTN_STOP, hInst, NULL);

        // --- 第 3 行：输出设备选择 ---
        CreateWindowW(L"STATIC", L"输出设备:",
                      WS_CHILD | WS_VISIBLE,
                      10, 96, 65, 18,
                      hWnd, (HMENU)IDC_LABEL_DEVICE, hInst, NULL);

        CreateWindowW(L"COMBOBOX", L"",
                      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST |
                      CBS_HASSTRINGS | WS_VSCROLL,
                      80, 94, 250, 200,
                      hWnd, (HMENU)IDC_CB_DEVICE, hInst, NULL);

        // --- 第 4 行：音量滑块 ---
        CreateWindowW(L"STATIC", L"音量:",
                      WS_CHILD | WS_VISIBLE,
                      10, 128, 65, 18,
                      hWnd, (HMENU)IDC_LABEL_VOLUME, hInst, NULL);

        CreateWindowW(TRACKBAR_CLASSW, L"",
                      WS_CHILD | WS_VISIBLE | TBS_HORZ |
                      TBS_BOTTOM | TBS_NOTICKS,
                      60, 126, 200, 28,
                      hWnd, (HMENU)IDC_SLIDER_VOLUME, hInst, NULL);

        // 设置滑块范围 0-100，默认 80
        SendDlgItemMessageW(hWnd, IDC_SLIDER_VOLUME, TBM_SETRANGE,
                            (WPARAM)TRUE, (LPARAM)MAKELONG(0, 100));
        SendDlgItemMessageW(hWnd, IDC_SLIDER_VOLUME, TBM_SETPOS,
                            (WPARAM)TRUE, 80);
        g_audioEngine.SetVolume(0.8f);

        // --- 第 5 行：链接（用按钮模拟，比 SysLink 更可靠） ---
        CreateWindowW(L"BUTTON", L"🔗 安装虚拟电缆 (推荐)",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
                      10, 156, 180, 22,
                      hWnd, (HMENU)IDC_LINK_CABLE, hInst, NULL);

        CreateWindowW(L"BUTTON", L"🔗 打开声音设置",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
                      195, 156, 145, 22,
                      hWnd, (HMENU)IDC_LINK_SOUND, hInst, NULL);

        // 为链接按钮创建带下划线的字体
        LOGFONTW lf = {};
        GetObjectW(g_hFont, sizeof(lf), &lf);
        lf.lfUnderline = TRUE;
        g_hLinkFont = CreateFontIndirectW(&lf);

        // --- 状态文本 ---
        CreateWindowW(L"STATIC", L"就绪 - 请选择 WAV/MP3 音频文件",
                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                      10, 180, 320, 18,
                      hWnd, (HMENU)IDC_STATIC_STATUS, hInst, NULL);

        // 应用字体到所有子控件
        EnumChildWindows(hWnd, [](HWND hChild, LPARAM lParam) -> BOOL {
            SendMessageW(hChild, WM_SETFONT, (WPARAM)lParam, TRUE);
            return TRUE;
        }, (LPARAM)g_hFont);

        // 链接按钮使用带下划线的字体
        SendMessageW(GetDlgItem(hWnd, IDC_LINK_CABLE), WM_SETFONT,
                     (WPARAM)g_hLinkFont, TRUE);
        SendMessageW(GetDlgItem(hWnd, IDC_LINK_SOUND), WM_SETFONT,
                     (WPARAM)g_hLinkFont, TRUE);

        // 初始化音频引擎
        if (!InitAudioEngine(hWnd)) {
            SetStatus(hWnd, L"警告：未找到可用的音频输出设备");
        }

        // 初始按钮状态
        EnableWindow(GetDlgItem(hWnd, IDC_BTN_PLAY), FALSE);
        EnableWindow(GetDlgItem(hWnd, IDC_BTN_STOP), FALSE);

        return 0;
    }

    // --- WM_CTLCOLORSTATIC: 设置静态控件背景色 ---
    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        HWND hCtrl = (HWND)lParam;

        // 状态栏和链接控件用浅灰背景
        int ctrlId = GetDlgCtrlID(hCtrl);
        if (ctrlId == IDC_STATIC_STATUS || ctrlId >= IDC_LABEL_FILE) {
            SetBkMode(hdcStatic, TRANSPARENT);
            SetTextColor(hdcStatic, RGB(50, 50, 50));
            return (LRESULT)g_hBgBrush;
        }
        break;
    }

    // --- WM_CTLCOLORBTN: 链接按钮设蓝色文字 ---
    case WM_CTLCOLORBTN: {
        int ctrlId = GetDlgCtrlID((HWND)lParam);
        if (ctrlId == IDC_LINK_CABLE || ctrlId == IDC_LINK_SOUND) {
            SetTextColor((HDC)wParam, RGB(0, 102, 204));  // 蓝色
            SetBkMode((HDC)wParam, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        break;
    }

    // --- WM_COMMAND: 按钮点击、combobox 选择 ---
    case WM_COMMAND: {
        WORD wmId = LOWORD(wParam);
        WORD wmEvent = HIWORD(wParam);

        switch (wmId) {

        // "打开文件" 按钮
        case IDC_BTN_OPEN: {
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hWnd;
            ofn.lpstrFilter = L"音频文件 (WAV/MP3)\0*.wav;*.mp3\0WAV 文件\0*.wav\0MP3 文件\0*.mp3\0所有文件\0*.*\0";
            ofn.lpstrFile = new wchar_t[MAX_PATH];
            ofn.lpstrFile[0] = L'\0';
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
            ofn.lpstrDefExt = L"wav";

            if (GetOpenFileNameW(&ofn)) {
                LoadAudioFile(hWnd, ofn.lpstrFile);
            }

            delete[] ofn.lpstrFile;
            return 0;
        }

        // "播放" 按钮
        case IDC_BTN_PLAY: {
            if (g_audioEngine.GetState() != AudioState::Playing &&
                g_audioEngine.HasData()) {
                g_audioEngine.Play();

                // 根据目标设备类型显示不同提示
                std::wstring devId = GetSelectedDeviceId(
                    GetDlgItem(hWnd, IDC_CB_DEVICE));
                bool isCable = (devId.find(L"cable") != std::wstring::npos ||
                                devId.find(L"CABLE") != std::wstring::npos ||
                                devId.find(L"virtual") != std::wstring::npos ||
                                devId.find(L"Virtual") != std::wstring::npos);

                if (isCable) {
                    SetStatus(hWnd, L"播放中 → 虚拟电缆（对方可听到）");
                } else {
                    SetStatus(hWnd, L"播放中 → 扬声器（⚠ 通话对方听不到）");
                }

                UpdateButtonStates(hWnd, AudioState::Playing);
            }
            return 0;
        }

        // "停止" 按钮
        case IDC_BTN_STOP: {
            g_audioEngine.Stop();
            SetStatus(hWnd, L"已停止");
            UpdateButtonStates(hWnd, AudioState::Idle);
            return 0;
        }

        // 设备下拉框选择变化
        case IDC_CB_DEVICE: {
            if (wmEvent == CBN_SELCHANGE) {
                std::wstring deviceId = GetSelectedDeviceId(
                    GetDlgItem(hWnd, IDC_CB_DEVICE));
                if (!deviceId.empty()) {
                    // 保存当前音频数据（如果有的话）
                    bool hadData = g_audioEngine.HasData();
                    g_audioEngine.Stop();

                    // 切换到新设备
                    if (g_audioEngine.Initialize(deviceId, hWnd)) {
                        SetStatus(hWnd, L"已切换输出设备");

                        // 如果有音频数据，重新加载
                        if (hadData) {
                            const BYTE* data = nullptr;
                            DWORD dataSize = 0;
                            const WAVEFORMATEX* fmt = nullptr;
                            if (GetActiveDecoderData(data, dataSize, fmt)) {
                                g_audioEngine.LoadPcmData(data, dataSize, *fmt);
                                UpdateButtonStates(hWnd, AudioState::Ready);
                            }
                        }
                    } else {
                        SetStatus(hWnd, L"错误：无法切换到所选设备");
                    }
                }
            }
            return 0;
        }

        // "安装虚拟电缆" 链接按钮
        case IDC_LINK_CABLE: {
            ShellExecuteW(NULL, L"open",
                L"https://vb-audio.com/Cable/", NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }

        // "打开声音设置" 链接按钮
        case IDC_LINK_SOUND: {
            ShellExecuteW(NULL, L"open",
                L"ms-settings:sound", NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }

        } // switch wmId
        break;
    }

    // --- WM_HSCROLL: 音量滑块 ---
    case WM_HSCROLL: {
        HWND hSlider = (HWND)lParam;
        if (hSlider == GetDlgItem(hWnd, IDC_SLIDER_VOLUME)) {
            int pos = (int)SendMessageW(hSlider, TBM_GETPOS, 0, 0);
            g_audioEngine.SetVolume(pos / 100.0f);
        }
        return 0;
    }


    // --- 自定义消息：音频状态变化 ---
    case WM_AUDIO_STATE_CHANGE: {
        AudioState newState = (AudioState)wParam;
        UpdateButtonStates(hWnd, newState);

        switch (newState) {
        case AudioState::Idle:
            SetStatus(hWnd, L"播放完毕");
            break;
        case AudioState::Ready:
            SetStatus(hWnd, L"就绪");
            break;
        case AudioState::Playing: {
            // 根据设备类型显示不同提示
            std::wstring devId = GetSelectedDeviceId(
                GetDlgItem(hWnd, IDC_CB_DEVICE));
            bool isCable = (devId.find(L"cable") != std::wstring::npos ||
                            devId.find(L"CABLE") != std::wstring::npos ||
                            devId.find(L"virtual") != std::wstring::npos ||
                            devId.find(L"Virtual") != std::wstring::npos);
            if (isCable)
                SetStatus(hWnd, L"播放中 → 虚拟电缆（对方可听到）");
            else
                SetStatus(hWnd, L"播放中 → 扬声器（⚠ 通话对方听不到）");
            break;
        }
        }
        return 0;
    }

    // --- WM_SETCURSOR: 链接按钮上显示手型光标 ---
    case WM_SETCURSOR: {
        HWND hCursorWnd = (HWND)wParam;
        int ctrlId = GetDlgCtrlID(hCursorWnd);
        if (ctrlId == IDC_LINK_CABLE || ctrlId == IDC_LINK_SOUND) {
            SetCursor(LoadCursor(NULL, MAKEINTRESOURCE(32649)));
            return TRUE;
        }
        break;
    }

    // --- WM_DESTROY: 清理 ---
    case WM_DESTROY: {
        g_audioEngine.Stop();
        g_audioEngine.Uninitialize();
        CloseAllDecoders();

        if (g_hFont) {
            DeleteObject(g_hFont);
            g_hFont = NULL;
        }
        if (g_hLinkFont) {
            DeleteObject(g_hLinkFont);
            g_hLinkFont = NULL;
        }
        if (g_hBgBrush) {
            DeleteObject(g_hBgBrush);
            g_hBgBrush = NULL;
        }

        PostQuitMessage(0);
        return 0;
    }

    } // switch msg

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ============================================================================
// WinMain - 程序入口
// ============================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    // --- COM 初始化（WASAPI 需要） ---
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(NULL, L"COM 初始化失败", L"SoundTime 错误",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    // --- ComCtl 初始化（SysLink 等控件需要） ---
    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC  = ICC_LINK_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    // --- 注册窗口类 ---
    const wchar_t CLASS_NAME[] = L"SoundTimeWindow";

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm       = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        CoUninitialize();
        return 1;
    }

    // --- 计算窗口大小（客户区 350×215，加上标题栏和边框） ---
    RECT clientRect = { 0, 0, 350, 215 };
    AdjustWindowRect(&clientRect,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    int windowW = clientRect.right - clientRect.left;
    int windowH = clientRect.bottom - clientRect.top;

    // --- 窗口居中 ---
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - windowW) / 2;
    int posY = (screenH - windowH) / 2;

    // --- 创建窗口 ---
    HWND hWnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"SoundTime - 音频播放器",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, windowW, windowH,
        NULL, NULL, hInstance, NULL);

    if (!hWnd) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // --- 消息循环 ---
    MSG msg = {};
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // --- 清理 ---
    CoUninitialize();
    return (int)msg.wParam;
}
