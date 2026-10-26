# LibJS's C++ API, which LibJS's users build against, is implemented over the embedding ABI of the Rust runtime. When
# LibWeb is built, the API's objects and the runtime go into LibWeb's library, whose Rust crate builds the runtime, so
# that a process has one copy of the runtime and LibWeb's Rust code depends on it directly. Without LibWeb, LibJS is a
# library of its own, which exports only its own JS_API functions and keeps the runtime's symbols hidden.

# The features the runtime is built with, by LibJS's own crate build or by LibWeb's crate.
set(LIBJS_RUST_FEATURES allocator)
if (ENABLE_ADDRESS_SANITIZER)
    list(APPEND LIBJS_RUST_FEATURES address-sanitizer)
endif()
if (ENABLE_THREAD_SANITIZER)
    list(APPEND LIBJS_RUST_FEATURES thread-sanitizer)
endif()
# The JIT is a default feature of the crate, so that cargo builds and lints it unless told otherwise.
if (ENABLE_JS_JIT)
    list(APPEND LIBJS_RUST_FEATURES jit)
endif()
set(LIBJS_RUST_FEATURES ${LIBJS_RUST_FEATURES} PARENT_SCOPE)

add_subdirectory(Rust)

# The optimizing JIT's compiler, a pure Rust crate that only works on plain data. Its unit tests run without the rest of
# LibJS.
if (ENABLE_JS_JIT)
    test_rust_crate(MANIFEST_PATH JIT/Rust/Cargo.toml CRATE_NAME libjs_jit)
endif()

set(SOURCES
    Console.cpp
    CyclicModule.cpp
    Debugger.cpp
    DecodedBytecodeCache.cpp
    Heap/Cell.cpp
    Module.cpp
    ParserError.cpp
    Print.cpp
    Runtime/AbstractOperations.cpp
    Runtime/Agent.cpp
    Runtime/Array.cpp
    Runtime/ArrayBuffer.cpp
    Runtime/BigInt.cpp
    Runtime/BigIntObject.cpp
    Runtime/BooleanObject.cpp
    Runtime/Completion.cpp
    Runtime/ConsoleObject.cpp
    Runtime/DataView.cpp
    Runtime/Date.cpp
    Runtime/DeclarativeEnvironment.cpp
    Runtime/Environment.cpp
    Runtime/Error.cpp
    Runtime/ErrorData.cpp
    Runtime/ErrorTypes.cpp
    Runtime/ExecutionContext.cpp
    Runtime/FinalizationRegistry.cpp
    Runtime/FunctionEnvironment.cpp
    Runtime/FunctionObject.cpp
    Runtime/GlobalEnvironment.cpp
    Runtime/HostArray.cpp
    Runtime/HostFunction.cpp
    Runtime/HostModule.cpp
    Runtime/HostObject.cpp
    Runtime/Intrinsics.cpp
    Runtime/Iterator.cpp
    Runtime/JobCallback.cpp
    Runtime/JSONObject.cpp
    Runtime/Map.cpp
    Runtime/MapIterator.cpp
    Runtime/ModuleEnvironment.cpp
    Runtime/NativeFunction.cpp
    Runtime/NumberObject.cpp
    Runtime/Object.cpp
    Runtime/ObjectEnvironment.cpp
    Runtime/PrimitiveString.cpp
    Runtime/Promise.cpp
    Runtime/PromiseCapability.cpp
    Runtime/PromiseJob.cpp
    Runtime/PropertyDescriptor.cpp
    Runtime/Realm.cpp
    Runtime/RegExpObject.cpp
    Runtime/Set.cpp
    Runtime/SetIterator.cpp
    Runtime/StringObject.cpp
    Runtime/Symbol.cpp
    Runtime/TypedArray.cpp
    Runtime/Value.cpp
    Runtime/VM.cpp
    Script.cpp
    ScriptCompilation.cpp
    SourceCode.cpp
    SourceTextModule.cpp
    SyntaxHighlighter.cpp
    SyntheticModule.cpp
    ToolEntryPoints.cpp
)

