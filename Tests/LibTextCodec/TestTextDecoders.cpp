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

static Vector<u32> process_code_points(TextCodec::Decoder& decoder, StringView input)
{
    Vector<u32> code_points;
    MUST(decoder.process_code_points(input, [&](u32 code_point) -> ErrorOr<void> {
        TRY(code_points.try_append(code_point));
        return {};
    }));
    return code_points;
}

TEST_CASE(test_utf8_decode)
{
    auto& decoder = decoder_for("UTF-8"sv);
    // Bytes for U+1F600 GRINNING FACE
    auto test_string = "\xf0\x9f\x98\x80"sv;

    EXPECT(!decoder.to_utf8(test_string, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Fatal).is_error());

    auto processed_code_points = process_code_points(decoder, test_string);
    EXPECT(processed_code_points.size() == 1);
    EXPECT(processed_code_points[0] == 0x1F600);

    EXPECT(MUST(decoder.to_utf8(test_string, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)) == test_string);
}

TEST_CASE(test_utf8_process_code_points)
{
    auto& decoder = decoder_for("UTF-8"sv);
    auto test_string = "A\xf0\x9f\x98\x80"sv;

    EXPECT_EQ(process_code_points(decoder, test_string), (Vector<u32> { 0x41, 0x1F600 }));
}

TEST_CASE(test_utf8_process_code_points_replaces_surrogates)
{
    auto& decoder = decoder_for("UTF-8"sv);
    auto utf8_encoded_surrogate_bytes = Vector<u8> { 0xed, 0xa0, 0x80 };
    auto utf8_encoded_surrogate = StringView(bytes(utf8_encoded_surrogate_bytes));

    EXPECT(decoder.to_utf8(utf8_encoded_surrogate, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Fatal).is_error());
    EXPECT_EQ(MUST(decoder.to_utf8(utf8_encoded_surrogate, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)), "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"sv);
    EXPECT_EQ(process_code_points(decoder, utf8_encoded_surrogate), (Vector<u32> { 0xfffd, 0xfffd, 0xfffd }));
}

TEST_CASE(test_utf8_process_code_points_replaces_truncated_tail_as_single_error)
{
    auto& decoder = decoder_for("UTF-8"sv);
    auto truncated_tail_bytes = Vector<u8> { 0xf0, 0x9f, 0x98 };
    auto truncated_tail = StringView(bytes(truncated_tail_bytes));

    EXPECT(decoder.to_utf8(truncated_tail, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Fatal).is_error());
    EXPECT_EQ(MUST(decoder.to_utf8(truncated_tail, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)), "\xef\xbf\xbd"sv);
    EXPECT_EQ(process_code_points(decoder, truncated_tail), (Vector<u32> { 0xfffd }));
}

TEST_CASE(test_utf8_process_code_points_replaces_overlong_sequences)
{
    auto& decoder = decoder_for("UTF-8"sv);
    auto overlong_null_bytes = Vector<u8> { 0xc0, 0x80 };
    auto overlong_null = StringView(bytes(overlong_null_bytes));

    EXPECT(decoder.to_utf8(overlong_null, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Fatal).is_error());
    EXPECT_EQ(MUST(decoder.to_utf8(overlong_null, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)), "\xef\xbf\xbd\xef\xbf\xbd"sv);
    EXPECT_EQ(process_code_points(decoder, overlong_null), (Vector<u32> { 0xfffd, 0xfffd }));
}

TEST_CASE(test_utf8_process_code_points_restores_invalid_second_byte)
{
    auto& decoder = decoder_for("UTF-8"sv);
    auto overlong_three_byte_sequence_bytes = Vector<u8> { 0xe0, 0x80, 0x80 };
    auto overlong_three_byte_sequence = StringView(bytes(overlong_three_byte_sequence_bytes));
    auto out_of_range_four_byte_sequence_bytes = Vector<u8> { 0xf4, 0x90, 0x80, 0x80 };
    auto out_of_range_four_byte_sequence = StringView(bytes(out_of_range_four_byte_sequence_bytes));

    EXPECT(decoder.to_utf8(overlong_three_byte_sequence, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Fatal).is_error());
    EXPECT_EQ(MUST(decoder.to_utf8(overlong_three_byte_sequence, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)), "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"sv);
    EXPECT_EQ(process_code_points(decoder, overlong_three_byte_sequence), (Vector<u32> { 0xfffd, 0xfffd, 0xfffd }));

    EXPECT(decoder.to_utf8(out_of_range_four_byte_sequence, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Fatal).is_error());
    EXPECT_EQ(MUST(decoder.to_utf8(out_of_range_four_byte_sequence, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)), "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"sv);
    EXPECT_EQ(process_code_points(decoder, out_of_range_four_byte_sequence), (Vector<u32> { 0xfffd, 0xfffd, 0xfffd, 0xfffd }));
}

