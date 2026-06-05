cmake_minimum_required(VERSION 3.12.4)

set(CMAKE_C_COMPILER "/mnt/d/ArmPath/arm-linux-gcc/usr/local/arm/5.4.0/usr/bin/arm-linux-gcc")
set(CMAKE_CXX_COMPILER "/mnt/d/ArmPath/arm-linux-gcc/usr/local/arm/5.4.0/usr/bin/arm-linux-g++")

project(jarvis)

set(BEMFA_UID_DEFAULT "bf0bae36c31f649e87ae77d1216a6c95" CACHE STRING "Bemfa TCP UID/private key" FORCE)

include_directories(.)
include_directories(./include)
include_directories(./lvgl)
include_directories(./lv_drivers)
include_directories(./ui)

add_subdirectory(lvgl)
add_subdirectory(lv_drivers)
add_subdirectory(./ui)

add_executable(${PROJECT_NAME}
    main.c  # 主程序入口
    src/audio_output.c  # 音频输出
    src/lyrics_parser.c # 歌词解析
    src/music.c # 音乐播放
    src/video.c # 视频播放
    src/deepseek.c # DeepSeek 控制
    src/asr.c # ASR 识别
    src/tts.c # TTS 朗读
    src/pipeline.c # 管线处理
    src/device.c   # 智能设备模拟进程 + 链表管理
    src/bemfa.c    # 巴法云 TCP 客户端
)

if(BEMFA_UID_DEFAULT)
    target_compile_definitions(${PROJECT_NAME} PRIVATE BEMFA_UID_DEFAULT="${BEMFA_UID_DEFAULT}")
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    lvgl lvgl::examples lvgl::demos lvgl::drivers ui
    pthread
)
#打开vosk语音模型
#VOSK_SAMPLE_RATE=16000 python3 asr_server.py ./vosk-model-cn-0.22

#开启tts服务
#python3 tts_server.py