# What every target that includes LibJS's headers needs: the clang plugin that checks the code, LibUnicode, so that
# ICU's data is in every process that runs JavaScript, and the generated headers of the embedding ABI, which the public
# headers include, before anything compiles against them.
add_library(LibJSHeaders INTERFACE)
target_link_libraries(LibJSHeaders INTERFACE JSClangPlugin LibUnicode)
add_dependencies(LibJSHeaders libjs_embedding_headers-ffi-headers)
if (NOT BUILD_SHARED_LIBS)
    target_compile_definitions(LibJSHeaders INTERFACE LIBJS_STATIC_DEFINE)
endif()
add_lagom_library_install_rules(LibJSHeaders ALIAS_NAME JSHeaders)

# The objects of the C++ API, compiled once for whichever library they go into. Their export header is generated under
# LibJS's name, so that JS_API means the same wherever they are, and its guard is the define the objects are compiled
# with, as it would be for a shared LibJS.
add_library(LibJSObjects OBJECT ${SOURCES})
ladybird_library_setup(LibJSObjects)
target_link_libraries(LibJSObjects PUBLIC LibJSHeaders)
target_link_libraries(LibJSObjects PRIVATE LibCore LibCrypto LibGC LibSyntax LibTextCodec)
target_compile_definitions(LibJSObjects PRIVATE LibJS_EXPORTS)
set_target_properties(LibJSObjects PROPERTIES DEFINE_SYMBOL LibJS_EXPORTS)
ladybird_generate_export_header(LibJSObjects js BASE_NAME LibJS)

if (LIBJS_FOLDED_INTO_LIBWEB)
    # LibJS is what a target that also uses LibWeb links: LibWeb's library holds the objects and the runtime.
    add_library(LibJS INTERFACE)
    target_link_libraries(LibJS INTERFACE LibWeb LibJSHeaders)
    set_target_properties(LibJS PROPERTIES EXPORT_NAME JS)
    ladybird_install_library_headers(LibJS JS)
    add_lagom_library_install_rules(LibJS ALIAS_NAME JS)

    # Standalone JavaScript tools link the objects and an independent runtime archive, so building them does not
    # build LibWeb's Rust code or generate its bindings.
    add_library(LibJSStandalone INTERFACE)
    target_link_libraries(LibJSStandalone INTERFACE
        $<TARGET_OBJECTS:LibJSObjects>
        LibJSHeaders libjs_rust AK LibCore LibCrypto LibGC LibSyntax LibTextCodec
    )
    if ((LINUX OR BSD) AND BUILD_SHARED_LIBS)
        # The tool's copy of the runtime, with its Rust standard library, stays out of its dynamic symbol table, as the
        # copy in LibJS's own library does.
        target_link_options(LibJSStandalone INTERFACE
            "LINKER:--exclude-libs,$<TARGET_FILE_NAME:libjs_rust>"
        )
    endif()
else()
    set(SOURCES $<TARGET_OBJECTS:LibJSObjects>)
    ladybird_lib(LibJS js)
    set_target_properties(LibJS PROPERTIES LINKER_LANGUAGE CXX WINDOWS_EXPORT_ALL_SYMBOLS OFF)
    target_link_libraries(LibJS PUBLIC LibJSHeaders)
    target_link_libraries(LibJS PRIVATE LibCore LibCrypto LibGC LibSyntax LibTextCodec)

    if (NOT BUILD_SHARED_LIBS OR WIN32)
        target_link_libraries(LibJS PRIVATE libjs_rust)
    elseif (APPLE)
        set(CMAKE_LINK_LIBRARY_USING_load_hidden "LINKER:-load_hidden,<LIBRARY>")
        set(CMAKE_LINK_LIBRARY_USING_load_hidden_SUPPORTED TRUE)
        target_link_libraries(LibJS PRIVATE "$<LINK_LIBRARY:load_hidden,libjs_rust>")
    else()
        target_link_libraries(LibJS PRIVATE libjs_rust)
        get_target_property(libjs_rust_archive libjs_rust IMPORTED_LOCATION)
        cmake_path(GET libjs_rust_archive FILENAME libjs_rust_archive_name)
        target_link_options(LibJS PRIVATE "LINKER:--exclude-libs,${libjs_rust_archive_name}")
    endif()

    add_library(LibJSStandalone INTERFACE)
    target_link_libraries(LibJSStandalone INTERFACE LibJS)
endif()
