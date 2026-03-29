set(SOURCES
    Database.cpp
)

ladybird_lib(LibDatabase database EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibDatabase PRIVATE LibCore)

if (CMAKE_VERSION VERSION_GREATER_EQUAL 4.3.1)
    target_link_libraries(LibDatabase PRIVATE SQLite3::SQLite3)
else()
    target_link_libraries(LibDatabase PRIVATE SQLite::SQLite3)
endif()