TEST_CASE(test_utf16be_decode)
{
    auto& decoder = decoder_for("UTF-16BE"sv);
    // This is the output of `python3 -c "print('säk😀'.encode('utf-16be'))"`.
    auto test_string = "\x00s\x00\xe4\x00k\xd8=\xde\x00"sv;

    auto utf8 = MUST(decoder.to_utf8(test_string, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Fatal));
    EXPECT_EQ(utf8, "säk😀"sv);
}

TEST_CASE(test_utf16be_process_code_points)
{
    auto& decoder = decoder_for("UTF-16BE"sv);

    EXPECT_EQ(process_code_points(decoder, StringView(bytes({ 0xfe, 0xff, 0x00, 'A', 0xd8, 0x3d, 0xde, 0x00 }))), (Vector<u32> { 0x41, 0x1F600 }));
    EXPECT_EQ(process_code_points(decoder, StringView(bytes({ 0xd8, 0x3d, 0x00, 'A', 0xde, 0x00 }))), (Vector<u32> { 0xfffd, 0x41, 0xfffd }));
    EXPECT_EQ(process_code_points(decoder, StringView(bytes({ 0x00, 'A', 0xff }))), (Vector<u32> { 0x41, 0xfffd }));
    EXPECT_EQ(MUST(decoder.to_utf8(StringView(bytes({ 0x00, 'A', 0xff })), TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)), "A\xef\xbf\xbd"sv);
}

TEST_CASE(test_streaming_decoder_utf8_mid_sequence)
{
    auto streaming_decoder = TextCodec::StreamingDecoder { "UTF-8"sv, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 'a', 0xc3 }))), "a"sv);
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0xa9, 'b' }))), "éb"sv);
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_finishes_incomplete_sequence)
{
    auto& decoder = decoder_for("UTF-8"sv);
    auto streaming_decoder = TextCodec::StreamingDecoder { "UTF-8"sv, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement };

    auto incomplete_sequence = Vector<u8> { 0xc3 };
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes(incomplete_sequence))), ""sv);
    EXPECT_EQ(MUST(streaming_decoder.finish()), MUST(decoder.to_utf8(StringView(bytes(incomplete_sequence)), TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)));
}

