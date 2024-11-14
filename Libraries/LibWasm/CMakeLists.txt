set(SOURCES
    AbstractMachine/AbstractMachine.cpp
    AbstractMachine/BytecodeInterpreter.cpp
    AbstractMachine/Configuration.cpp
    AbstractMachine/Validator.cpp
    Parser/Parser.cpp
    Printer/Printer.cpp
    WASI/Wasi.cpp
)

serenity_lib(LibWasm wasm)
target_link_libraries(LibWasm PRIVATE LibCore LibGC LibJS)

include(wasm_spec_tests)
