cmake_minimum_required(VERSION 3.19)
project(core_environ_module LANGUAGES CXX)

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

set(ENVIRON_PATH ${CMAKE_CURRENT_LIST_DIR})
set(ENVIRON_SOURCE_FILES
    ${ENVIRON_PATH}/Application.cpp
    ${ENVIRON_PATH}/DetectCPU.cpp
    ${ENVIRON_PATH}/DumpSend.cpp
    ${ENVIRON_PATH}/XP3ArchiveRepack.cpp

    $<$<BOOL:${ANDROID}>:${ENVIRON_PATH}/android/AndroidUtils.cpp>
    $<$<BOOL:${ANDROID}>:${ENVIRON_PATH}/linux/Platform.cpp>
    
    $<$<BOOL:${LINUX}>:${ENVIRON_PATH}/linux/Platform.cpp>

    $<$<BOOL:${WINDOWS}>:${ENVIRON_PATH}/win32/Platform.cpp>

    $<$<BOOL:${MACOS}>:${ENVIRON_PATH}/apple/macos/platform.mm>

    $<$<BOOL:${WEB}>:${ENVIRON_PATH}/web/Platform.cpp>
    $<$<BOOL:${WEB}>:${ENVIRON_PATH}/web/stubs.cpp>
    $<$<BOOL:${WEB}>:${ENVIRON_PATH}/web/VirtualLazyFS.cpp>

    ${ENVIRON_PATH}/cocos2d/AppDelegate.cpp
    ${ENVIRON_PATH}/cocos2d/CCKeyCodeConv.cpp
    ${ENVIRON_PATH}/cocos2d/CustomFileUtils.cpp
    ${ENVIRON_PATH}/cocos2d/MainScene.cpp
    ${ENVIRON_PATH}/cocos2d/YUVSprite.cpp

    ${ENVIRON_PATH}/ConfigManager/GlobalConfigManager.cpp
    ${ENVIRON_PATH}/ConfigManager/IndividualConfigManager.cpp
    ${ENVIRON_PATH}/ConfigManager/LocaleConfigManager.cpp
    ${ENVIRON_PATH}/sdl/tvpsdl.cpp

    ${ENVIRON_PATH}/ui/BaseForm.cpp
    ${ENVIRON_PATH}/ui/ConsoleWindow.cpp
    ${ENVIRON_PATH}/ui/DebugViewLayerForm.cpp
    ${ENVIRON_PATH}/ui/FileSelectorForm.cpp
    ${ENVIRON_PATH}/ui/GameMainMenu.cpp
    ${ENVIRON_PATH}/ui/GlobalPreferenceForm.cpp
    ${ENVIRON_PATH}/ui/IndividualPreferenceForm.cpp
    ${ENVIRON_PATH}/ui/MainFileSelectorForm.cpp
    ${ENVIRON_PATH}/ui/InGameMenuForm.cpp
    ${ENVIRON_PATH}/ui/MessageBox.cpp
    ${ENVIRON_PATH}/ui/PreferenceForm.cpp
    ${ENVIRON_PATH}/ui/SelectListForm.cpp
    ${ENVIRON_PATH}/ui/SimpleMediaFilePlayer.cpp
    ${ENVIRON_PATH}/ui/TipsHelpForm.cpp
    ${ENVIRON_PATH}/ui/XP3RepackForm.cpp
    ${ENVIRON_PATH}/ui/extension/ActionExtension.cpp
    ${ENVIRON_PATH}/ui/extension/UIExtension.cpp

    ${ENVIRON_PATH}/win32/SystemControl.cpp
#    ${ENVIRON_PATH}/win32/CompatibleNativeFuncs.cpp
#    ${ENVIRON_PATH}/win32/ConfigFormUnit.cpp
#    ${ENVIRON_PATH}/win32/DetectCPU.cpp
#    ${ENVIRON_PATH}/win32/EmergencyExit.cpp
#    ${ENVIRON_PATH}/win32/HitWindow.cpp
#    ${ENVIRON_PATH}/win32/MouseCursor.cpp
#    ${ENVIRON_PATH}/win32/my_HintWindow.cpp
#    ${ENVIRON_PATH}/win32/Platform.cpp
#    ${ENVIRON_PATH}/win32/TouchPoint.cpp
#    ${ENVIRON_PATH}/win32/TVPWindow.cpp
#    ${ENVIRON_PATH}/win32/VersionFormUnit.cpp
#    ${ENVIRON_PATH}/win32/WindowFormUnit.cpp
#    ${ENVIRON_PATH}/win32/WindowsUtil.cpp
)

set(ENVIRON_HEADERS_DIR
    ${ENVIRON_PATH}/
    ${ENVIRON_PATH}/win32
    $<$<BOOL:${ANDROID}>:${ENVIRON_PATH}/android>
    ${ENVIRON_PATH}/sdl
)

if(EMSCRIPTEN)
    list(REMOVE_ITEM ENVIRON_SOURCE_FILES ${ENVIRON_PATH}/ui/SimpleMediaFilePlayer.cpp)
endif()

if(KRKR2_ENABLE_WASMTIME_HEADLESS)
    # Wasmtime headless 差分 guest 无浏览器宿主（无 window.VLFS），且其
    # pthread 主线程代理队列无人消费，VLFS::Enabled() 的 RunOnMainSync
    # 探测会直接 futex abort——编译期禁用 VLFS，引擎走原 MEMFS 路径。
    set_source_files_properties(${ENVIRON_PATH}/web/VirtualLazyFS.cpp
        PROPERTIES COMPILE_DEFINITIONS KRKR2_WASMTIME_HEADLESS)
endif()

add_library(${PROJECT_NAME} STATIC ${ENVIRON_SOURCE_FILES})
target_include_directories(${PROJECT_NAME} PUBLIC ${ENVIRON_HEADERS_DIR})
target_link_libraries(${PROJECT_NAME} PUBLIC tjs2 PRIVATE core_base_module core_visual_module core_sound_module core_plugin_module core_movie_module core_utils_module)

find_package(cocos2dx CONFIG REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE
        cocos2dx::cocos2d
        minizip
)

if(NOT EMSCRIPTEN)
    find_package(7zip CONFIG REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE 7zip::7zip)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE -DKRKR2_NO_7ZIP -DKRKR2_NO_FFMPEG)
endif()
