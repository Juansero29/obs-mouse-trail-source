cmake_minimum_required(VERSION 3.16)

project(mouse_trail VERSION 1.0.0 LANGUAGES C CXX)

# Path layout expected by this lightweight MinGW build.
set(DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
set(OBS_HEADERS_DIR "${DEPS_DIR}/obs-studio-src/libobs")
set(OBS_CONFIG_DIR "${DEPS_DIR}/obs-studio-src/build_x64/config")
set(OBS_IMPORT_LIB "${DEPS_DIR}/lib/libobs.dll.a")

# Override OBS_ROOT at configure time if needed:
# cmake -B build -G Ninja -DOBS_ROOT="D:/Programs/OBS28/obs-studio"
set(OBS_ROOT "C:/Program Files/obs-studio" CACHE PATH "Root directory of OBS installation")
set(OBS_PLUGIN_DIR "${OBS_ROOT}/obs-plugins/64bit")
set(OBS_BIN_DIR "${OBS_ROOT}/bin/64bit")
set(OBS_DATA_PLUGIN_DIR "${OBS_ROOT}/data/obs-plugins/mouse_trail")

configure_file(src/plugin-support.c.in plugin-support.c @ONLY)

add_library(${PROJECT_NAME} MODULE
    src/plugin-main.c
    "${CMAKE_BINARY_DIR}/plugin-support.c"
    src/trail-source.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${OBS_HEADERS_DIR}"
    "${DEPS_DIR}/obs-studio-src"
    "${OBS_CONFIG_DIR}"
    src
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wno-unused-function>
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wno-unused-function>
)

target_link_libraries(${PROJECT_NAME} PRIVATE "${OBS_IMPORT_LIB}")

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "mouse_trail"
    PREFIX ""
    SUFFIX ".dll"
)

# Runtime DLLs needed by Strawberry Perl MinGW-built plugins.
set(MINGW_BIN "C:/Strawberry/c/bin")

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
