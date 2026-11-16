set(SOURCES
    CaptureStream.cpp
    ClientConnection.cpp
    DeviceInfo.cpp
    Devices.cpp
)

set(GENERATED_SOURCES
    CameraClientEndpoint.h
    CameraServerEndpoint.h
    ../../Services/CameraServer/CameraServerControlClientEndpoint.h
    ../../Services/CameraServer/CameraServerControlEndpoint.h
)

compile_ipc(CameraClient.ipc CameraClientEndpoint.h)
compile_ipc(CameraServer.ipc CameraServerEndpoint.h)
compile_ipc(../../Services/CameraServer/CameraServerControl.ipc ../../Services/CameraServer/CameraServerControlEndpoint.h)
compile_ipc(../../Services/CameraServer/CameraServerControlClient.ipc ../../Services/CameraServer/CameraServerControlClientEndpoint.h)

ladybird_lib(LibCamera camera)
target_link_libraries(LibCamera PRIVATE LibCore LibIPC)
