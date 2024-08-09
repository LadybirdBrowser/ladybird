include(FindPackageHandleStandardArgs)

find_library(libjxl_LIBRARY
    NAMES jxl
)

find_path(libjxl_INCLUDE_PATH
    NAMES jxl/decode.h
)

set(libjxl_COMPILE_OPTIONS "" CACHE STRING "Extra compile options of libjxl")

set(libjxl_LINK_LIBRARIES "" CACHE STRING "Extra link libraries of libjxl")

set(libjxl_LINK_FLAGS "" CACHE STRING "Extra link flags of libjxl")

find_package_handle_standard_args(libjxl
    REQUIRED_VARS libjxl_LIBRARY libjxl_INCLUDE_PATH
)

if (libjxl_FOUND)
    if (NOT TARGET libjxl::libjxl)
        add_library(libjxl::libjxl UNKNOWN IMPORTED)
        set_target_properties(libjxl::libjxl PROPERTIES
            IMPORTED_LOCATION "${libjxl_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${libjxl_INCLUDE_PATH}"
            INTERFACE_COMPILE_OPTIONS "${libjxl_COMPILE_OPTIONS}"
            INTERFACE_LINK_LIBRARIES "${libjxl_LINK_LIBRARIES}"
            INTERFACE_LINK_FLAGS "${libjxl_LINK_FLAGS}"
        )
    endif()
endif()
