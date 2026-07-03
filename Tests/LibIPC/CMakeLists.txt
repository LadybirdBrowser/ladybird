if (UNIX AND NOT APPLE)
    ladybird_test("TestTransportSocket.cpp" LibIPC LIBS LibIPC LibSync)
    ladybird_test("TestConnection.cpp" LibIPC LIBS LibIPC)
endif()
