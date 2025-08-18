cmake_minimum_required(VERSION 3.11)
project(librsvg C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCROCO libcroco-0.6 IMPORTED_TARGET)
pkg_check_modules(GLIB2 glib-2.0 gobject-2.0 gmodule-2.0 gio-2.0 IMPORTED_TARGET)
pkg_check_modules(CAIRO cairo IMPORTED_TARGET)
pkg_check_modules(GDK_PIXBUF gdk-pixbuf-2.0 IMPORTED_TARGET)
pkg_check_modules(PANGO pango pangocairo IMPORTED_TARGET)
find_package(LibXml2 REQUIRED)

if(CMAKE_SYSTEM_NAME MATCHES "Windows")
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux")
else()
    pkg_check_modules(PANGO2 pangoft2 pangofc IMPORTED_TARGET)
    set(PANGO_FT2_LIB PkgConfig::PANGO2)
endif()

set(RSVG_API_MAJOR_VERSION 2)
set(RSVG_API_VERSION "2.0")
Set(VERSION "2.40.20")
set(LIBRSVG_TARGET "rsvg-${RSVG_API_MAJOR_VERSION}")

Set(prefix "${CMAKE_INSTALL_PREFIX}")
Set(exec_prefix "\${prefix}")
Set(libdir "\${prefix}/lib")
Set(includedir "\${prefix}/include")

# Public required modules (cf. headers and librsvg.pc)
set(librsvg_pc_requires glib-2.0 gio-2.0 gdk-pixbuf-2.0 cairo)
# Other required modules from configure.ac
set(librsvg_pc_requires_private
    libxml-2.0
    pangocairo
    pangoft2
    cairo-png
    libcroco-0.6
    gthread-2.0
    gmodule-2.0
    gobject-2.0
    gio-unix-2.0
    fontconfig
)
if(WIN32)
    string(REPLACE "gio-unix" "gio-windows" librsvg_pc_requires_private "${librsvg_pc_requires_private}")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBRSVG_LINK_PUBLIC ${librsvg_pc_requires} IMPORTED_TARGET REQUIRED)
pkg_check_modules(LIBRSVG_LINK_PRIVATE ${librsvg_pc_requires_private} IMPORTED_TARGET REQUIRED)

set(LIBRSVG_SOURCES
    librsvg-features.c
    rsvg-css.c
    rsvg-css.h
    rsvg-compat.h
    rsvg-defs.c
    rsvg-defs.h
    rsvg-image.c
    rsvg-image.h
    rsvg-io.c
    rsvg-io.h
    rsvg-paint-server.c
    rsvg-paint-server.h
    rsvg-path.c
    rsvg-path.h
    rsvg-private.h
    rsvg-base-file-util.c
    rsvg-filter.c
    rsvg-filter.h
    rsvg-marker.c
    rsvg-marker.h
    rsvg-mask.c
    rsvg-mask.h
    rsvg-shapes.c
    rsvg-shapes.h
    rsvg-structure.c
    rsvg-structure.h
    rsvg-styles.c
    rsvg-styles.h
    rsvg-text.c
    rsvg-text.h
    rsvg-cond.c
    rsvg-base.c
    librsvg-enum-types.c
    rsvg-cairo-draw.c
    rsvg-cairo-draw.h
    rsvg-cairo-render.c
    rsvg-cairo-render.h
    rsvg-cairo-clip.h
    rsvg-cairo-clip.c
    rsvg.c
    rsvg-gobject.c
    rsvg-file-util.c
    rsvg-size-callback.c
    rsvg-size-callback.h
    rsvg-xml.c
    rsvg-xml.h
    rsvg.h
    rsvg-cairo.h
    librsvg-features.h
    librsvg-enum-types.h
)

