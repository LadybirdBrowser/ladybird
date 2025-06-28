add_subdirectory(ImageDecoder)
add_subdirectory(RequestServer)
add_subdirectory(WebContent)
add_subdirectory(WebWorker)

# FIXME: Increase support for building targets on Windows
if (WIN32 AND ENABLE_WINDOWS_CI)
    return()
endif()

add_subdirectory(WebDriver)
