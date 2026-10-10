# CMake macOS defaults module

include_guard(GLOBAL)

# Use Applications directory as default install destination
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT OR CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
  set(
    CMAKE_INSTALL_PREFIX
    "$ENV{HOME}/Library/Application Support/obs-studio/plugins"
    CACHE STRING
    "Default plugin installation directory"
    FORCE
  )
endif()

# Set empty codesigning team if not specified as cache variable
if(NOT CODESIGN_TEAM)
  set(CODESIGN_TEAM "" CACHE STRING "OBS code signing team for macOS" FORCE)

  # Set ad-hoc codesigning identity if not specified as cache variable
  if(NOT CODESIGN_IDENTITY)
    set(CODESIGN_IDENTITY "-" CACHE STRING "OBS code signing identity for macOS" FORCE)
  endif()
endif()

include(xcode)

# Set default bundle identifier if not specified
if(NOT MACOS_BUNDLEID)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" buildspec)
  string(JSON _bundleId ERROR_VARIABLE _err GET ${buildspec} platformConfig macos bundleId)
  if(NOT _err AND _bundleId)
    set(MACOS_BUNDLEID "${_bundleId}" CACHE STRING "OBS plugin bundle identifier" FORCE)
  else()
    set(MACOS_BUNDLEID "com.nankill.${_name}" CACHE STRING "OBS plugin bundle identifier" FORCE)
  endif()
endif()

include(buildspec)

# Enable find_package targets to become globally available targets
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
# Enable RPATH support for generated binaries
set(CMAKE_MACOSX_RPATH TRUE)
# Use RPATHs from build tree _in_ the build tree
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
# Do not add default linker search paths to RPATH
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)
# Use common bundle-relative RPATH for installed targets
set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")
