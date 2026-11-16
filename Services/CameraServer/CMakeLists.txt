set(SOURCES
    CameraBackend.cpp
    CaptureSession.cpp
    ControlConnection.cpp
    FrameConversion.cpp
    ServerConnection.cpp
    SyntheticCameraBackend.cpp
    Tabs.cpp
)

add_library(cameraserverservice STATIC ${SOURCES})
add_executable(CameraServer main.cpp)

if (LINUX)
    target_sources(CameraServer PRIVATE SandboxLinux.cpp)
else()
    target_sources(CameraServer PRIVATE SandboxUnimplemented.cpp)
endif()

add_dependencies(cameraserverservice
    generate_CameraServerControlClientEndpoint.h
    generate_CameraServerControlEndpoint.h
)

target_include_directories(cameraserverservice PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/../..)
target_include_directories(cameraserverservice PRIVATE ${LADYBIRD_SOURCE_DIR}/Services/)

target_link_libraries(CameraServer PRIVATE cameraserverservice LibCamera LibCore LibMain LibSandbox LibThreading)
find_package(libyuv CONFIG REQUIRED)
target_link_libraries(cameraserverservice PRIVATE LibCamera LibCore LibIPC LibThreading yuv)

# The platform backends: the only code in the tree that opens a camera.
if (NOT APPLE AND NOT WIN32)
    pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
    if (PIPEWIRE_FOUND)
        target_sources(cameraserverservice PRIVATE PipeWireCameraBackend.cpp)
        target_compile_definitions(cameraserverservice PRIVATE CAMERASERVER_PLATFORM_BACKEND=1)
        target_link_libraries(cameraserverservice PRIVATE PkgConfig::PIPEWIRE)
    else()
        message(WARNING "No camera backend available")
    endif()
endif()

if (WIN32)
    ladybird_windows_bin(CameraServer CONSOLE)
endif()

set_target_properties(CameraServer PROPERTIES OUTPUT_NAME ladybird-camera)
ladybird_sign_helper_process(CameraServer)
