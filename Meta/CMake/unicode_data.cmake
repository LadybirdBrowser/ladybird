include(${CMAKE_CURRENT_LIST_DIR}/utils.cmake)

set(EMOJI_RES_PATH "${SerenityOS_SOURCE_DIR}/Base/res/emoji")
set(EMOJI_FILE_LIST_PATH "${SerenityOS_SOURCE_DIR}/Meta/emoji-file-list.txt")

set(EMOJI_DATA_HEADER EmojiData.h)
set(EMOJI_DATA_IMPLEMENTATION EmojiData.cpp)

invoke_generator(
    "EmojiData"
    Lagom::GenerateEmojiData
    "${EMOJI_FILE_LIST_PATH}"
    "${EMOJI_DATA_HEADER}"
    "${EMOJI_DATA_IMPLEMENTATION}"
    arguments -f "${EMOJI_FILE_LIST_PATH}" -r "${EMOJI_RES_PATH}"

    # This will make this command only run when the modified time of the directory changes,
    # which only happens if files within it are added or deleted, but not when a file is modified.
    # This is fine for this use-case, because the contents of a file changing should not affect
    # the generated emoji data.
    dependencies "${EMOJI_RES_PATH}"
)

set(UNICODE_DATA_SOURCES
    ${EMOJI_DATA_HEADER}
    ${EMOJI_DATA_IMPLEMENTATION}
)
