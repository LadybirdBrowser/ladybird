if(ANDROID AND ANDROID_NATIVE_API_LEVEL LESS 24)
    # https://android.googlesource.com/platform/bionic/+/master/docs/32-bit-abi.md
    set(HAVE_FILE_OFFSET_BITS FALSE CACHE INTERNAL "")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    add_compile_definitions(_WINSOCK_DEPRECATED_NO_WARNINGS)
endif()
