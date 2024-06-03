set(SQL_SERVER_SOURCE_DIR ${LADYBIRD_SOURCE_DIR}/Userland/Services/SQLServer)

set(SQL_SERVER_SOURCES
    ${SQL_SERVER_SOURCE_DIR}/ConnectionFromClient.cpp
    ${SQL_SERVER_SOURCE_DIR}/DatabaseConnection.cpp
    ${SQL_SERVER_SOURCE_DIR}/SQLStatement.cpp
    main.cpp
)

add_executable(SQLServer ${SQL_SERVER_SOURCES})

target_include_directories(SQLServer PRIVATE ${LADYBIRD_SOURCE_DIR}/Userland/Services/)
target_include_directories(SQLServer PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/..)
target_link_libraries(SQLServer PRIVATE LibCore LibFileSystem LibIPC LibSQL LibMain)
