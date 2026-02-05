set(SOURCES
    IDLGenerators.cpp
    main.cpp
)

lagom_tool(BindingsGenerator LIBS LibIDL LibMain)
target_compile_options(BindingsGenerator PUBLIC -g)
