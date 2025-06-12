include(ProcessorCount)
ProcessorCount(N)
# Executing echo here allows us to print to the standard output,
# to separate the processor count from potential errors
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${N}")
