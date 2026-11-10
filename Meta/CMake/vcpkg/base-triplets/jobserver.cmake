# NOTE: This must not set any variables in the including scope, as the triplet's variables are part of every port's ABI.

if(NOT COMMAND vcpkg_execute_build_process)
    return()
endif()

if(COMMAND z_ladybird_build_jobs_come_from_jobserver)
    return()
endif()
function(z_ladybird_build_jobs_come_from_jobserver)
endfunction()

function(z_ladybird_make_takes_jobserver_fifo out_var)
    set(takes_fifo OFF)
    if(DEFINED Z_VCPKG_MAKE)
        execute_process(
            COMMAND "${Z_VCPKG_MAKE}" --version
            OUTPUT_VARIABLE version_output
            ERROR_QUIET
            RESULT_VARIABLE result
        )
        # GNU Make takes a jobserver FIFO from 4.4 on.
        if(result EQUAL 0 AND version_output MATCHES "^GNU Make ([0-9]+\\.[0-9]+)" AND CMAKE_MATCH_1 VERSION_GREATER_EQUAL 4.4)
            set(takes_fifo ON)
        endif()
    endif()
    set("${out_var}" "${takes_fifo}" PARENT_SCOPE)
endfunction()

function(z_ladybird_ninja_takes_jobserver_fifo out_var)
    set(takes_fifo OFF)
    vcpkg_find_acquire_program(NINJA)
    execute_process(
        COMMAND "${NINJA}" --version
        OUTPUT_VARIABLE version_output
        ERROR_QUIET
        RESULT_VARIABLE result
    )
    # Ninja joins a jobserver from 1.13 on.
    if(result EQUAL 0 AND version_output MATCHES "^([0-9]+\\.[0-9]+)" AND CMAKE_MATCH_1 VERSION_GREATER_EQUAL 1.13)
        set(takes_fifo ON)
    endif()
    set("${out_var}" "${takes_fifo}" PARENT_SCOPE)
endfunction()

# Meta/Utils/vcpkg_deps.py runs each port as a client of its jobserver, which vcpkg's explicit -j would opt out of.
function(vcpkg_execute_build_process)
    if(NOT "$ENV{MAKEFLAGS}" MATCHES "--jobserver-auth=fifo:")
        _vcpkg_execute_build_process(${ARGV})
        return()
    endif()

    set(ninja_takes_fifo OFF)
    if("${Z_VCPKG_CMAKE_GENERATOR}" STREQUAL "Ninja")
        z_ladybird_ninja_takes_jobserver_fifo(ninja_takes_fifo)
    endif()
    z_ladybird_make_takes_jobserver_fifo(make_takes_fifo)

    set(jobs "${VCPKG_CONCURRENCY}")
    set(arguments "")
    set(dropped_jobs OFF)
    set(section "")
    list(LENGTH ARGV argument_count)
    set(index 0)
    while(index LESS argument_count)
        list(GET ARGV ${index} argument)
        math(EXPR index "${index} + 1")

        if(argument MATCHES "^(COMMAND|NO_PARALLEL_COMMAND|WORKING_DIRECTORY|LOGNAME|SAVE_LOG_FILES)$")
            set(section "${argument}")
        elseif(section STREQUAL "COMMAND")
            # vcpkg_cmake_build passes -jN to ninja.
            if(ninja_takes_fifo AND argument STREQUAL "-j${jobs}")
                set(dropped_jobs ON)
                continue()
            endif()
            if(make_takes_fifo)
                # vcpkg_build_make passes -j N to make.
                if(argument STREQUAL "-j" AND index LESS argument_count)
                    list(GET ARGV ${index} next_argument)
                    if(next_argument STREQUAL jobs)
                        math(EXPR index "${index} + 1")
                        set(dropped_jobs ON)
                        continue()
                    endif()
                endif()
                # vcpkg_make_install passes its make command line as a single shell string.
                string(REGEX REPLACE " -j ${jobs}( |$)" "\\1" stripped_argument "${argument}")
                if(NOT stripped_argument STREQUAL argument)
                    set(argument "${stripped_argument}")
                    set(dropped_jobs ON)
                endif()
            endif()
        endif()
        list(APPEND arguments "${argument}")
    endwhile()

    if(dropped_jobs)
        message(STATUS "Taking build jobs from the jobserver")
    endif()
    _vcpkg_execute_build_process(${arguments})
endfunction()
