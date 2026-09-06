cmake_minimum_required(VERSION 3.28)

find_program(CCACHE_PROGRAM ccache)

if (CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Ccache found: ${CCACHE_PROGRAM}")
endif ()

add_compile_definitions(
        $<$<CONFIG:Debug>:DEBUG>
        $<$<CONFIG:Debug>:_DEBUG>   # 对于 MSVC 兼容性
        $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

if(ANDROID)
    set(VCPKG_TARGET_ANDROID ON)
    if(ANDROID_NDK)
        set(ENV{ANDROID_NDK_HOME} "${ANDROID_NDK}")
    endif()
    add_compile_options(-Wno-inconsistent-missing-override)
    include(cmake/vcpkg_android.cmake)
elseif(VCPKG_TARGET_TRIPLET STREQUAL "wasm32-emscripten" OR EMSCRIPTEN)
    set(WEB ON)
    set(VCPKG_TARGET_TRIPLET "wasm32-emscripten")
    set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/triplets")
    if(CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg")
        set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_TOOLCHAIN_FILE}")
    endif()
    set(CMAKE_TOOLCHAIN_FILE $ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake)
else()
    set(CMAKE_TOOLCHAIN_FILE $ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake)
endif()

set(APP_NAME krkr2)

project(${APP_NAME})

option(ENABLE_TESTS "enable tests execute build(exclude android)" ON)
option(BUILD_TOOLS "build tools execute build(exclude android ios)" ON)
option(KRKR2_WEB_DEBUG_ENABLE_ASAN "enable AddressSanitizer in Web Debug builds" ON)
option(KRKR2_WEB_ENABLE_SOURCE_MAP "emit a WebAssembly source map for Web builds" OFF)
option(KRKR2_ENABLE_WASMTIME_HEADLESS "build the generic Wasmtime headless debug runner" OFF)
if(NOT DEFINED KRKR2_USE_STUB_SPDLOG)
    set(_KRKR2_USE_STUB_SPDLOG_DEFAULT OFF)
    if(WEB AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_KRKR2_USE_STUB_SPDLOG_DEFAULT ON)
    endif()
    set(KRKR2_USE_STUB_SPDLOG ${_KRKR2_USE_STUB_SPDLOG_DEFAULT} CACHE BOOL
        "use the no-op spdlog stub package")
endif()

if(KRKR2_USE_STUB_SPDLOG)
    set(spdlog_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/spdlog_stub" CACHE PATH
        "stub spdlog package path" FORCE)
    message(STATUS "Using no-op spdlog stub")
elseif(DEFINED spdlog_DIR AND spdlog_DIR MATCHES "/cmake/spdlog_stub$")
    unset(spdlog_DIR CACHE)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(KRKR2CORE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cpp/core)
set(KRKR2PLUGIN_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cpp/plugins)

if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/EHsc>" "/MP" "/utf-8")
    add_link_options("/ignore:4099" "/INCREMENTAL" "/DEBUG:FASTLINK")
    # set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(WEB)
    add_compile_options(
        -pthread
        -fwasm-exceptions
        -msimd128
        $<$<AND:$<CONFIG:Debug>,$<BOOL:${KRKR2_WEB_DEBUG_ENABLE_ASAN}>>:-fsanitize=address>
        $<$<BOOL:${KRKR2_WEB_ENABLE_SOURCE_MAP}>:-gline-tables-only>
    )
    add_link_options($<$<AND:$<CONFIG:Debug>,$<BOOL:${KRKR2_WEB_DEBUG_ENABLE_ASAN}>>:-fsanitize=address>)
endif()

if(ANDROID)
    add_library(${PROJECT_NAME} SHARED ${CMAKE_CURRENT_SOURCE_DIR}/platforms/android/cpp/krkr2_android.cpp)
    find_package(unofficial-breakpad CONFIG REQUIRED)
    target_link_libraries(${PROJECT_NAME} PUBLIC unofficial::breakpad::libbreakpad_client)
elseif(WEB)
    if(NOT EMSCRIPTEN_VERSION VERSION_EQUAL "6.0.9")
        message(FATAL_ERROR
            "Web pthread JSPI targets Emscripten 6.0.9; review its SDK lifecycle hooks before upgrading")
    endif()
    add_executable(${PROJECT_NAME}
        ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/pthread_jspi.c)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "index" SUFFIX ".html")
    set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/jspi_jsc_mainloop_fix.js
        ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/pthread_jspi.js
        ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/vlfs.js
        ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/version_web_entrypoint.cmake)
    # WebAssembly.Memory 初始大小。ASan 的影子内存（覆盖整个地址空间的 1/8）+
    # redzone 把静态数据段顶得很高，wasm 模块声明的最小内存随之暴涨（非 ASan ~64MB，
    # ASan 实测需 ~843MB）。该值必须 >= wasm 声明的 min，否则 JS 创建的 Memory 页数
    # 小于导入声明 → LinkError（memory import has N pages < declared initial of M）。
    # 此变量同时驱动链接请求值和 shell.html 的预分配初值（iOS Safari 兼容路径）。
    # ASan 请求 1 GiB，但链接器仍可能按静态数据与影子内存上调实际最小内存；
    # 6.0.9 当前构建约为 1.29 GiB。链接后的 sync_prealloc_memory.cmake 会从
    # index.js 读取实际默认值并更新 shell，不能假定链接请求值就是 wasm min。
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND KRKR2_WEB_DEBUG_ENABLE_ASAN)
        set(KRKR2_WEB_INITIAL_MEMORY 1073741824)  # 1 GiB
    else()
        set(KRKR2_WEB_INITIAL_MEMORY 67108864)    # 64 MiB
    endif()
    # Worker 按需初始化：主线程创建时经 JSPI 等待，后台 pthread 经异步代理
    # 等待。已就绪 Worker 仍同步复用；此值只控制可选预热，不再要求覆盖峰值。
    set(KRKR2_WEB_PTHREAD_POOL_SIZE "0" CACHE STRING
        "Optional pre-warmed Emscripten pthread workers")
    target_link_options(${PROJECT_NAME} PRIVATE
        "SHELL:-s USE_SDL=2"
        "SHELL:-s USE_SDL_TTF=2"
        "SHELL:-s MAX_WEBGL_VERSION=2"
        "SHELL:-s FULL_ES2=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        # Declare this explicitly so 6.0.9 retains Module.wasmMemory and
        # Module.INITIAL_MEMORY before it derives imported memory from -pthread.
        # shell.html supplies the shared memory; sync_prealloc_memory.cmake
        # reads the generated INITIAL_MEMORY fallback after linking.
        "SHELL:-s IMPORTED_MEMORY=1"
        # 见上方 KRKR2_WEB_INITIAL_MEMORY 注释（ASan 1GB / 非 ASan 64MB）。
        "SHELL:-s INITIAL_MEMORY=${KRKR2_WEB_INITIAL_MEMORY}"
        "SHELL:-s STACK_SIZE=2097152"
        "$<$<CONFIG:Debug>:SHELL:-s STACK_OVERFLOW_CHECK=2>"
        "$<$<NOT:$<CONFIG:Debug>>:SHELL:-s STACK_OVERFLOW_CHECK=0>"
        "$<$<CONFIG:Debug>:SHELL:-s ASSERTIONS=2>"
        "SHELL:-s FORCE_FILESYSTEM=1"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
        "SHELL:-s PTHREAD_POOL_SIZE=${KRKR2_WEB_PTHREAD_POOL_SIZE}"
        "SHELL:-s DEFAULT_PTHREAD_STACK_SIZE=1048576"
        # VirtualLazyFS: 主线程文件读经 JSPI 挂起等待 JS 异步供数
        # （Chromium 137+ / WebKit 26.4+，即 Safari 27 beta；EM_ASYNC_JS 导入
        # 自动包 WebAssembly.Suspending）。
        # krkr2_main_loop_tick 是项目显式加入 JSPI_EXPORTS 的入口（cocos 主循环
        # tick，见 vcpkg cocos2dx patch CCApplication-emscripten.cpp）。Emscripten
        # 6.0.9 还会自动加入 main/__main_argc_argv，并单独用 promising 包装
        # pthread 入口；其它 JS 回调入口是否可挂起仍需逐条检查。glue 的
        # getWasmTableEntry 会给"同时是 JSPI_EXPORTS 导出"的函数指针表项
        # 包 WebAssembly.promising。该识别靠函数对象身份比较，JSC 不满足
        # table.get/export 身份等价，需用户库覆盖 set_main_loop_arg 兜底，
        # 见 jspi_jsc_mainloop_fix.js。
        "SHELL:-s JSPI=1"
        "SHELL:-s JSPI_EXPORTS=['krkr2_main_loop_tick']"
        "SHELL:--js-library ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/jspi_jsc_mainloop_fix.js"
        "SHELL:--js-library ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/pthread_jspi.js"
        -Wl,--wrap=pthread_create
        -Wl,--wrap=__pthread_create
        -Wl,--wrap=emscripten_builtin_pthread_create
        -fwasm-exceptions
        $<$<BOOL:${KRKR2_WEB_ENABLE_SOURCE_MAP}>:-gsource-map>
        $<$<CONFIG:Debug>:--emit-symbol-map>
        $<$<CONFIG:Debug>:-g2>
        $<$<CONFIG:Debug>:-O1>
        -pthread
        -lopenal
        --shell-file ${CMAKE_CURRENT_BINARY_DIR}/shell.html
    )
    # UI 资源/字体改经 VirtualLazyFS 按需读取（cocos CCFileUtils →
    # krkr2_vlfs_read_all 桥）：打成 store（不压缩）模式 assets.zip，
    # shell.html 启动时 fetch 为 Blob 注册进 VLFS，条目即 Blob 切片，
    # 不再像 --preload-file 那样全量驻留 MEMFS。
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    file(GLOB_RECURSE KRKR2_UI_ASSET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/ui/cocos-studio/*)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/assets.zip
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CreateStoredZip.py
            ${CMAKE_CURRENT_BINARY_DIR}/assets.zip
            ${CMAKE_CURRENT_SOURCE_DIR}/ui/cocos-studio
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ui/cocos-studio
        DEPENDS
            ${KRKR2_UI_ASSET_FILES}
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CreateStoredZip.py
        COMMENT "Packing UI assets (stored zip) for VirtualLazyFS"
        VERBATIM
    )
    add_custom_target(krkr2_web_assets ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/assets.zip)
    add_dependencies(${PROJECT_NAME} krkr2_web_assets)
    target_compile_options(${PROJECT_NAME} PRIVATE "SHELL:-s USE_SDL=2" "SHELL:-s USE_SDL_TTF=2" -fwasm-exceptions)
    # Local ZIP opener is visible by default in Debug, and can be enabled
    # explicitly in Release with ?pickZip=1.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(KRKR2_LOCAL_ZIP_PICKER_DEFAULT_VISIBLE "true")
    else()
        set(KRKR2_LOCAL_ZIP_PICKER_DEFAULT_VISIBLE "false")
    endif()
    # One identity for every mutually dependent Web artifact. CI supplies the
    # source SHA plus a unique run identity; local builds fall back to the
    # configure timestamp.
    # Restrict the value to URL/cache-key-safe characters because it is also
    # embedded in query strings and the Service Worker cache name.
    if(NOT DEFINED KRKR2_BUILD_VERSION OR KRKR2_BUILD_VERSION STREQUAL "")
        string(TIMESTAMP KRKR2_BUILD_VERSION "%Y%m%d%H%M%S")
    endif()
    if(NOT KRKR2_BUILD_VERSION MATCHES "^[A-Za-z0-9._-]+$")
        message(FATAL_ERROR
            "KRKR2_BUILD_VERSION must contain only A-Z, a-z, 0-9, dot, underscore or hyphen")
    endif()
    message(STATUS "KrKr2 Web build version: ${KRKR2_BUILD_VERSION}")
    # PWA: only enabled for non-Debug builds
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        file(READ ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/pwa-head.html KRKR2_PWA_HEAD)
        file(READ ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/pwa-sw.html KRKR2_PWA_SCRIPT)
        configure_file(
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/sw.js
            ${CMAKE_CURRENT_BINARY_DIR}/sw.js
            @ONLY
        )
    else()
        set(KRKR2_PWA_HEAD "")
        # Unregister any previously installed service worker and clear caches
        file(READ ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/pwa-unregister.html KRKR2_PWA_SCRIPT)
    endif()
    # Process shell.html template (injects PWA content for Release, empty for Debug)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/shell.html
        ${CMAKE_CURRENT_BINARY_DIR}/shell.html
        @ONLY
    )
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(
            TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/manifest.webmanifest
                ${CMAKE_CURRENT_BINARY_DIR}/sw.js
                $<TARGET_FILE_DIR:${PROJECT_NAME}>
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/pwa
                $<TARGET_FILE_DIR:${PROJECT_NAME}>/pwa
            COMMENT "Copy PWA manifest, service worker, and icons"
        )
    endif()
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/baidu_verify_codeva-3Z4bQwqyXI.html
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/robots.txt
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/sitemap.xml
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/vlfs.js
            ${CMAKE_CURRENT_BINARY_DIR}/assets.zip
            $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMENT "Copy site verification, robots.txt, sitemap.xml, vlfs.js, assets.zip"
    )
    # 链接后把 shell.html 预分配的 WebAssembly.Memory initial 同步到 emscripten
    # 实际烘焙进 index.js 的 INITIAL_MEMORY 默认值。ASan 影子内存把 wasm 声明的 min
    # 顶到高于链接参数 -s INITIAL_MEMORY（且随代码增长），配置期无法预测；不同步则
    # shell 提供的 Memory 页数 < 声明 min → LinkError，引擎启动前即 Aborted。
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DJS=$<TARGET_FILE_DIR:${PROJECT_NAME}>/index.js
            -DHTML=$<TARGET_FILE_DIR:${PROJECT_NAME}>/index.html
            -P ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/sync_prealloc_memory.cmake
        COMMENT "Sync shell prealloc memory to emscripten-baked wasm min"
        VERBATIM
    )
    # Emscripten expands {{{ SCRIPT }}} only at link time, so version its
    # generated index.js tag afterwards. Module.locateFile in shell.html uses
    # the same build id for index.wasm; this prevents an HTTP-cache mix of old
    # glue and new Wasm before the Service Worker has taken control.
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DHTML=$<TARGET_FILE_DIR:${PROJECT_NAME}>/index.html
            -DBUILD_VERSION=${KRKR2_BUILD_VERSION}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/platforms/web/version_web_entrypoint.cmake
        COMMENT "Version Web entrypoint assets as one atomic build"
        VERBATIM
    )
elseif(LINUX)
    add_executable(${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/platforms/linux/main.cpp)
elseif(WINDOWS)
    add_executable(${PROJECT_NAME}
        ${CMAKE_CURRENT_SOURCE_DIR}/platforms/windows/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/platforms/windows/game.rc
    )
elseif(IOS)
#        list(APPEND GAME_HEADER
#                proj.ios_mac/ios/AppController.h
#                proj.ios_mac/ios/RootViewController.h
#        )
#        set(APP_UI_RES
#                proj.ios_mac/ios/LaunchScreen.storyboard
#                proj.ios_mac/ios/LaunchScreenBackground.png
#                proj.ios_mac/ios/Images.xcassets
#        )
#        list(APPEND GAME_SOURCE
#                proj.ios_mac/ios/main.m
#                proj.ios_mac/ios/AppController.mm
#                proj.ios_mac/ios/RootViewController.mm
#                proj.ios_mac/ios/Prefix.pch
#                ${APP_UI_RES}
#        )
elseif(MACOS)
    set(APP_UI_RES
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/Icon.icns
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/Info.plist
    )
    add_executable(${PROJECT_NAME}
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/main.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/platforms/apple/macos/Prefix.pch
            ${APP_UI_RES}
    )
endif()

target_include_directories(${PROJECT_NAME} PUBLIC ${KRKR2CORE_PATH}/environ/cocos2d)

# external lib
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/cpp/external)

# build library
add_subdirectory(${KRKR2CORE_PATH})
add_subdirectory(${KRKR2PLUGIN_PATH})
if(WEB)
    add_subdirectory(tests/differential/wasmtime)
    if(KRKR2_ENABLE_WASMTIME_HEADLESS)
        add_subdirectory(platforms/wasmtime)
    endif()
endif()

target_link_libraries(${PROJECT_NAME} PUBLIC
    krkr2plugin krkr2core
)

if(APPLE AND NOT EMSCRIPTEN)
    target_link_libraries(${PROJECT_NAME} PUBLIC
        -Wl,-force_load,$<TARGET_FILE:krkr2plugins_ncbind>
    )
else()
    target_link_libraries(${PROJECT_NAME} PUBLIC
        -Wl,--whole-archive krkr2plugins_ncbind -Wl,--no-whole-archive
    )
endif()

## resources
if(NOT ANDROID AND NOT WEB)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CocosBuildHelpers.cmake")

    set(GAME_RES_FOLDER "${CMAKE_CURRENT_SOURCE_DIR}/ui/cocos-studio")

    if(WINDOWS OR APPLE)
        cocos_mark_multi_resources(common_res_files RES_TO "Resources" FOLDERS ${GAME_RES_FOLDER})
    endif()

    # mark app resources
    setup_cocos_app_config(${APP_NAME})

    if(APPLE)
        set_target_properties(${APP_NAME} PROPERTIES RESOURCE "${APP_UI_RES}")
        target_sources(${APP_NAME} PRIVATE ${common_res_files})
        if(MACOSX)
            set_target_properties(${APP_NAME} PROPERTIES
                    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/apple/macos/Info.plist"
            )
        elseif(IOS)
#            cocos_pak_xcode(${APP_NAME} INFO_PLIST "iOSBundleInfo.plist.in")
#            set_xcode_property(${APP_NAME} ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon")
#            set_xcode_property(${APP_NAME} DEVELOPMENT_TEAM "")
#            set_xcode_property(${APP_NAME} CODE_SIGN_IDENTITY "iPhone Developer")
        endif()
    elseif(WINDOWS)
        target_sources(${PROJECT_NAME} PRIVATE ${common_res_files})
        cocos_copy_target_dll(${APP_NAME})
    endif()

    if(LINUX OR WINDOWS)
        cocos_get_resource_path(APP_RES_DIR ${APP_NAME})
        cocos_copy_target_res(${APP_NAME} LINK_TO ${APP_RES_DIR} FOLDERS ${GAME_RES_FOLDER})
    endif()
endif()

if(ENABLE_TESTS AND NOT (IOS OR ANDROID OR WEB))
    enable_testing()
    add_subdirectory(tests)
endif()

if(BUILD_TOOLS AND NOT (IOS OR ANDROID OR WEB))
    add_subdirectory(tools)
endif()
