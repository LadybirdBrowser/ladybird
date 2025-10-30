if (WIN32)
    set(SOURCES MutexWindows.cpp ConditionVariableWindows.cpp)
else()
    set(SOURCES ConditionVariable.cpp)
endif()

ladybird_lib(LibSync sync EXPLICIT_SYMBOL_EXPORT)
