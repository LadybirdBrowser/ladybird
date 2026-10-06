# The LibJS target is the C++ API that LibJS's users build against, implemented over the Rust runtime's embedding ABI.
# It links the runtime with the runtime's symbols hidden, so it exports only its own JS_API functions, and every process
# that links LibJS has exactly one copy of the runtime.

add_subdirectory(Rust/Runtime)

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
    Runtime/Reference.cpp
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
