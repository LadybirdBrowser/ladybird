if (UNIX AND NOT APPLE)
    ladybird_test_suite(TestIPC LibIPC SOURCES TestTransportSocket.cpp TestConnection.cpp LIBS LibIPC)
endif()
