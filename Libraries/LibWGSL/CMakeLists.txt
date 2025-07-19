set(SOURCES
    AST.cpp
    Compiler.cpp
    Lexer.cpp
    Parser.cpp
    Preprocessor.cpp
)

find_package(SPIRV-Tools CONFIG REQUIRED)
find_package(SPIRV-Tools-opt CONFIG REQUIRED)
find_package(spirv_cross_core CONFIG REQUIRED)
find_package(spirv_cross_glsl CONFIG REQUIRED)
find_package(spirv_cross_msl CONFIG REQUIRED)
find_package(spirv_cross_hlsl CONFIG REQUIRED)
find_package(spirv_cross_cpp CONFIG REQUIRED)
find_package(spirv_cross_reflect CONFIG REQUIRED)
find_package(spirv_cross_c CONFIG REQUIRED)
ladybird_lib(LibWGSL wgsl EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibWGSL
    PRIVATE
        AK LibUnicode
        SPIRV-Tools-static SPIRV-Tools-opt
        spirv-cross-core spirv-cross-glsl spirv-cross-msl spirv-cross-hlsl spirv-cross-c
)
