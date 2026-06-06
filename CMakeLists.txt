# --- Detect if the plugin is built out of tree or not ---
if(CMAKE_PROJECT_NAME STREQUAL "obs-studio")
  set(BUILD_OUT_OF_TREE OFF)
else()
  set(BUILD_OUT_OF_TREE ON)
  cmake_minimum_required(VERSION 3.18)
endif()

project(isolated-record VERSION 0.1.2)
set(PROJECT_FULL_NAME "Isolated Record")

# New UUIDs for this plugin (regenerate if you fork).
set(MACOS_PACKAGE_UUID "B2F3A1C4-7E6D-4F2A-9B3C-1D5E8F0A6C72")
set(MACOS_INSTALLER_UUID "C9D8E7F6-5A4B-4C3D-8E2F-0A1B2C3D4E5F")

add_library(${PROJECT_NAME} MODULE)

configure_file(src/version.h.in ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h)

# Qt for the dock UI. AUTOMOC processes the Q_OBJECT in dock.hpp.
set(CMAKE_AUTOMOC ON)

target_sources(${PROJECT_NAME} PRIVATE
  src/plugin-main.cpp
  src/isolated-record.cpp
  src/isolated-record.hpp
  src/recorder-api.cpp
  src/recorder-api.hpp
  src/audio-recorder.cpp
  src/audio-recorder.hpp
  src/dock.cpp
  src/dock.hpp)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Designated initializers for obs_source_info are cleanest in C++20.
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

if(BUILD_OUT_OF_TREE)
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
  include(cmake/ObsPluginHelpers.cmake)
  set(OBS_FRONTEND_API_NAME "obs-frontend-api")
else()
  set(OBS_FRONTEND_API_NAME "frontend-api")
endif()

find_package(Qt6 COMPONENTS Widgets Core REQUIRED)
target_link_libraries(${PROJECT_NAME} Qt6::Widgets Qt6::Core)
set_target_properties(${PROJECT_NAME} PROPERTIES AUTOMOC ON)

if(OS_MACOS)
  set(MACOS_BUNDLEID "com.exeldro.${PROJECT_NAME}")
  set(MACOSX_PLUGIN_GUI_IDENTIFIER "${MACOS_BUNDLEID}")
  set(MACOSX_PLUGIN_BUNDLE_VERSION "${PROJECT_VERSION}")
  set(MACOSX_PLUGIN_SHORT_VERSION_STRING "1")
  target_compile_options(${PROJECT_NAME} PRIVATE -Wall)
endif()

target_link_libraries(${PROJECT_NAME}
  OBS::${OBS_FRONTEND_API_NAME}
  OBS::libobs)

if(BUILD_OUT_OF_TREE)
  if(NOT LIB_OUT_DIR)
    set(LIB_OUT_DIR "/lib/obs-plugins")
  endif()
  if(NOT DATA_OUT_DIR)
    set(DATA_OUT_DIR "/share/obs/obs-plugins/${PROJECT_NAME}")
  endif()
  set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
  install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/${LIB_OUT_DIR})
  install(DIRECTORY data/locale
    DESTINATION ${CMAKE_INSTALL_PREFIX}/${DATA_OUT_DIR})
  setup_plugin_target(${PROJECT_NAME})
else()
  target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_SOURCE_DIR}/UI/obs-frontend-api")
  if(OBS_CMAKE_VERSION VERSION_GREATER_EQUAL 3.0.0)
    set_target_properties_obs(${PROJECT_NAME} PROPERTIES FOLDER "plugins/exeldro" PREFIX "")
  else()
    set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "plugins/exeldro")
    setup_plugin_target(${PROJECT_NAME})
  endif()
endif()
