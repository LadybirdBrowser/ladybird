include(${SerenityOS_SOURCE_DIR}/Meta/CMake/unicode_data.cmake)

set(SOURCES
    CharacterTypes.cpp
    CurrencyCode.cpp
    Emoji.cpp
    IDNA.cpp
    Normalize.cpp
    String.cpp
    ${UNICODE_DATA_SOURCES}
)
set(GENERATED_SOURCES ${CURRENT_LIB_GENERATED})

serenity_lib(LibUnicode unicode)

find_package(ICU REQUIRED COMPONENTS data uc)
target_include_directories(LibUnicode PRIVATE ${ICU_INCLUDE_DIRS})
target_link_libraries(LibUnicode PUBLIC ${ICU_LIBRARIES})
