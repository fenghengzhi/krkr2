cmake_minimum_required(VERSION 3.19)
project(krkr2plugin)

add_library(${PROJECT_NAME} STATIC)

set(_PLUGIN_SOURCES
    AlphaMovie.cpp
    scriptsEx.cpp
    win32dialog.cpp
    dirlist.cpp
    csvParser.cpp
    varfile.cpp
    saveStruct.cpp
    getabout.cpp
    addFont.cpp
    wutcwf.cpp
    windowEx.cpp
    getSample.cpp
    layerExPerspective.cpp
    fftgraph.cpp
    LayerExBase.cpp
    layerExMovie.cpp
    kirikiroid2.cpp
    lzfs.cpp
)
target_sources(${PROJECT_NAME} PRIVATE ${_PLUGIN_SOURCES})

# CBinaryAccessor is one native dispatch type shared by xp3filter.dll and the
# emoteplayer PreRegist callback's two Motion.ResourceManager decrypt setters.
# The four references agree on this translation-unit sharing and dependency.
add_library(krkr2_binary_accessor STATIC
    BinaryAccessor.cpp
)
target_include_directories(krkr2_binary_accessor PUBLIC
    ${KRKR2PLUGIN_PATH}
)
target_link_libraries(krkr2_binary_accessor PUBLIC krkr2core)

set(_KRKR2_NCBIND_SOURCES
    xp3filter.cpp
    shrinkCopy.cpp
    layerExBTOA.cpp
    layerExRaster.cpp
    PackinOne.cpp
    DrawDeviceD3D.cpp
    textrender/TextRender.cpp
    extrans/Main.cpp
    extrans/wave.cpp
    extrans/mosaic.cpp
    extrans/turn.cpp
    extrans/turntrans_table.cpp
    extrans/rotatebase.cpp
    extrans/rotatetrans.cpp
    extrans/ripple.cpp
)

# layerExImage is an optional git submodule.  Keep a fresh checkout buildable
# when it is intentionally absent instead of asking source-restoration work to
# locate an external repository.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/layerExImage/Main.cpp" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/layerExImage/LayerExImage.cpp")
    list(APPEND _KRKR2_NCBIND_SOURCES
        layerExImage/Main.cpp
        layerExImage/LayerExImage.cpp
    )
    set_source_files_properties(
        layerExImage/Main.cpp
        layerExImage/LayerExImage.cpp
        PROPERTIES COMPILE_DEFINITIONS "NCB_MODULE_NAME=TJS_W(\"layerExImage.dll\")"
    )
else()
    message(STATUS "layerExImage submodule is absent; skipping layerExImage plugin sources")
endif()

add_library(krkr2plugins_ncbind STATIC ${_KRKR2_NCBIND_SOURCES})
set_source_files_properties(
    extrans/Main.cpp
    extrans/wave.cpp
    extrans/mosaic.cpp
    extrans/turn.cpp
    extrans/turntrans_table.cpp
    extrans/rotatebase.cpp
    extrans/rotatetrans.cpp
    extrans/ripple.cpp
    PROPERTIES COMPILE_DEFINITIONS "NCB_MODULE_NAME=TJS_W(\"extrans.dll\")"
)
target_include_directories(krkr2plugins_ncbind PRIVATE
    ${KRKR2PLUGIN_PATH}
    ${KRKR2PLUGIN_PATH}/simplebinder}
    ${KRKR2PLUGIN_PATH}/layerExImage
    ${KRKR2PLUGIN_PATH}/extrans
)
target_link_libraries(krkr2plugins_ncbind PRIVATE
    krkr2core
    krkr2_binary_accessor
)

target_include_directories(${PROJECT_NAME} PUBLIC
    ${KRKR2CORE_PATH}/
    ${KRKR2CORE_PATH}/base
    ${KRKR2CORE_PATH}/base/win32
    ${KRKR2CORE_PATH}/environ
    ${KRKR2CORE_PATH}/environ/win32
    ${KRKR2CORE_PATH}/environ/android
    ${KRKR2CORE_PATH}/environ/sdl
    ${KRKR2CORE_PATH}/msg
    ${KRKR2CORE_PATH}/msg/win32
    ${KRKR2CORE_PATH}/extension
    ${KRKR2CORE_PATH}/sound
    ${KRKR2CORE_PATH}/sound/win32
    ${KRKR2CORE_PATH}/tjs2
    ${KRKR2CORE_PATH}/utils
    ${KRKR2CORE_PATH}/utils/win32
    ${KRKR2CORE_PATH}/visual
    ${KRKR2CORE_PATH}/visual/ARM
    ${KRKR2CORE_PATH}/visual/win32
    ${KRKR2CORE_PATH}/visual/RenderScript/rs
)

add_definitions(-D__STDC_CONSTANT_MACROS -DTJS_TEXT_OUT_CRLF)

target_include_directories(${PROJECT_NAME} PUBLIC ${KRKR2PLUGIN_PATH} ${KRKR2PLUGIN_PATH}/simplebinder} ${KRKR2PLUGIN_PATH}/layerExImage)
target_link_libraries(${PROJECT_NAME} PUBLIC krkr2core)

find_package(lz4 CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE lz4::lz4)

add_subdirectory(psdfile)
add_subdirectory(psbfile)
add_subdirectory(layerex_draw)
add_subdirectory(motionplayer)

#add_subdirectory(json)
add_subdirectory(fstat)
#add_subdirectory(steam)
#add_subdirectory(DrawDeviceForSteam)
target_link_libraries(${PROJECT_NAME} PUBLIC
        psdfile
        psbfile
        layerExDraw
        motionplayer
        fstat
#        json steam DrawDeviceForSteam
)

# Monolithic native builds link these plugin translation units directly into
# the final image, so every NCB static registrar is present before
# ncbAutoRegister::AllRegist(). The Web/macOS ports package each module as a
# static archive instead; ordinary archive extraction drops registrar-only
# objects such as motionplayer/main.cpp.
# Force-load the already-separated archives at final executable links without
# propagating their sources into one another.
set(_KRKR2_PLUGIN_ARCHIVES
    ${PROJECT_NAME}
    psdfile
    psbfile
    layerExDraw
    motionplayer
    fstat
)
foreach(_plugin_archive IN LISTS _KRKR2_PLUGIN_ARCHIVES)
    if(APPLE AND NOT EMSCRIPTEN)
        target_link_options(${PROJECT_NAME} INTERFACE
            "-Wl,-force_load,$<TARGET_FILE:${_plugin_archive}>"
        )
    elseif(MSVC)
        target_link_options(${PROJECT_NAME} INTERFACE
            "/WHOLEARCHIVE:$<TARGET_FILE:${_plugin_archive}>"
        )
    else()
        target_link_options(${PROJECT_NAME} INTERFACE
            "SHELL:-Wl,--whole-archive $<TARGET_FILE:${_plugin_archive}> -Wl,--no-whole-archive"
        )
    endif()
endforeach()

find_package(fmt CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE fmt::fmt)

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)
