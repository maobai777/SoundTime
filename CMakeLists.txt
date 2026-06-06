cmake_minimum_required(VERSION 3.15)
project(SoundTime VERSION 1.0.0 LANGUAGES CXX)

# ============================================================================
# SoundTime - 本地音频虚拟麦克风播放器 (支持 WAV / MP3)
#
# 构建要求：
#   - Windows 10/11 SDK
#   - Visual Studio 2019/2022 (MSVC)
#   - CMake 3.15+
#
# 构建命令：
#   mkdir build && cd build
#   cmake .. -G "Visual Studio 17 2022" -A x64
#   cmake --build . --config Release
# ============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows 特有配置
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# --- 源文件 ---
set(SOURCES
    main.cpp
    audio_engine.cpp
    wav_reader.cpp
    mp3_decoder.cpp
)

set(HEADERS
    audio_engine.h
    wav_reader.h
    mp3_decoder.h
)

# --- 可执行文件 ---
add_executable(SoundTime WIN32 ${SOURCES} ${HEADERS})

# --- 链接库 ---
# ole32.lib        : COM 基础 (CoInitializeEx, CoCreateInstance 等)
# msacm32.lib      : ACM 音频格式转换 (acmStreamOpen 等)
# mf.lib/mfplat.lib: Media Foundation (MP3 解码)
# 注意: mfreadwrite 在 MinGW 中缺失，改用 LoadLibrary 动态加载
target_link_libraries(SoundTime PRIVATE
    ole32
    msacm32
    mf
    mfplat
)

# --- 编译器特定的编译选项 ---
if(MSVC)
    target_compile_options(SoundTime PRIVATE
        /W4           # 警告级别 4
        /utf-8        # UTF-8 源文件和执行字符集
    )
    target_compile_definitions(SoundTime PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
    )
elseif(MINGW OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(SoundTime PRIVATE
        -finput-charset=UTF-8      # 源文件为 UTF-8 编码
        -fexec-charset=UTF-8       # 窄字符串使用 UTF-8
    )
    # MinGW 需要额外链接这些库
    target_link_libraries(SoundTime PRIVATE
        comctl32
        uuid
    )
endif()

# --- 输出目录 ---
set_target_properties(SoundTime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_SOURCE_DIR}/bin"
)
