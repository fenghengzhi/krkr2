cmake_minimum_required(VERSION 3.16)
project(TestPlugins LANGUAGES CXX)

set(SOURCES
        lzfs-dll.cpp
        psbfile-dll.cpp
        motionplayer-dll.cpp
        motionplayer-ttstr-hash-test.cpp
)

string(REPLACE ".cpp" "" BASENAMES_SOURCES "${SOURCES}")
set(TARGETS ${BASENAMES_SOURCES})

foreach(name ${TARGETS})
    add_executable(${name} ${name}.cpp main.cpp)
endforeach()

find_package(lz4 CONFIG REQUIRED)
target_link_libraries(lzfs-dll PRIVATE lz4::lz4)

set(ALL_TARGETS
        ${TARGETS}
)

foreach(name ${ALL_TARGETS})
    target_link_libraries(${name}
        PRIVATE
            Catch2::Catch2
        PUBLIC
            krkr2plugin krkr2core
    )
    target_include_directories(${name} PRIVATE "${TEST_CONFIG_DIR}")
    catch_discover_tests(${name})
endforeach()

# D3DEmotePlayer's binary-faithful D3DLayer listener owner is implemented by
# DrawDeviceD3D.cpp in krkr2plugins_ncbind. The application whole-archives that
# library; isolated tests that compile EmotePlayer must link it explicitly as
# well.
foreach(name motionplayer-dll psbfile-dll motionplayer-ttstr-hash-test lzfs-dll)
    target_link_libraries(${name} PRIVATE krkr2plugins_ncbind)
endforeach()
