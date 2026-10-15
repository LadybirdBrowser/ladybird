# VCPKG NOTE: A minimal version of WebKit's https://github.com/WebKit/WebKit/blob/0742522b24152262b04913242cb0b3c48de92ba0/Source/cmake/WebKitCompilerFlags.cmake
# To support the adapted ANGLE CMake buildsystem

# Checks whether all the given compiler flags are supported by the compiler.
# The _compiler may be either "C" or "CXX", and the result from the check
# will be stored in the variable named by _result.
function(WEBKIT_CHECK_COMPILER_FLAGS _compiler _result)
    string(TOUPPER "${_compiler}" _compiler)
    set(${_result} FALSE PARENT_SCOPE)
    foreach (_flag IN LISTS ARGN)
        # If an equals (=) character is present in a variable name, it will
        # not be cached correctly, and the check will be retried ad nauseam.
        string(REPLACE "=" "__" _cachevar "${_compiler}_COMPILER_SUPPORTS_${_flag}")
        if (${_compiler} STREQUAL CXX)
            check_cxx_compiler_flag("${_flag}" "${_cachevar}")
        elseif (${_compiler} STREQUAL C)
            check_c_compiler_flag("${_flag}" "${_cachevar}")
        else ()
            set(${_cachevar} FALSE CACHE INTERNAL "" FORCE)
            message(WARNING "WEBKIT_CHECK_COMPILER_FLAGS: unknown compiler '${_compiler}'")
            return()
        endif ()
        if (NOT ${_cachevar})
            return()
        endif ()
    endforeach ()
    set(${_result} TRUE PARENT_SCOPE)
endfunction()


# Appends flags to COMPILE_OPTIONS of _subject if supported by the C
# or CXX _compiler. The _subject argument depends on its _kind, it may be
# a target name (with TARGET as _kind), or a path (with SOURCE or DIRECTORY
# as _kind).
function(WEBKIT_ADD_COMPILER_FLAGS _compiler _kind _subject)
    foreach (_flag IN LISTS ARGN)
        WEBKIT_CHECK_COMPILER_FLAGS(${_compiler} flag_supported "${_flag}")
        if (flag_supported)
            set_property(${_kind} ${_subject} APPEND PROPERTY COMPILE_OPTIONS "${_flag}")
        endif ()
    endforeach ()
endfunction()

# Appends flags to COMPILE_FLAGS of _target if supported by the C compiler.
# Note that it is simply not possible to pass different C and C++ flags, unless
# we drop support for the Visual Studio backend and use the COMPILE_LANGUAGE
# generator expression. This is a very serious limitation.
macro(WEBKIT_ADD_TARGET_C_FLAGS _target)
    WEBKIT_ADD_COMPILER_FLAGS(C TARGET ${_target} ${ARGN})
endmacro()

# Appends flags to COMPILE_FLAGS of _target if supported by the C++ compiler.
# Note that it is simply not possible to pass different C and C++ flags, unless
# we drop support for the Visual Studio backend and use the COMPILE_LANGUAGE
# generator expression. This is a very serious limitation.
macro(WEBKIT_ADD_TARGET_CXX_FLAGS _target)
    WEBKIT_ADD_COMPILER_FLAGS(CXX TARGET ${_target} ${ARGN})
endmacro()
