# With LIBJS_RUNTIME=Rust, Libraries/LibJS/CMakeLists.txt includes this file in place of building the C++ runtime. The
# LibJS target is then the facade: the C++ API that LibJS's users need, implemented over the Rust runtime's embedding
# ABI. It links the runtime with the runtime's symbols hidden, so it exports only its own JS_API functions, and every
# process that links LibJS has exactly one copy of the runtime.

add_subdirectory(Rust/Runtime)

set(SOURCES ${LIBJS_FILES_SHARED_WITH_CPP_RUNTIME})
list(FILTER SOURCES INCLUDE REGEX "\\.cpp$")
file(GLOB_RECURSE facade_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/LibJS/*.cpp")
list(APPEND SOURCES ${facade_sources})

ladybird_lib(LibJS js EXPLICIT_SYMBOL_EXPORT)

target_link_libraries(LibJS PRIVATE LibCore LibCrypto LibGC LibTextCodec)

# JS::SyntaxHighlighter is a LibSyntax highlighter.
target_link_libraries(LibJS PRIVATE LibSyntax)

# Link LibUnicode publicly to ensure ICU data (which is in libicudata.a) is available in any process using LibJS.
target_link_libraries(LibJS PUBLIC LibUnicode)

target_link_libraries(LibJS PUBLIC JSClangPlugin)

if (NOT BUILD_SHARED_LIBS OR WIN32)
    target_link_libraries(LibJS PRIVATE libjs_runtime_rust)
elseif (APPLE)
    set(CMAKE_LINK_LIBRARY_USING_load_hidden "LINKER:-load_hidden,<LIBRARY>")
    set(CMAKE_LINK_LIBRARY_USING_load_hidden_SUPPORTED TRUE)
    target_link_libraries(LibJS PRIVATE "$<LINK_LIBRARY:load_hidden,libjs_runtime_rust>")
else()
    target_link_libraries(LibJS PRIVATE libjs_runtime_rust)
    get_target_property(libjs_runtime_rust_archive libjs_runtime_rust IMPORTED_LOCATION)
    cmake_path(GET libjs_runtime_rust_archive FILENAME libjs_runtime_rust_archive_name)
    target_link_options(LibJS PRIVATE "LINKER:--exclude-libs,${libjs_runtime_rust_archive_name}")
endif()
