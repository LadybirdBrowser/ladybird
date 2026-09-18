function(add_simple_fuzzer name)
  add_executable(${name} "${name}.cpp")

  if (ENABLE_FUZZERS_OSSFUZZ)
    target_link_libraries(${name}
      PUBLIC ${ARGN} AK LibCore
      PRIVATE ${FUZZING_ENGINE})
    set_target_properties(${name}
      PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FUZZER_OUTPUT_DIRECTORY}")

    foreach(extension dict options)
      if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${name}.${extension}")
        configure_file("${name}.${extension}" "${FUZZER_OUTPUT_DIRECTORY}/${name}.${extension}" COPYONLY)
      endif()
    endforeach()
  elseif (ENABLE_FUZZERS_LIBFUZZER)
    target_compile_options(${name}
      PRIVATE $<$<CXX_COMPILER_ID:Clang>:-g -O1 -fsanitize=fuzzer>
      )
    target_link_libraries(${name}
      PUBLIC ${ARGN} AK LibCore
      PRIVATE $<$<CXX_COMPILER_ID:Clang>:-fsanitize=fuzzer>
      )
  else()
    target_sources(${name} PRIVATE "EntryShim.cpp")
    target_link_libraries(${name} PUBLIC ${ARGN} AK LibCore)
  endif()

  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
    target_compile_options(${name} PRIVATE -Wno-missing-prototypes)
  else()
    target_compile_options(${name} PRIVATE -Wno-missing-declarations)
  endif()
endfunction()

if (ENABLE_FUZZERS_OSSFUZZ)
  if (NOT FUZZER_OUTPUT_DIRECTORY)
    message(FATAL_ERROR "FUZZER_OUTPUT_DIRECTORY must be set for an OSS-Fuzz build")
  endif()
  if (NOT FUZZING_ENGINE)
    message(FATAL_ERROR "FUZZING_ENGINE must be set for an OSS-Fuzz build")
  endif()
  separate_arguments(FUZZING_ENGINE NATIVE_COMMAND "${FUZZING_ENGINE}")
endif()

include(fuzzers.cmake)

set(FUZZER_EXECUTABLES)
foreach(target IN LISTS FUZZER_TARGETS)
  add_simple_fuzzer("Fuzz${target}" "${FUZZER_DEPENDENCIES_${target}}")
  list(APPEND FUZZER_EXECUTABLES "Fuzz${target}")
endforeach()

add_custom_target(fuzzers DEPENDS ${FUZZER_EXECUTABLES})

if (ENABLE_FUZZERS_FUZZILLI)
    add_executable(FuzzilliJs FuzzilliJs.cpp)
    # Do not recursively instrument the callbacks or reward REPRL protocol coverage.
    target_compile_options(FuzzilliJs PRIVATE -g -O1 -fno-sanitize-coverage=trace-pc-guard)
    target_link_libraries(FuzzilliJs PRIVATE AK LibCore LibJS LibGC)
endif()
