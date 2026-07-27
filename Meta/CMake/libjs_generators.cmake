function(generate_libjs_bytecode)
    set(bytecode_source "${CMAKE_CURRENT_SOURCE_DIR}/Interpreter/interpreter.flap")
    set(op_header "Bytecode/Op.h")
    set(opcodes_header "Bytecode/OpCodes.h")
    add_custom_command(
        OUTPUT "${op_header}" "${opcodes_header}"
        COMMAND "${BYTECODE_GENERATOR_BIN}"
            --input "${bytecode_source}"
            --op-header "${op_header}.tmp"
            --opcodes-header "${opcodes_header}.tmp"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${op_header}.tmp" "${op_header}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${opcodes_header}.tmp" "${opcodes_header}"
        COMMAND "${CMAKE_COMMAND}" -E remove
            "${op_header}.tmp" "${opcodes_header}.tmp"
        DEPENDS "${BYTECODE_GENERATOR_BIN}" "${bytecode_source}"
        VERBATIM
    )
    add_custom_target(generate_Op.h DEPENDS "${op_header}" "${opcodes_header}")
    add_dependencies(ladybird_codegen_accumulator generate_Op.h)
endfunction()