TEST_CASE(test_streaming_decoder_utf8_invalid_second_byte_tail)
{
    auto streaming_decoder_for_overlong_three_byte_sequence = TextCodec::StreamingDecoder { "UTF-8"sv, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement };
    EXPECT_EQ(MUST(streaming_decoder_for_overlong_three_byte_sequence.to_utf8(bytes({ 0xe0, 0x80 }))), "\xef\xbf\xbd\xef\xbf\xbd"sv);
    EXPECT_EQ(MUST(streaming_decoder_for_overlong_three_byte_sequence.finish()), ""sv);

    auto streaming_decoder_for_out_of_range_four_byte_sequence = TextCodec::StreamingDecoder { "UTF-8"sv, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement };
    EXPECT_EQ(MUST(streaming_decoder_for_out_of_range_four_byte_sequence.to_utf8(bytes({ 0xf4, 0x90 }))), "\xef\xbf\xbd\xef\xbf\xbd"sv);
    EXPECT_EQ(MUST(streaming_decoder_for_out_of_range_four_byte_sequence.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_utf8_valid_second_byte_tail)
{
    auto streaming_decoder = TextCodec::StreamingDecoder { "UTF-8"sv, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0xe0, 0xa0 }))), ""sv);
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0x80 }))), "\xe0\xa0\x80"sv);
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_utf8_fatal_error_does_not_buffer_later_bytes)
{
    auto streaming_decoder = TextCodec::StreamingDecoder { "UTF-8"sv, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Fatal };

    EXPECT(streaming_decoder.to_utf8(bytes({ 0xfd, 0xef })).is_error());
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_utf16_odd_byte)
{
    auto streaming_decoder = TextCodec::StreamingDecoder { "UTF-16LE"sv, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0x41, 0x00, 0x42 }))), "A"sv);
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0x00 }))), "B"sv);
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_utf16_surrogate_pair_split)
{
    auto streaming_decoder = TextCodec::StreamingDecoder { "UTF-16BE"sv, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0x00, 0x41, 0xd8, 0x3d }))), "A"sv);
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0xde, 0x00 }))), "😀"sv);
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_gb18030_four_byte_tail)
{
    auto& decoder = decoder_for("gb18030"sv);
    auto streaming_decoder = TextCodec::StreamingDecoder { "gb18030"sv, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement };
    auto gb18030_sequence = Vector<u8> { 0x81, 0x30, 0x81, 0x30 };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 'a', 0x81, 0x30, 0x81 }))), "a"sv);
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0x30 }))), MUST(decoder.to_utf8(StringView(bytes(gb18030_sequence)), TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)));
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_big5_overlapping_trail)
{
    auto& decoder = decoder_for("Big5"sv);
    auto streaming_decoder = TextCodec::StreamingDecoder { "Big5"sv, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement };
    auto big5_sequence = Vector<u8> { 0xa4, 0xa4 };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes(big5_sequence))), MUST(decoder.to_utf8(StringView(bytes(big5_sequence)), TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)));
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);

    auto streaming_decoder_for_split_input = TextCodec::StreamingDecoder { "Big5"sv, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement };
    EXPECT_EQ(MUST(streaming_decoder_for_split_input.to_utf8(bytes({ 0xa4 }))), ""sv);
    EXPECT_EQ(MUST(streaming_decoder_for_split_input.to_utf8(bytes({ 0xa4 }))), MUST(decoder.to_utf8(StringView(bytes(big5_sequence)), TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)));
    EXPECT_EQ(MUST(streaming_decoder_for_split_input.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_euc_jp_three_byte_tail)
{
    auto& decoder = decoder_for("EUC-JP"sv);
    auto streaming_decoder = TextCodec::StreamingDecoder { "EUC-JP"sv, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement };
    auto euc_jp_sequence = Vector<u8> { 0x8f, 0xa2, 0xaf };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0x8f, 0xa2 }))), ""sv);
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0xaf }))), MUST(decoder.to_utf8(StringView(bytes(euc_jp_sequence)), TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)));
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_streaming_decoder_shift_jis_tail)
{
    auto& decoder = decoder_for("Shift_JIS"sv);
    auto streaming_decoder = TextCodec::StreamingDecoder { "Shift_JIS"sv, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement };
    auto shift_jis_sequence = Vector<u8> { 0x82, 0xa0 };

    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0x82 }))), ""sv);
    EXPECT_EQ(MUST(streaming_decoder.to_utf8(bytes({ 0xa0 }))), MUST(decoder.to_utf8(StringView(bytes(shift_jis_sequence)), TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)));
    EXPECT_EQ(MUST(streaming_decoder.finish()), ""sv);
}

TEST_CASE(test_utf16le_decode)
{
    auto& decoder = decoder_for("UTF-16LE"sv);
    // This is the output of `python3 -c "print('säk😀'.encode('utf-16le'))"`.
    auto test_string = "s\x00\xe4\x00k\x00=\xd8\x00\xde"sv;

    auto utf8 = MUST(decoder.to_utf8(test_string, TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Fatal));
    EXPECT_EQ(utf8, "säk😀"sv);
}

TEST_CASE(test_utf16le_process_code_points)
{
    auto& decoder = decoder_for("UTF-16LE"sv);

    EXPECT_EQ(process_code_points(decoder, StringView(bytes({ 0xff, 0xfe, 'A', 0x00, 0x3d, 0xd8, 0x00, 0xde }))), (Vector<u32> { 0x41, 0x1F600 }));
    EXPECT_EQ(process_code_points(decoder, StringView(bytes({ 0x3d, 0xd8, 'A', 0x00, 0x00, 0xde }))), (Vector<u32> { 0xfffd, 0x41, 0xfffd }));
    EXPECT_EQ(process_code_points(decoder, StringView(bytes({ 'A', 0x00, 0xff }))), (Vector<u32> { 0x41, 0xfffd }));
    EXPECT_EQ(MUST(decoder.to_utf8(StringView(bytes({ 'A', 0x00, 0xff })), TextCodec::IgnoreBOM::No, TextCodec::ErrorMode::Replacement)), "A\xef\xbf\xbd"sv);
}
