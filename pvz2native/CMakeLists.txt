cmake_minimum_required (VERSION 3.1)

project(pvz2native-core)

file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS src/*.c src/*.cpp)
if (NOT APPLE)
    list(FILTER SOURCE_FILES EXCLUDE REGEX ".*/platform/macos/.*")
endif()

# Keep Objective-C out of CMake's language initialisation. The project already
# uses Clang as its C compiler on macOS; this one source is compiled through the
# normal C rule with Clang explicitly switched to Objective-C mode. This avoids
# requiring a separate CMAKE_OBJC_COMPILE_OBJECT rule in nested/subproject builds.
if (APPLE)
    set(MACOS_LAUNCHER_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos/launcher_objc.c")
    set_source_files_properties(
        "${MACOS_LAUNCHER_SOURCE}"
        PROPERTIES
            COMPILE_OPTIONS "-x;objective-c;-fobjc-arc"
    )
endif()

# stb_vorbis is gone with src/audio/sound_repository.c, its only consumer. That
# file, src/audio/sles.c and src/audio/audio_engine.c were an older OpenSL
# implementation with no callers left: the live audio path is the guest-vtable
# bridge in src/dependencies/libopensles.cpp plus src/audio/audio_output.cpp.
add_executable(pvz2native ${SOURCE_FILES})
target_include_directories(pvz2native PUBLIC include/)
target_link_libraries(pvz2native ${CMAKE_DL_LIBS} zlibstatic SDL2::SDL2 SDL2main glad dynarmic)

if (APPLE)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    target_link_libraries(pvz2native ${COCOA_FRAMEWORK})
endif()
include_directories(pvz2native PUBLIC include/)
set_target_properties(pvz2native PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# Link the MinGW runtime statically so the .exe runs on a machine that has no
# toolchain installed. Everything else here is already static (SDL_STATIC is
# forced on in the root CMakeLists, as are zlib/glad/dynarmic), so the only
# remaining runtime dependencies were the three GCC DLLs -- and copying the
# binary to another PC failed on exactly one of them, libwinpthread-1.dll,
# which std::thread and std::mutex pull in.
#
# 64-bit on purpose, and it has to stay that way: dynarmic ships host backends
# for x86_64, arm64 and riscv64 only. Building with -m32 makes its
# DetectArchitecture report "x86", no backend sources are compiled at all, and
# the link fails on every Dynarmic::A32::Jit symbol. A static x86_64 build runs
# on any 64-bit Windows anyway, which is what -m32 would have been for.
if (MINGW)
    target_link_options(pvz2native PRIVATE -static)
endif()
