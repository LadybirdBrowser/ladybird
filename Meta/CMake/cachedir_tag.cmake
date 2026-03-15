# This creates a CACHEDIR.TAG file in the build directory so that backup 
# tools automatically skip it.
set(CACHEDIR_TAG_CONTENT 
"Signature: 8a477f597d28d172789f06886806bc55
# This file is a cache directory tag created by the Ladybird project.
# For information about cache directory tags, see:
# https://bford.info/cachedir/
")

cmake_path(SET ROOT_BUILD_DIR "${CMAKE_BINARY_DIR}")
cmake_path(GET ROOT_BUILD_DIR PARENT_PATH ROOT_BUILD_DIR)

set(CACHEDIR_TAG_PATH "${ROOT_BUILD_DIR}/CACHEDIR.TAG")

if(NOT EXISTS "${CACHEDIR_TAG_PATH}")
    file(WRITE "${CACHEDIR_TAG_PATH}" "${CACHEDIR_TAG_CONTENT}")
endif()
