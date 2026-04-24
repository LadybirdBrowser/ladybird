set(SOURCES "") # avoid pulling SOURCES from parent scope

lagom_tool(GenerateWindowOrWorkerInterfaces SOURCES GenerateWindowOrWorkerInterfaces.cpp LIBS LibMain LibIDL)
lagom_tool(GenerateAriaRoles                SOURCES GenerateAriaRoles.cpp LIBS LibMain)
lagom_tool(GenerateNamedCharacterReferences SOURCES GenerateNamedCharacterReferences.cpp LIBS LibMain)

add_subdirectory(BindingsGenerator)
