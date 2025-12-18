####################### CMakeLists.txt (libopenshot) #########################
# @brief CMake build file for libopenshot (used to generate makefiles)
# @author Jonathan Thomas <jonathan@openshot.org>
# @author FeRD (Frank Dana) <ferdnyc@gmail.com>
#
# @section LICENSE
#
# Copyright (c) 2008-2020 OpenShot Studios, LLC
#
# SPDX-License-Identifier: LGPL-3.0-or-later

include(GNUInstallDirs)

# Qt selection matches the main library (USE_QT6 option, CMake 3.16+ needed for Qt6)
set(_qt6_allowed TRUE)
if(CMAKE_VERSION VERSION_LESS "3.16")
  set(_qt6_allowed FALSE)
  if(USE_QT6 STREQUAL "ON")
    message(FATAL_ERROR "USE_QT6=ON requires CMake 3.16 or newer")
  endif()
endif()
if(USE_QT6 STREQUAL "OFF")
  set(_qt6_allowed FALSE)
endif()

set(_qt_selection "")
if(USE_QT6 STREQUAL "ON")
  if(NOT _qt6_allowed)
    message(FATAL_ERROR "Qt6 support disabled by CMake version; set USE_QT6=OFF or upgrade CMake")
  endif()
  set(_qt_selection "Qt6")
elseif(USE_QT6 STREQUAL "OFF")
  set(_qt_selection "Qt5")
else()
  if(_qt6_allowed)
    find_package(Qt6 COMPONENTS Core QUIET)
    if(Qt6_FOUND)
      set(_qt_selection "Qt6")
    endif()
  endif()
  if(NOT _qt_selection)
    set(_qt_selection "Qt5")
  endif()
endif()

if(_qt_selection STREQUAL "Qt6")
  set(QT_VERSION_MAJOR 6)
else()
  set(QT_VERSION_MAJOR 5)
endif()

find_package(Qt${QT_VERSION_MAJOR} COMPONENTS Gui REQUIRED)

############### CLI EXECUTABLES ################
# Create test executable
add_executable(openshot-example Example.cpp)

# Define path to test input files
file(TO_NATIVE_PATH "${PROJECT_SOURCE_DIR}/examples/" TEST_MEDIA_PATH)
target_compile_definitions(openshot-example PRIVATE
	-DTEST_MEDIA_PATH="${TEST_MEDIA_PATH}" )

# Link test executable to the new library
target_link_libraries(openshot-example openshot)

add_executable(openshot-html-example ExampleHtml.cpp)
target_link_libraries(openshot-html-example openshot Qt${QT_VERSION_MAJOR}::Gui)

############### PLAYER EXECUTABLE ################
# Create test executable
add_executable(openshot-player qt-demo/main.cpp)

set_target_properties(openshot-player
  PROPERTIES
    AUTOMOC ON
    WIN32_EXECUTABLE ON
)

# Link test executable to the new library
target_link_libraries(openshot-player openshot)

############### OPENCV EXAMPLE ################
#if (DEFINED CACHE{HAVE_OPENCV})
#	# Create test executable
#	add_executable(openshot-example-opencv
#			Example_opencv.cpp)
#
#	target_compile_definitions(openshot-example-opencv PRIVATE
#	                           -DTEST_MEDIA_PATH="${TEST_MEDIA_PATH}" )
#	# Link test executable to the new library
#	target_link_libraries(openshot-example-opencv openshot)
#endif()
