cmake_minimum_required(VERSION 3.20)
project(sei-stamper VERSION 1.2.2)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# --- OBS Studio ---
if(NOT DEFINED OBS_DIR)
    set(OBS_DIR "${CMAKE_SOURCE_DIR}/obs-studio-master/build/rundir/Release"
        CACHE PATH "OBS installation directory")
endif()
list(APPEND CMAKE_MODULE_PATH "${OBS_DIR}/../cmake")
list(APPEND CMAKE_PREFIX_PATH "${OBS_DIR}")

find_package(libobs QUIET)
if(NOT libobs_FOUND)
    # Manual libobs setup (paths differ per platform, handled below)
endif()

# --- Source files (shared) ---
set(PLUGIN_SOURCES
    src/sei-stamper-plugin.c
    src/ntp-client.c
    src/sei-handler.c
    src/h264-sps.c
    src/unified-encoder.c
)

if(APPLE)
    # macOS: add VideoToolbox encoder, exclude Windows-only files
    list(APPEND PLUGIN_SOURCES src/videotoolbox-encoder.c)
    # Stubs for disabled encoders (their .c files have #ifdef guards that
    # produce empty translation units, but we still compile them for the
    # extern struct obs_encoder_info symbols)
    list(APPEND PLUGIN_SOURCES src/nvenc-encoder.c src/qsv-encoder.c
         src/amd-encoder.c)
elseif(WIN32)
    # Windows: all original sources
    list(APPEND PLUGIN_SOURCES
        src/sei-stamper-encoder.c
        src/qsv-encoder.c
        src/nvenc-encoder.c
        src/amd-encoder.c
        src/sei-receiver-source.c
    )
endif()

add_library(sei-stamper MODULE ${PLUGIN_SOURCES})

# --- Platform-specific configuration ---
if(APPLE)
    # Find FFmpeg via pkg-config or OBS deps
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FFMPEG QUIET libavcodec libavformat libavutil)
    endif()

    # OBS headers from source tree
    set(LIBOBS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/obs-studio-master/libobs")

    if(NOT FFMPEG_FOUND)
        # Try OBS source tree deps
        file(GLOB OBS_DEPS_DIRS
             "${CMAKE_SOURCE_DIR}/obs-studio-master/.deps/obs-deps-*-universal")
        if(NOT OBS_DEPS_DIRS)
            file(GLOB OBS_DEPS_DIRS
                 "${CMAKE_SOURCE_DIR}/obs-studio-master/.deps/obs-deps-*-arm64")
        endif()
        if(NOT OBS_DEPS_DIRS)
            file(GLOB OBS_DEPS_DIRS
                 "${CMAKE_SOURCE_DIR}/obs-studio-master/.deps/obs-deps-*-x86_64")
        endif()
        list(SORT OBS_DEPS_DIRS)
        list(REVERSE OBS_DEPS_DIRS)
        if(OBS_DEPS_DIRS)
            list(GET OBS_DEPS_DIRS 0 LATEST_OBS_DEPS_DIR)
            message(STATUS "Found OBS deps: ${LATEST_OBS_DEPS_DIR}")
            set(FFMPEG_INCLUDE_DIRS "${LATEST_OBS_DEPS_DIR}/include")
            set(FFMPEG_LIBRARY_DIRS "${LATEST_OBS_DEPS_DIR}/lib")
        endif()
    endif()

    # SIMDE (SSE intrinsics emulation for ARM, required by OBS headers)
    find_path(SIMDE_INCLUDE_DIR simde/x86/sse2.h
        PATHS /opt/homebrew/opt/simde/include /usr/local/include)

    target_include_directories(sei-stamper PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LIBOBS_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/obs-studio-master/build/config
        ${FFMPEG_INCLUDE_DIRS}
        ${SIMDE_INCLUDE_DIR}
    )

    # Link OBS
    if(NOT libobs_FOUND)
        find_library(LIBOBS_LIBRARY libobs
            PATHS "/Applications/OBS.app/Contents/Frameworks"
                  "${OBS_DIR}/lib" "${OBS_DIR}/bin"
                  "${CMAKE_SOURCE_DIR}/obs-studio-master/build/libobs")
        if(LIBOBS_LIBRARY)
            message(STATUS "Found libobs: ${LIBOBS_LIBRARY}")
            target_link_libraries(sei-stamper ${LIBOBS_LIBRARY})
        else()
            message(FATAL_ERROR "Could not find libobs. Install OBS Studio or set OBS_DIR.")
        endif()
    else()
        target_link_libraries(sei-stamper OBS::libobs)
    endif()

    # Link FFmpeg
    if(FFMPEG_LIBRARY_DIRS)
        target_link_directories(sei-stamper PRIVATE ${FFMPEG_LIBRARY_DIRS})
    endif()
    target_link_libraries(sei-stamper avcodec avformat avutil)

    # Apple frameworks
    find_library(VIDEOTOOLBOX_FRAMEWORK VideoToolbox)
    find_library(COREMEDIA_FRAMEWORK CoreMedia)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)
    target_link_libraries(sei-stamper
        ${VIDEOTOOLBOX_FRAMEWORK}
        ${COREMEDIA_FRAMEWORK}
        ${COREVIDEO_FRAMEWORK}
    )

    target_compile_definitions(sei-stamper PRIVATE ENABLE_VIDEOTOOLBOX)

