add_executable(ProcessReaper main.cpp)
target_include_directories(ProcessReaper PRIVATE ${LADYBIRD_SOURCE_DIR}/Libraries)
target_link_libraries(ProcessReaper PRIVATE bsm)

ladybird_sign_helper_process(ProcessReaper)
