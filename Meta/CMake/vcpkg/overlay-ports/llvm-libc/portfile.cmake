# The math functions of LLVM libc are header-only, so this installs the headers of libc/ and builds nothing.
set(VCPKG_BUILD_TYPE release)

# NB: Double sinh, cosh and tanh are not in a release yet, so the version is a commit of main. From LLVM 24, this can
#     take https://github.com/llvm/llvm-project/releases/download/llvmorg-${VERSION}/llvm-project-${VERSION}.src.tar.xz.
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/llvm/llvm-project/archive/${VERSION}.tar.gz"
    FILENAME "llvm-project-${VERSION}.tar.gz"
    SHA512 4ab7f1f2c49a661148b38414c16d3600467f43a295ab0383358098eb2434dd0c21ea202da25bcd466d3b8f1b397527dd7793f4a7025f79388492fee291947bed
)

# The archive holds all of llvm-project, so only libc/ is extracted.
set(SOURCE_PATH "${CURRENT_BUILDTREES_DIR}/src")
file(REMOVE_RECURSE "${SOURCE_PATH}")
file(MAKE_DIRECTORY "${SOURCE_PATH}")
vcpkg_execute_required_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${ARCHIVE}" "llvm-project-${VERSION}/libc"
    WORKING_DIRECTORY "${SOURCE_PATH}"
    LOGNAME extract
)
set(LIBC_PATH "${SOURCE_PATH}/llvm-project-${VERSION}/libc")

# The headers the math functions include, at their paths under libc/.
foreach(directory hdr include shared src/__support src/string/memory_utils)
    get_filename_component(destination "${CURRENT_PACKAGES_DIR}/include/llvm-libc/${directory}" DIRECTORY)
    file(INSTALL "${LIBC_PATH}/${directory}" DESTINATION "${destination}" FILES_MATCHING PATTERN "*.h")
endforeach()

# FILES_MATCHING leaves the directories that hold no headers behind empty.
file(GLOB_RECURSE directories LIST_DIRECTORIES true "${CURRENT_PACKAGES_DIR}/include/*")
list(SORT directories ORDER DESCENDING)
foreach(directory IN LISTS directories)
    file(GLOB children "${directory}/*")
    if(IS_DIRECTORY "${directory}" AND NOT children)
        file(REMOVE_RECURSE "${directory}")
    endif()
endforeach()

vcpkg_install_copyright(FILE_LIST "${LIBC_PATH}/LICENSE.TXT")
