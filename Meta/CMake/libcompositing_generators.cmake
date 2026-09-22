function (generate_compositing_webgl_implementation)
    set(LIBCOMPOSITING_INPUT_FOLDER "${CMAKE_CURRENT_SOURCE_DIR}")

    invoke_py_generator(
        "GLFunctions.cpp"
        "generate_libweb_webgl_functions.py"
        "${LIBCOMPOSITING_INPUT_FOLDER}/WebGL/GLFunctions.json"
        "WebGL/GLFunctions.h"
        "WebGL/GLFunctions.cpp"
        arguments -j "${LIBCOMPOSITING_INPUT_FOLDER}/WebGL/GLFunctions.json"
        dependencies "${LADYBIRD_SOURCE_DIR}/Meta/Generators/libweb_webgl.py"
    )

    invoke_py_generator(
        "WebGLCommands.cpp"
        "generate_libweb_webgl_commands.py"
        "${LIBCOMPOSITING_INPUT_FOLDER}/WebGL/GLFunctions.json"
        "WebGL/WebGLCommands.h"
        "WebGL/WebGLCommands.cpp"
        arguments -j "${LIBCOMPOSITING_INPUT_FOLDER}/WebGL/GLFunctions.json"
        dependencies "${LADYBIRD_SOURCE_DIR}/Meta/Generators/libweb_webgl.py"
    )

    set(WEBGL_GENERATED_HEADERS
       "WebGL/GLFunctions.h"
       "WebGL/WebGLCommands.h"
    )
    list(TRANSFORM WEBGL_GENERATED_HEADERS PREPEND "${CMAKE_CURRENT_BINARY_DIR}/")
    if (ENABLE_INSTALL_HEADERS)
        install(FILES ${WEBGL_GENERATED_HEADERS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/LibCompositing/WebGL")
    endif()
endfunction()
