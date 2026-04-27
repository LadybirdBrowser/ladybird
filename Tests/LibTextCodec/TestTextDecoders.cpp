/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibTextCodec/Decoder.h>

static ReadonlyBytes bytes(Vector<u8> const& data)
{
    return data.span();
}

static TextCodec::Decoder& decoder_for(StringView encoding)
{
    auto decoder = TextCodec::decoder_for(encoding);
    VERIFY(decoder.has_value());
    return decoder.value();
}

TEST_CASE(test_utf8_decode)
{
    auto decoder = TextCodec::UTF8Decoder();
    // Bytes for U+1F600 GRINNING FACE
    auto test_string = "\xf0\x9f\x98\x80"sv;

    EXPECT(decoder.validate(test_string));

    Vector<u32> processed_code_points;
    MUST(decoder.process(test_string, [&](u32 code_point) {
        return processed_code_points.try_append(code_point);
    }));
    EXPECT(processed_code_points.size() == 1);
    EXPECT(processed_code_points[0] == 0x1F600);

    EXPECT(MUST(decoder.to_utf8(test_string)) == test_string);
}

TEST_CASE(test_utf16be_decode)
{
    auto decoder = TextCodec::UTF16BEDecoder();
    // This is the output of `python3 -c "print('säk😀'.encode('utf-16be'))"`.
    auto test_string = "\x00s\x00\xe4\x00k\xd8=\xde\x00"sv;

    EXPECT(decoder.validate(test_string));
    auto utf8 = MUST(decoder.to_utf8(test_string));
    EXPECT_EQ(utf8, "säk😀"sv);
}

TEST_CASE(test_streaming_decoder_utf8_mid_sequence)
{
    auto& decoder = decoder_for("UTF-8"sv);
    auto streaming_decoder = TextCodec::StreamingDecoder { decoder };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 'a', 0xc3 }))), "a"sv);
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0xa9, 'b' }))), "éb"sv);
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_finishes_incomplete_sequence)
{
    auto& decoder = decoder_for("UTF-8"sv);
    auto streaming_decoder = TextCodec::StreamingDecoder { decoder };

    auto incomplete_sequence = Vector<u8> { 0xc3 };
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes(incomplete_sequence))), ""sv);
    EXPECT_EQ(MUST(streaming_decoder.finish()), MUST(decoder.to_utf8(StringView(bytes(incomplete_sequence)))));
}

TEST_CASE(test_streaming_decoder_utf16_odd_byte)
{
    auto& decoder = decoder_for("UTF-16LE"sv);
    auto streaming_decoder = TextCodec::StreamingDecoder { decoder };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0x41, 0x00, 0x42 }))), "A"sv);
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0x00 }))), "B"sv);
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_utf16_surrogate_pair_split)
{
    auto& decoder = decoder_for("UTF-16BE"sv);
    auto streaming_decoder = TextCodec::StreamingDecoder { decoder };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0x00, 0x41, 0xd8, 0x3d }))), "A"sv);
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0xde, 0x00 }))), "😀"sv);
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_gb18030_four_byte_tail)
{
    auto& decoder = decoder_for("gb18030"sv);
    auto streaming_decoder = TextCodec::StreamingDecoder { decoder };
    auto gb18030_sequence = Vector<u8> { 0x81, 0x30, 0x81, 0x30 };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 'a', 0x81, 0x30, 0x81 }))), "a"sv);
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0x30 }))), MUST(decoder.to_utf8(StringView(bytes(gb18030_sequence)))));
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_big5_overlapping_trail)
{
    auto& decoder = decoder_for("Big5"sv);
    auto streaming_decoder = TextCodec::StreamingDecoder { decoder };
    auto big5_sequence = Vector<u8> { 0xa4, 0xa4 };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes(big5_sequence))), MUST(decoder.to_utf8(StringView(bytes(big5_sequence)))));
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);

    auto streaming_decoder_for_split_input = TextCodec::StreamingDecoder { decoder };
    EXPECT_EQ(MUST(streaming_decoder_for_split_input.to_utf8(bytes({ 0xa4 }))), ""sv);
    EXPECT_EQ(MUST(streaming_decoder_for_split_input.to_utf8(bytes({ 0xa4 }))), MUST(decoder.to_utf8(StringView(bytes(big5_sequence)))));
    EXPECT_EQ(MUST(streaming_decoder_for_split_input.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_euc_jp_three_byte_tail)
{
    auto& decoder = decoder_for("EUC-JP"sv);
    auto streaming_decoder = TextCodec::StreamingDecoder { decoder };
    auto euc_jp_sequence = Vector<u8> { 0x8f, 0xa2, 0xaf };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0x8f, 0xa2 }))), ""sv);
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0xaf }))), MUST(decoder.to_utf8(StringView(bytes(euc_jp_sequence)))));
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_shift_jis_tail)
{
    auto& decoder = decoder_for("Shift_JIS"sv);
    auto streaming_decoder = TextCodec::StreamingDecoder { decoder };
    auto shift_jis_sequence = Vector<u8> { 0x82, 0xa0 };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0x82 }))), ""sv);
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0xa0 }))), MUST(decoder.to_utf8(StringView(bytes(shift_jis_sequence)))));
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_utf16le_decode)
{
    auto decoder = TextCodec::UTF16LEDecoder();
    // This is the output of `python3 -c "print('säk😀'.encode('utf-16le'))"`.
    auto test_string = "s\x00\xe4\x00k\x00=\xd8\x00\xde"sv;

    EXPECT(decoder.validate(test_string));
    auto utf8 = MUST(decoder.to_utf8(test_string));
    EXPECT_EQ(utf8, "säk😀"sv);
}
