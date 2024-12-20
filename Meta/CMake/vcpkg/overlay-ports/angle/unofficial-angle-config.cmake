include(CMakeFindDependencyMacro)
find_dependency(ZLIB)
if(UNIX AND NOT APPLE)
  find_dependency(X11 COMPONENTS Xext Xi)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/unofficial-angle-targets.cmake")
