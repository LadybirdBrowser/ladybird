set(SOURCES
    Main.cpp
)

ladybird_lib(LibMain main TYPE STATIC)
target_link_libraries(LibMain PRIVATE LibThreading)
target_link_libraries(LibMain INTERFACE "$<LINK_ONLY:LibThreading>")
