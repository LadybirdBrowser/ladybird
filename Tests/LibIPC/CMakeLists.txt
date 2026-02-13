if (NOT WIN32)
    ladybird_test("TestTransportSocket.cpp" LibIPC LIBS LibIPC)
endif()
