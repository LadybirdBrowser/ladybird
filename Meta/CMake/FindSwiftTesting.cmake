# Finds the swift-testing library
# On Apple platforms, this is a framework included in the Xcode release

# FIXME: Using Xcode's library actually doesn't work for rpath reasons
#        When swift-testing ships better toolchain CMake support, we'll need to revisit this

include(FetchContent)

# Allow the Ninja generators to output messages as they happen by assigning
# these jobs to the 'console' job pool
set(console_access "")
if(CMAKE_GENERATOR MATCHES "^Ninja")
    set(console_access
        USES_TERMINAL_CONFIGURE YES
        USES_TERMINAL_BUILD YES
        USES_TERMINAL_INSTALL YES
    )
endif()

set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE OPT_IN)
FetchContent_Declare(SwiftTesting
    GIT_REPOSITORY https://github.com/swiftlang/swift-testing.git
    GIT_TAG d00d46920f9bb35342ad29398ea4740a2bbf3d38
    PATCH_COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_DIR}/patches/git-patch.cmake"
                  "${CMAKE_CURRENT_LIST_DIR}/patches/swift-testing//0001-CMake-Allow-ExternalProjects-to-use-console-with-Nin.patch"
    OVERRIDE_FIND_PACKAGE
    SYSTEM
    ${console_access}
)

block()
    add_cxx_compile_options(-Wno-error)
    set(SwiftTesting_MACRO "<auto>")
    FetchContent_MakeAvailable(SwiftTesting)
    add_cxx_compile_options(-Werror)
endblock()

if (NOT TARGET SwiftTesting::SwiftTesting)
    # FIXME: This should be an interface property on the target itself, if the maintainers intend
    #        for the repository to be fetch-content-able
    set_property(TARGET Testing APPEND PROPERTY INTERFACE_COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:Swift>:SHELL:-load-plugin-executable ${CMAKE_BINARY_DIR}/bin/TestingMacros#TestingMacros>")
    add_library(SwiftTesting::SwiftTesting ALIAS Testing)
    set(SwiftTesting_LIBRARIES SwiftTesting::SwiftTesting)
endif()
set(SwiftTesting_FOUND TRUE)
