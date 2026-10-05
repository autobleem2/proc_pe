# proc_pe: an AutoBleem scanner processor - turns PE app packages (.mod) into Apps. A processor is a plain console
# program: no AutoBleem SDK, no SDL, nothing but the C++ standard library and the vendored decoders (miniz for
# a gzip'd control file; libarchive + liblzma for the tar.xz data), so it builds with any toolchain. Modelled on
# proc_unzip.
cmake_minimum_required(VERSION 3.12)
project(proc_pe C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the console's Cortex-A35 (ARMv8-A, AArch32, NEON), as the launcher builds for it
if (AB_TARGET STREQUAL "psc")
    if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 8.0)
        set(CMAKE_C_FLAGS "-mfloat-abi=hard -march=armv8-a+simd -Os -s")
        set(CMAKE_CXX_FLAGS "-mfloat-abi=hard -march=armv8-a+simd -Os -s")
    else()
        set(CMAKE_C_FLAGS "-mfloat-abi=hard -march=armv8-a -mfpu=neon-vfpv4 -Os -s")
        set(CMAKE_CXX_FLAGS "-mfloat-abi=hard -march=armv8-a -mfpu=neon-vfpv4 -Os -s")
    endif()
endif()

# a package can pass 2 GB: 64-bit file offsets on the 32-bit targets too
if (NOT WIN32)
    add_compile_definitions(_FILE_OFFSET_BITS=64)
endif()

add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)
set_source_files_properties(third_party/miniz/miniz.c PROPERTIES COMPILE_FLAGS "-w")
target_compile_definitions(miniz PUBLIC MINIZ_NO_TIME)

# the data.tar.xz: libarchive's tar reader and xz filter over the vendored liblzma
include(third_party/archive.cmake)

# the version lives in one place: package/processor.ini (the program, the self-test and the package all read it)
file(STRINGS package/processor.ini PE_VERSION_LINE REGEX "^Version=")
string(REGEX REPLACE "^Version=([^\r\n]*).*" "\\1" PE_VERSION "${PE_VERSION_LINE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS package/processor.ini)

add_executable(pe src/main.cpp)
target_compile_definitions(pe PRIVATE PE_VERSION="${PE_VERSION}")
target_link_libraries(pe miniz archive)
if (MINGW OR WIN32)
    # one file to copy onto the stick: no libstdc++/libgcc DLLs next to it
    target_link_options(pe PRIVATE -static)
endif()
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pe PRIVATE -Wall -Wextra)
endif()

# the self-test: real archives, the real program, what the protocol and the rules promise
option(PROC_BUILD_TESTS "build and register the self-test" ON)
# a cross build runs it only where the build machine can run the result: the i386 PC stick
if (PROC_BUILD_TESTS AND (NOT CMAKE_CROSSCOMPILING OR AB_TARGET STREQUAL "pcusb"))
    enable_testing()
    add_executable(pe_selftest tests/selftest.cpp)
    target_compile_definitions(pe_selftest PRIVATE PE_EXE="$<TARGET_FILE:pe>"
                                                   TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
                                                   PE_VERSION="${PE_VERSION}")
    add_dependencies(pe_selftest pe)
    add_test(NAME pe_selftest COMMAND pe_selftest WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()
