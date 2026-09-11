cmake_minimum_required(VERSION 3.19)
project(lz4 VERSION 1.7.4 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(LZ4_BUILD_CLI "Build the lz4 command line tool" OFF)

add_library(lz4 lib/lz4.c lib/lz4hc.c lib/lz4frame.c lib/xxhash.c)
add_library(lz4::lz4 ALIAS lz4)
target_include_directories(lz4 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib>
    $<INSTALL_INTERFACE:include>)
# The release Makefile uses this prefix too. Cocos exports a different XXH32
# streaming ABI, so the bundled frame checksum must use LZ4's own xxhash.
target_compile_definitions(lz4 PRIVATE XXH_NAMESPACE=LZ4_)
set_target_properties(lz4 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION 1)
if(WIN32 AND BUILD_SHARED_LIBS)
    target_compile_definitions(lz4 PRIVATE LZ4_DLL_EXPORT=1
                                   INTERFACE LZ4_DLL_IMPORT=1)
endif()

if(LZ4_BUILD_CLI)
    add_executable(lz4cli programs/bench.c programs/lz4cli.c
                          programs/lz4io.c programs/datagen.c)
    target_compile_definitions(lz4cli PRIVATE XXH_NAMESPACE=LZ4_)
    target_link_libraries(lz4cli PRIVATE lz4)
    set_target_properties(lz4cli PROPERTIES OUTPUT_NAME lz4)
    install(TARGETS lz4cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(TARGETS lz4 EXPORT lz4-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES lib/lz4.h lib/lz4hc.h lib/lz4frame.h lib/lz4frame_static.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
configure_package_config_file(lz4-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/lz4-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lz4)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/lz4-config-version.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/lz4-config.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/lz4-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lz4)
install(EXPORT lz4-targets NAMESPACE lz4::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lz4)

set(PREFIX "\${pcfiledir}/../..")
set(LIBDIR "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
set(INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
set(VERSION ${PROJECT_VERSION})
configure_file(lib/liblz4.pc.in liblz4.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/liblz4.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
