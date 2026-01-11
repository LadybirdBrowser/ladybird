find_package(PkgConfig)
pkg_check_modules(
   mm REQUIRED IMPORTED_TARGET
   cairomm-1.16
   pangomm-2.48
   gdk-pixbuf-2.0
   graphene-1.0
)

pkg_check_modules(gtk4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(gtkmm REQUIRED IMPORTED_TARGET glibmm-2.68 gtkmm-4.0)
pkg_check_modules(libadwaita REQUIRED IMPORTED_TARGET libadwaita-1)

find_program(GLIB_COMPILE_RESOURCES NAMES glib-compile-resources REQUIRED)
#find_program(BLUEPRINT_COMPILER NAMES blueprint-compiler REQUIRED)

add_executable(ladybird
   main.cpp
   EventLoopImplementationGLib.cpp
)

target_link_libraries(ladybird PRIVATE PkgConfig::mm PkgConfig::gtk4 PkgConfig::gtkmm PkgConfig::libadwaita)
create_ladybird_bundle(ladybird)
