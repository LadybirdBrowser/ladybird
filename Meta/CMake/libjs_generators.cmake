function(generate_bytecode_def_derived)
    set(LIBJS_INPUT_FOLDER "${CMAKE_CURRENT_SOURCE_DIR}")
    invoke_py_generator(
            "Op.cpp"
            "generate-libjs-bytecode-def-derived.py"
            "${LIBJS_INPUT_FOLDER}/Bytecode/Bytecode.def"
            "Bytecode/Op.h"
            "Bytecode/Op.cpp"
            EXTRA_HEADER "Bytecode/OpCodes.h"
            arguments -i "${LIBJS_INPUT_FOLDER}/Bytecode/Bytecode.def"
    )
endfunction()
