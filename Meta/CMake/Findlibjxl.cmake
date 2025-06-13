
#  SDL_image:  An example image loading library for use with SDL
#  Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>
#
#  This software is provided 'as-is', without any express or implied
#  warranty.  In no event will the authors be held liable for any damages
#  arising from the use of this software.
#
#  Permission is granted to anyone to use this software for any purpose,
#  including commercial applications, and to alter it and redistribute it
#  freely, subject to the following restrictions:
#
#  1. The origin of this software must not be misrepresented; you must not
#     claim that you wrote the original software. If you use this software
#     in a product, an acknowledgment in the product documentation would be
#     appreciated but is not required.
#  2. Altered source versions must be plainly marked as such, and must not be
#     misrepresented as being the original software.
#  3. This notice may not be removed or altered from any source distribution.

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