elseif(WIN32)
    # === Windows: original configuration (unchanged) ===
    if(NOT libobs_FOUND)
        set(LIBOBS_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/obs-studio-master/libobs")
        set(LIBOBS_LIB_DIR
            "${CMAKE_SOURCE_DIR}/obs-studio-master/build/libobs/Release")
        set(LIBOBS_BIN_DIR "${OBS_DIR}/bin/64bit")
        add_library(OBS::libobs SHARED IMPORTED)
        set_target_properties(OBS::libobs PROPERTIES
            IMPORTED_LOCATION "${LIBOBS_BIN_DIR}/obs.dll"
            IMPORTED_IMPLIB "${LIBOBS_LIB_DIR}/obs.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBOBS_INCLUDE_DIR}"
        )
    endif()

    find_library(PTHREAD_LIBRARY NAMES w32-pthreads
        PATHS
        "${CMAKE_SOURCE_DIR}/obs-studio-master/build/deps/w32-pthreads/Release"
        "${CMAKE_SOURCE_DIR}/obs-studio-master/build/deps/w32-pthreads/Debug"
    )

    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FFMPEG QUIET libavcodec libavformat libavutil libswscale)
    endif()

    file(GLOB OBS_DEPS_DIRS
         "${CMAKE_SOURCE_DIR}/obs-studio-master/.deps/obs-deps-20??-??-??-x64")
    list(SORT OBS_DEPS_DIRS)
    list(REVERSE OBS_DEPS_DIRS)
    if(OBS_DEPS_DIRS)
        list(GET OBS_DEPS_DIRS 0 LATEST_OBS_DEPS_DIR)
    else()
        set(LATEST_OBS_DEPS_DIR
            "${CMAKE_SOURCE_DIR}/obs-studio-master/.deps/obs-deps-2025-08-23-x64")
    endif()

    if(NOT FFMPEG_FOUND)
        set(FFMPEG_INCLUDE_DIRS "${LATEST_OBS_DEPS_DIR}/include")
        set(FFMPEG_LIBRARY_DIRS "${LATEST_OBS_DEPS_DIR}/bin")
    endif()

    set(X264_INCLUDE_DIR "${LATEST_OBS_DEPS_DIR}/include")
    set(X264_LIBRARY "${LATEST_OBS_DEPS_DIR}/lib/libx264.lib")

    target_include_directories(sei-stamper PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${LIBOBS_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/obs-studio-master/build/config
        ${CMAKE_SOURCE_DIR}/obs-studio-master/deps/w32-pthreads
        ${FFMPEG_INCLUDE_DIRS}
    )

    # SRT
    find_library(SRT_LIBRARY NAMES srt libsrt
        PATHS
        "${CMAKE_SOURCE_DIR}/srt/_build/Release"
        "${OBS_DIR}/bin/64bit"
    )
    if(SRT_LIBRARY)
        set(SRT_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/srt/srtcore")
        target_include_directories(sei-stamper PRIVATE ${SRT_INCLUDE_DIRS})
    endif()

    target_link_libraries(sei-stamper
        OBS::libobs
        ws2_32
        ${PTHREAD_LIBRARY}
    )

    set(FFMPEG_LIB_DIR "${LATEST_OBS_DEPS_DIR}/lib")
    target_link_libraries(sei-stamper
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swscale.lib"
    )

    if(EXISTS "${X264_LIBRARY}")
        target_link_libraries(sei-stamper "${X264_LIBRARY}")
    endif()

    # VPL
    set(VPL_LIBRARY "${LATEST_OBS_DEPS_DIR}/lib/vpl.lib")
    if(EXISTS "${VPL_LIBRARY}")
        target_include_directories(sei-stamper
            PRIVATE "${LATEST_OBS_DEPS_DIR}/include")
        target_link_libraries(sei-stamper "${VPL_LIBRARY}")
        add_definitions(-DENABLE_VPL)
    endif()

    add_definitions(-DENABLE_NVENC)
    add_definitions(-DENABLE_AMD)

    if(SRT_LIBRARY)
        target_link_libraries(sei-stamper ${SRT_LIBRARY})
    endif()
endif()

# --- Output ---
set_target_properties(sei-stamper PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin"
)

install(TARGETS sei-stamper
    LIBRARY DESTINATION obs-plugins/64bit
    RUNTIME DESTINATION obs-plugins/64bit
)

install(DIRECTORY data/
    DESTINATION data/obs-plugins/sei-stamper
)

message(STATUS "=================================================")
message(STATUS "SEI Stamper Plugin Configuration")
message(STATUS "=================================================")
message(STATUS "OBS Directory: ${OBS_DIR}")
message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "=================================================")
