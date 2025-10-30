if (WIN32)
    set(SOURCES MutexWindows.cpp ConditionVariableWindows.cpp)
else()
    set(SOURCES MutexPOSIX.cpp ConditionVariablePOSIX.cpp)
endif()

ladybird_lib(LibSync sync EXPLICIT_SYMBOL_EXPORT)
