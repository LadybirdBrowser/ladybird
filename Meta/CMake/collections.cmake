include_guard(GLOBAL)

include(FetchContent)

set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE OPT_IN)
FetchContent_Declare(SwiftCollections
    GIT_REPOSITORY https://github.com/apple/swift-collections.git
    GIT_TAG 1.1.2
    PATCH_COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_DIR}/patches/git-patch.cmake"
                  "${CMAKE_CURRENT_LIST_DIR}/patches/swift-collections/0001-CMake-Remove-top-level-binary-module-locations.patch"
    OVERRIDE_FIND_PACKAGE
)

set(BUILD_TESTING_SAVE ${BUILD_TESTING})
set(BUILD_TESTING OFF)
set(BUILD_EXAMPLES OFF)

FetchContent_MakeAvailable(SwiftCollections)

set(BUILD_TESTING ${BUILD_TESTING_SAVE})