if(WIN32)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/config.h.win32" "${CMAKE_CURRENT_BINARY_DIR}/config.h" COPYONLY)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/rsvg.symbols" rsvg_symbols)
    string(REGEX REPLACE "/[*][^*]*[*]/" "" rsvg_symbols "${rsvg_symbols}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rsvg.def" "EXPORTS\n${rsvg_symbols}")
    list(APPEND LIBRSVG_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/rsvg.def")
else()
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/config.h.linux" "${CMAKE_CURRENT_BINARY_DIR}/config.h" COPYONLY)
    list(APPEND LIBRSVG_SOURCES rsvg.symbols)
endif()

add_library(${LIBRSVG_TARGET} ${LIBRSVG_SOURCES})
target_compile_definitions(${LIBRSVG_TARGET} PRIVATE
    -DRSVG_COMPILATION
    -D_CRT_SECURE_NO_WARNINGS
    -DSRCDIR="${CMAKE_CURRENT_SOURCE_DIR}" 
    -DGDK_PIXBUF_ENABLE_BACKEND 
    -DG_LOG_DOMAIN="libpixbufloader-svg"
    -DSRCDIR=""
    $<$<BOOL:${MINGW}>:HAVE_STRTOK_R>
)

target_link_libraries(${LIBRSVG_TARGET} PRIVATE
    PkgConfig::GLIB2
    ${LIBXML2_LIBRARIES}
    PkgConfig::PANGO
    PkgConfig::GDK_PIXBUF
    ${PANGO_FT2_LIB}
    PkgConfig::LIBCROCO
)

target_include_directories(${LIBRSVG_TARGET}
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
        "$<INSTALL_INTERFACE:include/librsvg-${RSVG_API_VERSION}>"
)
target_link_libraries(${LIBRSVG_TARGET}
    PUBLIC
        PkgConfig::LIBRSVG_LINK_PUBLIC
    PRIVATE
        PkgConfig::LIBRSVG_LINK_PRIVATE
)
install(TARGETS ${LIBRSVG_TARGET}
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(
    FILES
        rsvg.h
        rsvg-cairo.h
        librsvg-features.h
        librsvg-enum-types.h
    DESTINATION include/librsvg-${RSVG_API_VERSION}/librsvg
)

set(gdk_pixbuf_pc_requires_private gdk-pixbuf-2.0)
pkg_check_modules(GDK_PIXBUF ${gdk_pixbuf_pc_requires_private} IMPORTED_TARGET REQUIRED)
pkg_get_variable(GDK_PIXBUF_MODULEDIR ${gdk_pixbuf_pc_requires_private} gdk_pixbuf_moduledir)

set(PIXBUFLOADERSVG_SOURCES
    gdk-pixbuf-loader/io-svg.c
)

add_library(pixbufloader-svg MODULE ${PIXBUFLOADERSVG_SOURCES})
target_include_directories(pixbufloader-svg
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(pixbufloader-svg PRIVATE
    -DRSVG_COMPILATION
    -D_CRT_SECURE_NO_WARNINGS
    -DSRCDIR=""
    -DGDK_PIXBUF_ENABLE_BACKEND
    -DG_LOG_DOMAIN="libpixbufloader-svg"
)
target_link_libraries(pixbufloader-svg
    PRIVATE
        ${LIBRSVG_TARGET}
        PkgConfig::GDK_PIXBUF
)
install(TARGETS pixbufloader-svg
    RUNTIME DESTINATION "${GDK_PIXBUF_MODULEDIR}"
    LIBRARY DESTINATION "${GDK_PIXBUF_MODULEDIR}"
)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/librsvg.pc.in" "${CMAKE_CURRENT_BINARY_DIR}/librsvg.pc" @ONLY)
file(READ "${CMAKE_CURRENT_BINARY_DIR}/librsvg.pc" librsvg_pc)
list(JOIN librsvg_pc_requires_private " " requires_private)
string(REPLACE "Requires.private:" "Requires.private: ${requires_private}" librsvg_pc "${librsvg_pc}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/librsvg.pc" "${librsvg_pc}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/librsvg.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" RENAME "librsvg-${RSVG_API_VERSION}.pc")
