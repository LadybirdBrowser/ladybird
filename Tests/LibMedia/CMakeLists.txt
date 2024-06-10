set(TEST_SOURCES
    TestParseMatroska.cpp
    TestVP9Decode.cpp
)

foreach(source IN LISTS TEST_SOURCES)
    serenity_test("${source}" LibMedia LIBS LibMedia)
endforeach()

install(FILES vp9_in_webm.webm DESTINATION usr/Tests/LibMedia)
install(FILES vp9_4k.webm DESTINATION usr/Tests/LibMedia)
install(FILES vp9_clamp_reference_mvs.webm DESTINATION usr/Tests/LibMedia)
install(FILES vp9_oob_blocks.webm DESTINATION usr/Tests/LibMedia)
install(FILES master_elements_containing_crc32.mkv DESTINATION usr/Tests/LibMedia)
install(FILES oss-fuzz-testcase-52630.vp9 DESTINATION usr/Tests/LibMedia)
install(FILES oss-fuzz-testcase-53977.vp9 DESTINATION usr/Tests/LibMedia)
install(FILES oss-fuzz-testcase-62054.vp9 DESTINATION usr/Tests/LibMedia)
install(FILES oss-fuzz-testcase-63182.vp9 DESTINATION usr/Tests/LibMedia)
