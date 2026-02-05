# A script to apply a Git patch unless it was already applied
find_package(Git REQUIRED QUIET)

execute_process(
  ERROR_VARIABLE discarded RESULT_VARIABLE patch_not_yet_applied
  COMMAND ${GIT_EXECUTABLE} apply --reverse --check ${CMAKE_ARGV3})

if(patch_not_yet_applied)
  execute_process(
    ERROR_VARIABLE discarded
    COMMAND ${GIT_EXECUTABLE} apply  ${CMAKE_ARGV3} COMMAND_ERROR_IS_FATAL LAST
  )
endif()
