if (UNIX AND NOT APPLE)
    ladybird_test("TestTransportSocket.cpp" LibIPC LIBS LibIPC)
endif()
