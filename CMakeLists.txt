cmake_minimum_required(VERSION 3.16)

project(mouse_trail VERSION 1.0.0 LANGUAGES C CXX)

# Path layout expected by this lightweight MinGW build.
set(DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps")

if(WIN32)
    set(DEFAULT_OBS_HEADERS_DIR "${DEPS_DIR}/obs-studio-src/libobs")
    set(DEFAULT_OBS_CONFIG_DIR "${DEPS_DIR}/obs-studio-src/build_x64/config")
    set(DEFAULT_OBS_LIB_PATH "${DEPS_DIR}/lib/libobs.dll.a")
else()
    set(DEFAULT_OBS_HEADERS_DIR "${DEPS_DIR}/obs-studio-src/libobs")
    set(DEFAULT_OBS_CONFIG_DIR "")
    set(DEFAULT_OBS_LIB_PATH "obs")
endif()

set(OBS_HEADERS_DIR "${DEFAULT_OBS_HEADERS_DIR}" CACHE PATH "Path to OBS libobs headers directory")
set(OBS_CONFIG_DIR "${DEFAULT_OBS_CONFIG_DIR}" CACHE PATH "Path to generated OBS obsconfig.h directory")
set(OBS_LIB_PATH "${DEFAULT_OBS_LIB_PATH}" CACHE STRING "Path or linker name for libobs")

# Override OBS_ROOT at configure time if needed:
# cmake -B build -G Ninja -DOBS_ROOT="D:/Programs/OBS28/obs-studio"
set(OBS_ROOT "C:/Program Files/obs-studio" CACHE PATH "Root directory of OBS installation")
set(OBS_PLUGIN_DIR "${OBS_ROOT}/obs-plugins/64bit")
set(OBS_BIN_DIR "${OBS_ROOT}/bin/64bit")
set(OBS_DATA_PLUGIN_DIR "${OBS_ROOT}/data/obs-plugins/mouse_trail")
option(MOUSE_TRAIL_DEPLOY_AFTER_BUILD "Copy plugin artifacts to OBS after each build" ON)

configure_file(src/plugin-support.c.in plugin-support.c @ONLY)

add_library(${PROJECT_NAME} MODULE
    src/plugin-main.c
    "${CMAKE_BINARY_DIR}/plugin-support.c"
    src/trail-source.cpp
)

set(MOUSE_TRAIL_INCLUDE_DIRS
    "${OBS_HEADERS_DIR}"
    "${DEPS_DIR}/obs-studio-src"
    src
)

if(OBS_CONFIG_DIR)
    list(APPEND MOUSE_TRAIL_INCLUDE_DIRS "${OBS_CONFIG_DIR}")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${MOUSE_TRAIL_INCLUDE_DIRS})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wno-unused-function>
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wno-unused-function>
)

target_link_libraries(${PROJECT_NAME} PRIVATE "${OBS_LIB_PATH}")

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "mouse_trail"
    PREFIX ""
)

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".dll")
endif()

# Runtime DLLs needed by Strawberry Perl MinGW-built plugins.
set(MINGW_BIN "C:/Strawberry/c/bin")

if(MOUSE_TRAIL_DEPLOY_AFTER_BUILD AND WIN32)
    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${PROJECT_NAME}>
            "${OBS_PLUGIN_DIR}/mouse_trail.dll"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${OBS_DATA_PLUGIN_DIR}/locale"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini"
            "${OBS_DATA_PLUGIN_DIR}/locale/en-US.ini"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/fr-FR.ini"
            "${OBS_DATA_PLUGIN_DIR}/locale/fr-FR.ini"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/es-ES.ini"
            "${OBS_DATA_PLUGIN_DIR}/locale/es-ES.ini"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/de-DE.ini"
            "${OBS_DATA_PLUGIN_DIR}/locale/de-DE.ini"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MINGW_BIN}/libgcc_s_seh-1.dll"
            "${OBS_BIN_DIR}/libgcc_s_seh-1.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MINGW_BIN}/libstdc++-6.dll"
            "${OBS_BIN_DIR}/libstdc++-6.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MINGW_BIN}/libwinpthread-1.dll"
            "${OBS_BIN_DIR}/libwinpthread-1.dll"
        COMMENT "Copying mouse_trail.dll, locale data, and MinGW runtimes to OBS..."
    )
endif()
