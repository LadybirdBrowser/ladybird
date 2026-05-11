# Exported from "@gn_target@"
if(NOT TARGET @cmake_target@)
    @add_target@
    if("@not_executable@")
        include("${CMAKE_CURRENT_LIST_DIR}/@basename@-targets-debug.cmake" OPTIONAL)
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/@basename@-targets-release.cmake")
endif()
