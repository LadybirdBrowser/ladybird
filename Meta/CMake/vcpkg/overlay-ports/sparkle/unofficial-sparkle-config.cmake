get_filename_component(_sparkle_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

if(NOT TARGET unofficial::sparkle::Sparkle)
    add_library(unofficial::sparkle::Sparkle SHARED IMPORTED)
    set_target_properties(unofficial::sparkle::Sparkle PROPERTIES
        FRAMEWORK TRUE
        IMPORTED_LOCATION "${_sparkle_prefix}/Frameworks/Sparkle.framework"
        INTERFACE_INCLUDE_DIRECTORIES "${_sparkle_prefix}/Frameworks/Sparkle.framework"
    )
endif()

unset(_sparkle_prefix)
