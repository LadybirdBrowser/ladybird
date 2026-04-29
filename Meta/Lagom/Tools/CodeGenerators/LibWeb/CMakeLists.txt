set(SOURCES "") # avoid pulling SOURCES from parent scope

lagom_tool(GenerateNamedCharacterReferences SOURCES GenerateNamedCharacterReferences.cpp LIBS LibMain)

add_subdirectory(BindingsGenerator)
