set(SOURCES
    Database.cpp
)

find_package(SQLite3 REQUIRED)

ladybird_lib(LibDatabase database EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibDatabase PRIVATE LibCore SQLite::SQLite3)
