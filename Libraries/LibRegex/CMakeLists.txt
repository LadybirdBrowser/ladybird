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

serenity_lib(LibRegex regex EXPLICIT_SYMBOL_EXPORT)
target_link_libraries(LibRegex PRIVATE LibUnicode)
