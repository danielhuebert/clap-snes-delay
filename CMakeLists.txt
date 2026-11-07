cmake_minimum_required(VERSION 3.21)
project(spc-echo-clap LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Universal binary on macOS so the plugin loads in both Apple-Silicon and Intel hosts.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

# --- CLAP headers: use the vendored copy if present, otherwise fetch them. ---
set(CLAP_LOCAL "${CMAKE_CURRENT_SOURCE_DIR}/deps/clap/include")
if(EXISTS "${CLAP_LOCAL}/clap/clap.h")
  set(CLAP_INCLUDE_DIR "${CLAP_LOCAL}")
else()
  include(FetchContent)
  FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.10
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(clap)
  set(CLAP_INCLUDE_DIR "${clap_SOURCE_DIR}/include")
endif()

# A CLAP plugin is a dynamically loaded module exporting the `clap_entry` symbol.
add_library(${PROJECT_NAME} MODULE src/spc_delay.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE "${CLAP_INCLUDE_DIR}")
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)

# Hide everything except clap_entry (default visibility on that one symbol only).
set_target_properties(${PROJECT_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  OUTPUT_NAME "SPC Echo"
  PREFIX "")

if(APPLE)
  # Package as a proper .clap bundle.
  set_target_properties(${PROJECT_NAME} PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION clap
    MACOSX_BUNDLE_BUNDLE_NAME "SPC Echo"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.danielhuebert.spc-echo"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
  # macOS wants the bundle package type to be BNDL for plugins.
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "BNDL????" >
            "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/PkgInfo"
    COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>"
    VERBATIM)
else()
  # Linux/Windows: a single shared object with the .clap extension.
  set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".clap")
endif()
