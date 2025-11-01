if (WIN32)
    set(SOURCES MutexWindows.cpp ConditionVariableWindows.cpp RWLockWindows.cpp)
else()
    set(SOURCES ConditionVariable.cpp RWLock.cpp)
endif()

ladybird_lib(LibSync sync EXPLICIT_SYMBOL_EXPORT)
