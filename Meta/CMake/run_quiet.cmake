math(EXPR _last "${CMAKE_ARGC} - 1")
set(CMD "")
set(_seen FALSE)
foreach(i RANGE 0 ${_last})
  if(_seen)
    list(APPEND CMD "${CMAKE_ARGV${i}}")
  elseif(CMAKE_ARGV${i} STREQUAL "--")
    set(_seen TRUE)
  endif()
endforeach()

execute_process(
  COMMAND ${CMD}
  OUTPUT_FILE ${LOG}
  ERROR_FILE  ${LOG}
  RESULT_VARIABLE rc
)
if(NOT rc EQUAL 0)
  if(EXISTS "${LOG}")
    file(READ "${LOG}" out)
    message("${out}")
  endif()
  message(FATAL_ERROR "exec failed (exit ${rc})")
endif()
