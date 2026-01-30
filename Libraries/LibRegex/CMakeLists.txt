set(SOURCES
    RegexByteCode.cpp
    RegexLexer.cpp
    RegexMatcher.cpp
    RegexOptimizer.cpp
    RegexParser.cpp
)

if(SERENITYOS)
    list(APPEND SOURCES C/Regex.cpp)
endif()

ladybird_lib(LibRegex regex EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibRegex PRIVATE LibUnicode)

set_target_properties(LibRegex PROPERTIES UNITY_BUILD ${LADYBIRD_ENABLE_UNITY_BUILD})
