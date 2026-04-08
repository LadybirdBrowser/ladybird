find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(LIBADWAITA REQUIRED IMPORTED_TARGET libadwaita-1>=1.4)

add_executable(ladybird main.cpp)

target_sources(ladybird PRIVATE
    Application.cpp
    EventLoopImplementationGtk.cpp
)
target_link_libraries(ladybird PRIVATE PkgConfig::GTK4 PkgConfig::LIBADWAITA)
create_ladybird_bundle(ladybird)
