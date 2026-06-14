if (APPLE)
    set(SOURCES
        DisplaySleepInhibitorIOKit.cpp
    )
else()
    set(SOURCES
        DisplaySleepInhibitorNoOp.cpp
    )
endif()

ladybird_lib(LibWakeLock wakelock)

if (APPLE)
    target_link_libraries(LibWakeLock PRIVATE "-framework CoreFoundation" "-framework IOKit")
endif()
