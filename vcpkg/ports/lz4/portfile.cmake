# Preserve the codec behind lzfs and the software texture cache. The builtin
# registry no longer contains v1.7.4 (its oldest entry is v1.7.4.2).
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO lz4/lz4
    REF "v${VERSION}"
    SHA512 723c4489f17e3aa574a278ba8882d8de999c52dca09ec460e52019c7387d14d0877baf6e5e09aa7e2056ce52c22029ed5038889bfac1629a1491572012418973
    HEAD_REF dev
)

# Build-system adaptation only: use the unmodified release C sources and
# provide the lz4::lz4 config target expected by current consumers.
file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"
          "${CMAKE_CURRENT_LIST_DIR}/lz4-config.cmake.in"
     DESTINATION "${SOURCE_PATH}")

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES tools LZ4_BUILD_CLI)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS ${FEATURE_OPTIONS}
    OPTIONS_DEBUG -DCMAKE_DEBUG_POSTFIX=d
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/lz4)
vcpkg_fixup_pkgconfig()
if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/liblz4.pc"
                         " -llz4" " -llz4d")
endif()
if("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES lz4 AUTO_CLEAN)
endif()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include"
                    "${CURRENT_PACKAGES_DIR}/debug/share")
set(LICENSE_FILES "${SOURCE_PATH}/lib/LICENSE")
if("tools" IN_LIST FEATURES)
    list(APPEND LICENSE_FILES "${SOURCE_PATH}/programs/COPYING")
endif()
vcpkg_install_copyright(FILE_LIST ${LICENSE_FILES})
