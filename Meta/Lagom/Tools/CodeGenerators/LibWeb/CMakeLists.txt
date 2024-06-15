set(SOURCES "") # avoid pulling SOURCES from parent scope

lagom_tool(GenerateCSSEnums                 SOURCES GenerateCSSEnums.cpp LIBS LibMain)
lagom_tool(GenerateCSSMathFunctions         SOURCES GenerateCSSMathFunctions.cpp LIBS LibMain)
lagom_tool(GenerateCSSMediaFeatureID        SOURCES GenerateCSSMediaFeatureID.cpp LIBS LibMain)
lagom_tool(GenerateCSSPropertyID            SOURCES GenerateCSSPropertyID.cpp LIBS LibMain)
lagom_tool(GenerateCSSPseudoClass           SOURCES GenerateCSSPseudoClass.cpp LIBS LibMain)
lagom_tool(GenerateCSSTransformFunctions    SOURCES GenerateCSSTransformFunctions.cpp LIBS LibMain)
lagom_tool(GenerateCSSValueID               SOURCES GenerateCSSValueID.cpp LIBS LibMain)
lagom_tool(GenerateWindowOrWorkerInterfaces SOURCES GenerateWindowOrWorkerInterfaces.cpp LIBS LibMain LibIDL)
lagom_tool(GenerateAriaRoles                SOURCES GenerateAriaRoles.cpp LIBS LibMain)

add_subdirectory(BindingsGenerator)
