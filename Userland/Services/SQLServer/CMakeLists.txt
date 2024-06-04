compile_ipc(SQLServer.ipc SQLServerEndpoint.h)
compile_ipc(SQLClient.ipc SQLClientEndpoint.h)

set(SOURCES
    ConnectionFromClient.cpp
    DatabaseConnection.cpp
    main.cpp
    SQLStatement.cpp
)

set(GENERATED_SOURCES
    SQLClientEndpoint.h
    SQLServerEndpoint.h
)

serenity_bin(SQLServer)
target_link_libraries(SQLServer PRIVATE LibCore LibIPC LibSQL LibMain)
