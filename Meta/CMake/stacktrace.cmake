#
# Provides definitions for stack trace support via libbacktrace or std::stacktrace
#

include_guard()

find_package(Backtrace)
include(CheckCXXSourceCompiles)

function(check_std_stacktrace link_lib library_target out_var)
    set(CMAKE_REQUIRED_LIBRARIES ${link_lib})
    set(check_var HAVE_STD_STACKTRACE_CHECK)
    if (link_lib)
        set(check_var "HAVE_STD_STACKTRACE_WITH_${link_lib}")
    endif()
    check_cxx_source_compiles("
        #include <version>
        #include <stacktrace>
        #include <iostream>
        #if !defined(__cpp_lib_stacktrace) || (__cpp_lib_stacktrace < 202011L)
        #    error \"No std::stacktrace available\"
        #endif
        int main() {
            std::cout << std::stacktrace::current() << std::endl;
            return 0;
        }"
        ${check_var}
    )
    set(${out_var} ${${check_var}})
    if (${out_var})
        target_link_libraries(${library_target} PRIVATE "${link_lib}")
    endif()
    return(PROPAGATE ${out_var})
endfunction()

function(link_stacktrace_library target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "STD_DEFINITION" "")

    if (Backtrace_FOUND AND NOT ENABLE_STD_STACKTRACE)
        if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.30)
            target_link_libraries(${target} PRIVATE Backtrace::Backtrace)
        else()
            target_include_directories(${target} PRIVATE ${Backtrace_INCLUDE_DIRS})
            target_link_libraries(${target} PRIVATE ${Backtrace_LIBRARIES})
        endif()
    else()
        check_std_stacktrace("" ${target} HAVE_STD_STACKTRACE)

        if(NOT HAVE_STD_STACKTRACE AND CMAKE_CXX_COMPILER_ID STREQUAL GNU)
            if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 14.1)
                check_std_stacktrace("stdc++exp" ${target} HAVE_STD_STACKTRACE)
            else()
                check_std_stacktrace("stdc++_libbacktrace" ${target} HAVE_STD_STACKTRACE)
            endif()
        endif()

        if (NOT HAVE_STD_STACKTRACE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
            foreach(lib IN ITEMS "stdc++exp" "stdc++_libbacktrace" "c++experimental" )
                check_std_stacktrace("${lib}" ${target} HAVE_STD_STACKTRACE)
                if(HAVE_STD_STACKTRACE)
                    break()
                endif()
            endforeach()
        endif()

        if(HAVE_STD_STACKTRACE)
            target_compile_definitions(${target} PRIVATE ${ARG_STD_DEFINITION})
        else()
            set(msg_level WARNING)
            if (ENABLE_STD_STACKTRACE)
                set(msg_level FATAL_ERROR)
            endif()
            message(${msg_level} "Backtrace and <stacktrace> not found, stack traces will be unavailable")
        endif()
    endif()
endfunction()
