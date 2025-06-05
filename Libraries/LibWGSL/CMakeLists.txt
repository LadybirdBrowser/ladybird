set(SOURCES
    AST.cpp
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
serenity_lib(LibWGSL wgsl EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibWGSL
    PRIVATE
        AK
        SPIRV-Tools-static SPIRV-Tools-opt
        spirv-cross-core spirv-cross-glsl spirv-cross-msl spirv-cross-hlsl
)
