# With LIBJS_RUNTIME=Rust, LibJS's users compile against the LibJS facade over the Rust runtime. The facade's headers
# live in Libraries/LibJS/Facade/LibJS at the paths the C++ runtime's headers have in Libraries/LibJS, and come first on
# the include path, so <LibJS/X.h> names the facade's X.h. Every C++ LibJS header that the facade does not provide and
# that both runtimes do not share gets an #error stub in a generated poison root, which also comes before Libraries, so
# that code reaching one fails to compile instead of silently building against the C++ runtime.

set(LIBJS_SOURCE_DIRECTORY "${LADYBIRD_SOURCE_DIR}/Libraries/LibJS")
set(LIBJS_FACADE_DIRECTORY "${LIBJS_SOURCE_DIRECTORY}/Facade")
set(LIBJS_FACADE_POISON_DIRECTORY "${CMAKE_BINARY_DIR}/LibJSPoison")

# Paths relative to Libraries/LibJS of the files that both runtimes use verbatim: the facade builds the sources, and
# the headers stay reachable.
set(libjs_shared_files_list "${LIBJS_FACADE_DIRECTORY}/shared-files.txt")
file(STRINGS "${libjs_shared_files_list}" LIBJS_FILES_SHARED_WITH_CPP_RUNTIME REGEX "^[^#]")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${libjs_shared_files_list}")

file(GLOB_RECURSE libjs_cpp_runtime_headers CONFIGURE_DEPENDS RELATIVE "${LIBJS_SOURCE_DIRECTORY}"
    "${LIBJS_SOURCE_DIRECTORY}/*.h")
list(FILTER libjs_cpp_runtime_headers EXCLUDE REGEX
    "^(Facade|Rust|Flap|ABI|Runtime/JavaScriptImplementations)/")
# The C++ runtime's build generates these, and a Rust-mode build never does.
list(APPEND libjs_cpp_runtime_headers Bytecode/Op.h Bytecode/OpCodes.h RustFFI.h)

file(GLOB_RECURSE libjs_facade_headers CONFIGURE_DEPENDS RELATIVE "${LIBJS_FACADE_DIRECTORY}/LibJS"
    "${LIBJS_FACADE_DIRECTORY}/LibJS/*.h")

set(libjs_poisoned_headers ${libjs_cpp_runtime_headers})
list(REMOVE_ITEM libjs_poisoned_headers ${libjs_facade_headers} ${LIBJS_FILES_SHARED_WITH_CPP_RUNTIME})

foreach(header IN LISTS libjs_poisoned_headers)
    file(CONFIGURE
        OUTPUT "${LIBJS_FACADE_POISON_DIRECTORY}/LibJS/${header}"
        CONTENT "#error \"<LibJS/${header}> is C++-runtime-only; add it to Libraries/LibJS/Facade/LibJS/${header}\"\n"
        @ONLY)
endforeach()

file(GLOB_RECURSE libjs_existing_poison_stubs RELATIVE "${LIBJS_FACADE_POISON_DIRECTORY}/LibJS"
    "${LIBJS_FACADE_POISON_DIRECTORY}/LibJS/*.h")
foreach(stub IN LISTS libjs_existing_poison_stubs)
    if (NOT stub IN_LIST libjs_poisoned_headers)
        file(REMOVE "${LIBJS_FACADE_POISON_DIRECTORY}/LibJS/${stub}")
    endif()
endforeach()

# Each BEFORE prepends, which puts the facade ahead of the poison root.
include_directories(BEFORE "${LIBJS_FACADE_POISON_DIRECTORY}")
include_directories(BEFORE "${LIBJS_FACADE_DIRECTORY}")
