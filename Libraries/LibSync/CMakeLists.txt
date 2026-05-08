if (WIN32)
    set(SOURCES MutexWindows.cpp ConditionVariableWindows.cpp RWLockWindows.cpp)
else()
    set(SOURCES MutexPOSIX.cpp ConditionVariablePOSIX.cpp RWLockPOSIX.cpp)
endif()

ladybird_lib(LibSync sync EXPLICIT_SYMBOL_EXPORT)
