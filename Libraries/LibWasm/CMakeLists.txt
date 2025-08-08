set(SOURCES
    AbstractMachine/AbstractMachine.cpp
    AbstractMachine/BytecodeInterpreter.cpp
    AbstractMachine/Configuration.cpp
    AbstractMachine/Validator.cpp
    Parser/Parser.cpp
    Printer/Printer.cpp
)

# FIXME: Add Windows support
if (NOT WIN32)
    list(APPEND SOURCES WASI/Wasi.cpp)
endif()

ladybird_lib(LibWasm wasm EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibWasm PRIVATE LibCore)

include(wasm_spec_tests)
