/*
 * Copyright (c) 2023-2024, Kemal Zebari <kemalzebra@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/MimeSniff/Resource.h>

namespace {

using namespace Web::MimeSniff;

struct BytePatternTableRow {
    StringView byte_pattern;
    StringView pattern_mask;
    ReadonlyBytes ignored_leading_bytes;
    StringView mime_type;

    // NOTE: If the byte pattern has a tag-terminating byte, add a byte where this byte should be. The value itself is ignored in
    //       the pattern_matching_algorithm() (see the NOTE in this algorithm for more details).
    bool is_tag_terminated { false };
};

// https://mimesniff.spec.whatwg.org/#tag-terminating-byte
bool is_tag_terminating_byte(u8 byte)
{
    // A tag-terminating byte (abbreviated 0xTT) is any one of the following bytes: 0x20 (SP), 0x3E (">").
    return byte == 0x20 || byte == 0x3E;
}

// https://mimesniff.spec.whatwg.org/#binary-data-byte
bool is_binary_data_byte(u8 byte)
{
    //  A binary data byte is a byte in the range 0x00 to 0x08 (NUL to BS), the byte 0x0B (VT), a byte in
    //  the range 0x0E to 0x1A (SO to SUB), or a byte in the range 0x1C to 0x1F (FS to US).
    return (byte <= 0x08) || byte == 0x0B || (byte >= 0x0E && byte <= 0x1A) || (byte >= 0x1C && byte <= 0x1F);
}

// https://mimesniff.spec.whatwg.org/#pattern-matching-algorithm
bool pattern_matching_algorithm(ReadonlyBytes input, ReadonlyBytes pattern, ReadonlyBytes mask, ReadonlyBytes ignored, bool is_tag_terminated = false)
{
    // 1. Assert: pattern’s length is equal to mask’s length.
    VERIFY(pattern.size() == mask.size());

    // 2. If input’s length is less than pattern’s length, return false.
    if (input.size() < pattern.size())
        return false;

    // 3. Let s be 0.
    size_t s = 0;

    // 4. While s < input’s length:
    while (s < input.size()) {
        // 1. If ignored does not contain input[s], break.
        if (!ignored.contains_slow(input[s]))
            break;

        // 2. Set s to s + 1.
        s++;
    }

    // 5. Let p be 0.
    size_t p = 0;

    // 6. While p < pattern’s length:
    while (p < pattern.size()) {
        // 1. Let maskedData be the result of applying the bitwise AND operator to input[s] and mask[p].
        u8 masked_data = input[s] & mask[p];

        // NOTE: This non-standard branch exists to avoid having to create 2 byte patterns just so that
        //       they can only differ by their tag-terminating byte (which could be a 0x20 or 0x3E byte).
        if (is_tag_terminated && p + 1 == pattern.size())
            return is_tag_terminating_byte(masked_data);

        // 2. If maskedData is not equal to pattern[p], return false.
        if (masked_data != pattern[p])
            return false;

        // 3. Set s to s + 1.
        s++;

        // 4. Set p to p + 1.
        p++;
    }

    // 7. Return true.
    return true;
}

ReadonlyBytes constexpr no_ignored_bytes;

// https://mimesniff.spec.whatwg.org/#matching-an-image-type-pattern
Optional<MimeType> match_an_image_type_pattern(ReadonlyBytes input)
{
    // 1. Execute the following steps for each row row in the following table:
    static Array<BytePatternTableRow, 8> constexpr pattern_table {
        // A Windows Icon signature.
        BytePatternTableRow { "\x00\x00\x01\x00"sv, "\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "image/x-icon"sv },

        // A Windows Cursor signature.
        BytePatternTableRow { "\x00\x00\x02\x00"sv, "\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "image/x-icon"sv },

        // The string "BM", a BMP signature.
        BytePatternTableRow { "\x42\x4D"sv, "\xFF\xFF"sv, no_ignored_bytes, "image/bmp"sv },

        // The string "GIF87a", a GIF signature.
        BytePatternTableRow { "\x47\x49\x46\x38\x37\x61"sv, "\xFF\xFF\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "image/gif"sv },

        // The string "GIF89a", a GIF signature.
        BytePatternTableRow { "\x47\x49\x46\x38\x39\x61"sv, "\xFF\xFF\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "image/gif"sv },

        // The string "RIFF" followed by four bytes followed by the string "WEBPVP".
        BytePatternTableRow { "\x52\x49\x46\x46\x00\x00\x00\x00\x57\x45\x42\x50\x56\x50"sv,
            "\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "image/webp"sv },

        // An error-checking byte followed by the string "PNG" followed by CR LF SUB LF, the PNG signature.
        BytePatternTableRow { "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A"sv, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "image/png"sv },

        // The JPEG Start of Image marker followed by the indicator byte of another marker.
        BytePatternTableRow { "\xFF\xD8\xFF"sv, "\xFF\xFF\xFF"sv, no_ignored_bytes, "image/jpeg"sv },
    };

    for (auto const& row : pattern_table) {
        // 1. Let patternMatched be the result of the pattern matching algorithm given input, the value in
        //    the first column of row, the value in the second column of row, and the value in the third
        //    column of row.
        auto pattern_matched = pattern_matching_algorithm(input, row.byte_pattern.bytes(), row.pattern_mask.bytes(), row.ignored_leading_bytes);

        // 2. If patternMatched is true, return the value in the fourth column of row.
        if (pattern_matched)
            return MimeType::parse(row.mime_type);
    }

    // 2. Return undefined.
    return OptionalNone {};
}

// https://mimesniff.spec.whatwg.org/#signature-for-mp4
bool matches_mp4_signature(ReadonlyBytes sequence)
{
    // 1. Let sequence be the byte sequence to be matched, where sequence[s] is byte s in sequence and sequence[0] is the first byte in sequence.

    // 2. Let length be the number of bytes in sequence.
    auto length = sequence.size();

    // 3. If length is less than 12, return false.
    if (length < 12)
        return false;

    // 4. Let box-size be the four bytes from sequence[0] to sequence[3], interpreted as a 32-bit unsigned big-endian integer.
    u32 box_size = 0;
    box_size |= static_cast<u32>(sequence[0] << 24);
    box_size |= static_cast<u32>(sequence[1] << 16);
    box_size |= static_cast<u32>(sequence[2] << 8);
    box_size |= sequence[3];

    // 5. If length is less than box-size or if box-size modulo 4 is not equal to 0, return false.
    if ((length < box_size) || (box_size % 4 != 0))
        return false;

    // 6. If the four bytes from sequence[4] to sequence[7] are not equal to 0x66 0x74 0x79 0x70 ("ftyp"), return false.
    if (sequence.slice(4, 4) != "\x66\x74\x79\x70"sv.bytes())
        return false;

    // 7. If the three bytes from sequence[8] to sequence[10] are equal to 0x6D 0x70 0x34 ("mp4"), return true.
    if (sequence.slice(8, 3) == "\x6D\x70\x34"sv.bytes())
        return true;

    // 8. Let bytes-read be 16.
    u32 bytes_read = 16;

    // 9. While bytes-read is less than box-size, continuously loop through these steps:
    //      1. If the three bytes from sequence[bytes-read] to sequence[bytes-read + 2] are equal to 0x6D 0x70 0x34 ("mp4"), return true.
    //      2. Increment bytes-read by 4.
    while (bytes_read < box_size) {
        if (sequence.slice(bytes_read, 3) == "\x6D\x70\x34"sv.bytes())
            return true;
        bytes_read += 4;
    }

    // 10. Return false.
    return false;
}

// https://mimesniff.spec.whatwg.org/#matching-an-audio-or-video-type-pattern
Optional<MimeType> match_an_audio_or_video_type_pattern(ReadonlyBytes input)
{
    // 1. Execute the following steps for each row row in the following table:
    static Array<BytePatternTableRow, 6> constexpr pattern_table {
        // The string "FORM" followed by four bytes followed by the string "AIFF", the AIFF signature.
        BytePatternTableRow { "\x46\x4F\x52\x4D\x00\x00\x00\x00\x41\x49\x46\x46"sv,
            "\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "audio/aiff"sv },

        //  The string "ID3", the ID3v2-tagged MP3 signature.
        BytePatternTableRow { "\x49\x44\x33"sv, "\xFF\xFF\xFF"sv, no_ignored_bytes, "audio/mpeg"sv },

        // The string "OggS" followed by NUL, the Ogg container signature.
        BytePatternTableRow { "\x4F\x67\x67\x53\x00"sv, "\xFF\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "application/ogg"sv },

        // The string "MThd" followed by four bytes representing the number 6 in 32 bits (big-endian), the MIDI signature.
        BytePatternTableRow { "\x4D\x54\x68\x64\x00\x00\x00\x06"sv, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "audio/midi"sv },

        // The string "RIFF" followed by four bytes followed by the string "AVI ", the AVI signature.
        BytePatternTableRow { "\x52\x49\x46\x46\x00\x00\x00\x00\x41\x56\x49\x20"sv,
            "\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "video/avi"sv },

        // The string "RIFF" followed by four bytes followed by the string "WAVE", the WAVE signature.
        BytePatternTableRow { "\x52\x49\x46\x46\x00\x00\x00\x00\x57\x41\x56\x45"sv,
            "\xFF\xFF\xFF\xFF\x00\x00\x00\x00\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "audio/wave"sv }
    };

    for (auto const& row : pattern_table) {
        // 1. Let patternMatched be the result of the pattern matching algorithm given input, the
        //    value in the first column of row, the value in the second column of row, and the
        //    value in the third column of row.
        auto pattern_matched = pattern_matching_algorithm(input, row.byte_pattern.bytes(), row.pattern_mask.bytes(), row.ignored_leading_bytes);

        // 2. If patternMatched is true, return the value in the fourth column of row.
        if (pattern_matched)
            return MimeType::parse(row.mime_type);
    }

    // 2. If input matches the signature for MP4, return "video/mp4".
    if (matches_mp4_signature(input))
        return MimeType::create("video"_string, "mp4"_string);

    // FIXME: 3. If input matches the signature for WebM, return "video/webm".
    // FIXME: 4. If input matches the signature for MP3 without ID3, return "audio/mpeg".

    // 5. Return undefined.
    return OptionalNone {};
}

// https://mimesniff.spec.whatwg.org/#matching-a-font-type-pattern
Optional<MimeType> match_a_font_type_pattern(ReadonlyBytes input)
{
    // 1. Execute the following steps for each row row in the following table:
    static Array<BytePatternTableRow, 6> constexpr pattern_table {
        // 34 bytes followed by the string "LP", the Embedded OpenType signature.
        BytePatternTableRow {
            .byte_pattern = "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x4C\x50"sv,
            .pattern_mask = "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xFF\xFF"sv,
            .ignored_leading_bytes = no_ignored_bytes,
            .mime_type = "application/vnd.ms-fontobject"sv,
        },

        // 4 bytes representing the version number 1.0, a TrueType signature.
        BytePatternTableRow { "\x00\x01\x00\x00"sv, "\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "font/ttf"sv },

        // The string "OTTO", the OpenType signature.
        BytePatternTableRow { "\x4F\x54\x54\x4F"sv, "\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "font/otf"sv },

        // The string "ttcf", the TrueType Collection signature.
        BytePatternTableRow { "\x74\x74\x63\x66"sv, "\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "font/collection"sv },

        // The string "wOFF", the Web Open Font Format 1.0 signature.
        BytePatternTableRow { "\x77\x4F\x46\x46"sv, "\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "font/woff"sv },

        // The string "wOF2", the Web Open Font Format 2.0 signature.
        BytePatternTableRow { "\x77\x4F\x46\x32"sv, "\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "font/woff2"sv },
    };

    for (auto const& row : pattern_table) {
        // 1. Let patternMatched be the result of the pattern matching algorithm given input, the
        //    value in the first column of row, the value in the second column of row, and the
        //    value in the third column of row.
        auto pattern_matched = pattern_matching_algorithm(input, row.byte_pattern.bytes(), row.pattern_mask.bytes(), row.ignored_leading_bytes);

        // 2. If patternMatched is true, return the value in the fourth column of row.
        if (pattern_matched)
            return MimeType::parse(row.mime_type);
    }

    // 2. Return undefined.
    return OptionalNone {};
}

// https://mimesniff.spec.whatwg.org/#matching-an-archive-type-pattern
Optional<MimeType> match_an_archive_type_pattern(ReadonlyBytes input)
{
    // 1. Execute the following steps for each row row in the following table:
    static Array<BytePatternTableRow, 3> constexpr pattern_table {
        // The GZIP archive signature.
        BytePatternTableRow { "\x1F\x8B\x08"sv, "\xFF\xFF\xFF"sv, no_ignored_bytes, "application/x-gzip"sv },

        // The string "PK" followed by ETX EOT, the ZIP archive signature.
        BytePatternTableRow { "\x50\x4B\x03\x04"sv, "\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "application/zip"sv },

        // The string "Rar " followed by SUB BEL NUL, the RAR archive signature.
        BytePatternTableRow { "\x52\x61\x72\x20\x1A\x07\x00"sv, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "application/x-rar-compressed"sv },
    };

    for (auto const& row : pattern_table) {
        // 1. Let patternMatched be the result of the pattern matching algorithm given input, the
        //    value in the first column of row, the value in the second column of row, and the
        //    value in the third column of row.
        auto pattern_matched = pattern_matching_algorithm(input, row.byte_pattern.bytes(), row.pattern_mask.bytes(), row.ignored_leading_bytes);

        // 2. If patternMatched is true, return the value in the fourth column of row.
        if (pattern_matched)
            return MimeType::parse(row.mime_type);
    }

    // 2. Return undefined.
    return OptionalNone {};
}

// https://mimesniff.spec.whatwg.org/#rules-for-identifying-an-unknown-mime-type
MimeType rules_for_identifying_an_unknown_mime_type(Resource const& resource, bool sniff_scriptable = false)
{
    // 1. If the sniff-scriptable flag is set, execute the following steps for each row row in the following table:
    if (sniff_scriptable) {
        static auto constexpr text_html_mime_type = "text/html"sv;

        // https://mimesniff.spec.whatwg.org/#whitespace-byte
        // A whitespace byte (abbreviated 0xWS) is any one of the following bytes: 0x09 (HT), 0x0A (LF), 0x0C (FF), 0x0D (CR), 0x20 (SP).
        static Array<u8, 5> constexpr ignored_whitespace_bytes { 0x09, 0x0A, 0x0C, 0x0D, 0x20 };
        static Array<BytePatternTableRow, 19> constexpr pattern_table {
            // The case-insensitive string "<!DOCTYPE HTML" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x21\x44\x4F\x43\x54\x59\x50\x45\x20\x48\x54\x4D\x4C\x00"sv,
                "\xFF\xFF\xDF\xDF\xDF\xDF\xDF\xDF\xDF\xFF\xDF\xDF\xDF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<HTML" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x48\x54\x4D\x4C\x00"sv, "\xFF\xDF\xDF\xDF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<HEAD" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x48\x45\x41\x44\x00"sv, "\xFF\xDF\xDF\xDF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<SCRIPT" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x53\x43\x52\x49\x50\x54\x00"sv,
                "\xFF\xDF\xDF\xDF\xDF\xDF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<IFRAME" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x49\x46\x52\x41\x4D\x45\x00"sv,
                "\xFF\xDF\xDF\xDF\xDF\xDF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<H1" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x48\x31\x00"sv, "\xFF\xDF\xFF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<DIV" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x44\x49\x56\x00"sv, "\xFF\xDF\xDF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<FONT" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x46\x4F\x4E\x54\x00"sv, "\xFF\xDF\xDF\xDF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<TABLE" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x54\x41\x42\x4C\x45\x00"sv, "\xFF\xDF\xDF\xDF\xDF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<A" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x41\x00"sv, "\xFF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<STYLE" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x53\x54\x59\x4C\x45\x00"sv,
                "\xFF\xDF\xDF\xDF\xDF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<TITLE" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x54\x49\x54\x4C\x45\x00"sv,
                "\xFF\xDF\xDF\xDF\xDF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<B" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x42\x00"sv, "\xFF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<BODY" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x42\x4F\x44\x59\x00"sv, "\xFF\xDF\xDF\xDF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<BR" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x42\x52\x00"sv, "\xFF\xDF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The case-insensitive string "<P" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x50\x00"sv, "\xFF\xDF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The string "<!--" followed by a tag-terminating byte.
            BytePatternTableRow { "\x3C\x21\x2D\x2D\x00"sv, "\xFF\xFF\xFF\xFF\xFF"sv, ignored_whitespace_bytes, text_html_mime_type, true },

            // The string "<?xml".
            BytePatternTableRow { "\x3C\x3F\x78\x6D\x6C"sv, "\xFF\xFF\xFF\xFF\xFF"sv, ignored_whitespace_bytes, "text/xml"sv },

            // The string "%PDF-", the PDF signature.
            BytePatternTableRow { "\x25\x50\x44\x46\x2D"sv, "\xFF\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "application/pdf"sv },
        };

        for (auto const& row : pattern_table) {
            // 1. Let patternMatched be the result of the pattern matching algorithm given resource’s resource header,
            //    the value in the first column of row, the value in the second column of row, and the value in the
            //    third column of row.
            auto pattern_matched = pattern_matching_algorithm(resource.resource_header(), row.byte_pattern.bytes(), row.pattern_mask.bytes(), row.ignored_leading_bytes, row.is_tag_terminated);

            // 2. If patternMatched is true, return the value in the fourth column of row.
            if (pattern_matched) {
                if (auto maybe_type = MimeType::parse(row.mime_type); maybe_type.has_value())
                    return maybe_type.release_value();
            }
        }
    }

    // 2. Execute the following steps for each row row in the following table:
    static auto constexpr text_plain_mime_type = "text/plain"sv;
    static Array<BytePatternTableRow, 4> constexpr pattern_table {
        // The string "%!PS-Adobe-", the PostScript signature.
        BytePatternTableRow { "\x25\x21\x50\x53\x2D\x41\x64\x6F\x62\x65\x2D"sv,
            "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"sv, no_ignored_bytes, "application/postscript"sv },

        // UTF-16BE BOM
        BytePatternTableRow { "\xFE\xFF\x00\x00"sv, "\xFF\xFF\x00\x00"sv, no_ignored_bytes, text_plain_mime_type },

        // UTF-16LE BOM
        BytePatternTableRow { "\xFF\xFE\x00\x00"sv, "\xFF\xFF\x00\x00"sv, no_ignored_bytes, text_plain_mime_type },

        // UTF-8 BOM
        BytePatternTableRow { "\xEF\xBB\xBF\x00"sv, "\xFF\xFF\xFF\x00"sv, no_ignored_bytes, text_plain_mime_type },
    };

    for (auto const& row : pattern_table) {
        // 1. Let patternMatched be the result of the pattern matching algorithm given resource’s resource header,
        //    the value in the first column of row, the value in the second column of row, and the value in the
        //    third column of row.
        auto pattern_matched = pattern_matching_algorithm(resource.resource_header(), row.byte_pattern.bytes(), row.pattern_mask.bytes(), row.ignored_leading_bytes);

        // 2. If patternMatched is true, return the value in the fourth column of row.
        if (pattern_matched) {
            if (auto maybe_type = MimeType::parse(row.mime_type); maybe_type.has_value())
                return maybe_type.release_value();
        }
    }

    // 3. Let matchedType be the result of executing the image type pattern matching algorithm given resource’s resource header.
    auto matched_type = match_an_image_type_pattern(resource.resource_header());

    // 4. If matchedType is not undefined, return matchedType.
    if (matched_type.has_value())
        return matched_type.release_value();

    // 5. Set matchedType to the result of executing the audio or video type pattern matching algorithm given resource’s resource header.
    matched_type = match_an_audio_or_video_type_pattern(resource.resource_header());

    // 6. If matchedType is not undefined, return matchedType.
    if (matched_type.has_value())
        return matched_type.release_value();

    // 7. Set matchedType to the result of executing the archive type pattern matching algorithm given resource’s resource header.
    matched_type = match_an_archive_type_pattern(resource.resource_header());

    // 8. If matchedType is not undefined, return matchedType.
    if (matched_type.has_value())
        return matched_type.release_value();

    // 9. If resource’s resource header contains no binary data bytes, return "text/plain".
    if (!any_of(resource.resource_header(), is_binary_data_byte))
        return MimeType::create("text"_string, "plain"_string);

    // 10. Return "application/octet-stream".
    return MimeType::create("application"_string, "octet-stream"_string);
}

}

namespace Web::MimeSniff {

Resource Resource::create(ReadonlyBytes data, SniffingConfiguration configuration)
{
    // NOTE: Non-standard but for cases where pattern matching fails, let's fall back to the safest MIME type.
    auto default_computed_mime_type = MimeType::create("application"_string, "octet-stream"_string);
    auto resource = Resource { data, configuration.no_sniff, move(default_computed_mime_type) };

    resource.supplied_mime_type_detection_algorithm(configuration.scheme, move(configuration.supplied_type));
    resource.context_specific_sniffing_algorithm(configuration.sniffing_context);

    return resource;
}

MimeType Resource::sniff(ReadonlyBytes data, SniffingConfiguration configuration)
{
    auto resource = create(data, move(configuration));
    return move(resource.m_computed_mime_type);
}

Resource::Resource(ReadonlyBytes data, bool no_sniff, MimeType&& default_computed_mime_type)
    : m_no_sniff(no_sniff)
    , m_computed_mime_type(move(default_computed_mime_type))
{
    read_the_resource_header(data);
}

Resource::~Resource() = default;

// https://mimesniff.spec.whatwg.org/#supplied-mime-type-detection-algorithm
// NOTE: Parameters are non-standard.
void Resource::supplied_mime_type_detection_algorithm(StringView scheme, Optional<MimeType> supplied_type)
{
    // 1. Let supplied-type be null.
    // 2. If the resource is retrieved via HTTP, execute the following steps:
    //        1. If one or more Content-Type headers are associated with the resource, execute the following steps:
    //               1. Set supplied-type to the value of the last Content-Type header associated with the resource.
    //               2. Set the check-for-apache-bug flag if supplied-type is exactly equal to one of the values in the following table:
    // NOTE: Non-standard but this algorithm expects the caller to handle step 2.1.1.
    if (supplied_type.has_value()) {
        if (Fetch::Infrastructure::is_http_or_https_scheme(scheme)) {
            // NOTE: The spec expects a space between the semicolon and the start of the charset parameter. However, we will lose this
            //       space because MimeType::parse() ignores any spaces found there.
            static Array<StringView, 4> constexpr apache_bug_mime_types = {
                "text/plain"sv,
                "text/plain;charset=ISO-8859-1"sv,
                "text/plain;charset=iso-8859-1"sv,
                "text/plain;charset=UTF-8"sv
            };

            auto serialized_supplied_type = supplied_type->serialized();
            for (auto apache_bug_mime_type : apache_bug_mime_types) {
                if (serialized_supplied_type == apache_bug_mime_type) {
                    m_check_for_apache_bug_flag = true;
                    break;
                }
            }
        }
    }

    // 3. If the resource is retrieved directly from the file system, set supplied-type
    //    to the MIME type provided by the file system.
    // 4. If the resource is retrieved via another protocol (such as FTP), set
    //    supplied-type to the MIME type as determined by that protocol, if any.
    // 5. If supplied-type is not a MIME type, the supplied MIME type is undefined.
    //    Abort these steps.
    // 6. The supplied MIME type is supplied-type.
    // NOTE: The expectation is for the caller to handle these spec steps.
    m_supplied_mime_type = supplied_type;
}

// https://mimesniff.spec.whatwg.org/#read-the-resource-header
void Resource::read_the_resource_header(ReadonlyBytes data)
{
    // 1. Let buffer be a byte sequence.
    ByteBuffer buffer;

    // 2. Read bytes of the resource into buffer until one of the following conditions is met:
    //      - the end of the resource is reached.
    //      - the number of bytes in buffer is greater than or equal to 1445.
    //      - a reasonable amount of time has elapsed, as determined by the user agent.
    // FIXME: The spec expects us to be reading from a stream. Reimplement this spec step once
    //        we have greater support for streaming in areas that calls on this API.
    static size_t constexpr MAX_SNIFF_SIZE = 1445;
    buffer.append(data.slice(0, min(data.size(), MAX_SNIFF_SIZE)));

    // 3. The resource header is buffer.
    m_resource_header = move(buffer);
}

// https://mimesniff.spec.whatwg.org/#mime-type-sniffing-algorithm
void Resource::mime_type_sniffing_algorithm()
{
    // 1. If the supplied MIME type is an XML MIME type or HTML MIME type, the computed MIME type is the supplied MIME type.
    //    Abort these steps.
    if (m_supplied_mime_type.has_value() && (m_supplied_mime_type->is_xml() || m_supplied_mime_type->is_html())) {
        m_computed_mime_type = m_supplied_mime_type.value();
        return;
    }

    // 2. If the supplied MIME type is undefined or if the supplied MIME type’s essence
    //    is "unknown/unknown", "application/unknown", or "*/*", execute the rules for
    //    identifying an unknown MIME type with the sniff-scriptable flag equal to the
    //    inverse of the no-sniff flag and abort these steps.
    if (!m_supplied_mime_type.has_value() || m_supplied_mime_type->essence().is_one_of("unknown/unknown", "application/unknown", "*/*")) {
        m_computed_mime_type = rules_for_identifying_an_unknown_mime_type(*this, !m_no_sniff);
        return;
    }

    // 3. If the no-sniff flag is set, the computed MIME type is the supplied MIME type.
    //    Abort these steps.
    if (m_no_sniff) {
        m_computed_mime_type = m_supplied_mime_type.value();
        return;
    }

    // 4. If the check-for-apache-bug flag is set, execute the rules for distinguishing
    //    if a resource is text or binary and abort these steps.
    if (m_check_for_apache_bug_flag) {
        rules_for_distinguishing_if_a_resource_is_text_or_binary();
        return;
    }

    // FIXME: 5. If the supplied MIME type is an image MIME type supported by the user agent, let matched-type be
    //    the result of executing the image type pattern matching algorithm with the resource header as
    //    the byte sequence to be matched.
    Optional<MimeType> matched_type;

    // 6. If matched-type is not undefined, the computed MIME type is matched-type.
    //    Abort these steps.
    if (matched_type.has_value()) {
        m_computed_mime_type = matched_type.release_value();
        return;
    }

    // FIXME: 7. If the supplied MIME type is an audio or video MIME type supported by the user agent, let matched-type be
    //    the result of executing the audio or video type pattern matching algorithm with the resource header as
    //    the byte sequence to be matched.

    // 8. If matched-type is not undefined, the computed MIME type is matched-type.
    //    Abort these steps.
    if (matched_type.has_value()) {
        m_computed_mime_type = matched_type.release_value();
        return;
    }

    // 9. The computed MIME type is the supplied MIME type.
    m_computed_mime_type = m_supplied_mime_type.value();

    return;
}

// https://mimesniff.spec.whatwg.org/#sniffing-a-mislabeled-binary-resource
void Resource::rules_for_distinguishing_if_a_resource_is_text_or_binary()
{
    // 1. Let length be the number of bytes in the resource header.
    auto length = m_resource_header.size();

    // 2. If length is greater than or equal to 2 and the first 2 bytes of the
    //    resource header are equal to 0xFE 0xFF (UTF-16BE BOM) or 0xFF 0xFE (UTF-16LE BOM), the computed MIME type is "text/plain".
    //    Abort these steps.
    auto resource_header_span = m_resource_header.span();
    auto utf_16_be_bom = "\xFE\xFF"sv.bytes();
    auto utf_16_le_bom = "\xFF\xFE"sv.bytes();
    if (length >= 2
        && (resource_header_span.starts_with(utf_16_be_bom)
            || resource_header_span.starts_with(utf_16_le_bom))) {
        m_computed_mime_type = MimeType::create("text"_string, "plain"_string);
        return;
    }

    // 3. If length is greater than or equal to 3 and the first 3 bytes of the resource header are equal to 0xEF 0xBB 0xBF (UTF-8 BOM),
    //    the computed MIME type is "text/plain".
    //    Abort these steps.
    auto utf_8_bom = "\xEF\xBB\xBF"sv.bytes();
    if (length >= 3 && resource_header_span.starts_with(utf_8_bom)) {
        m_computed_mime_type = MimeType::create("text"_string, "plain"_string);
        return;
    }

    // 4. If the resource header contains no binary data bytes, the computed MIME type is "text/plain".
    //    Abort these steps.
    if (!any_of(resource_header(), is_binary_data_byte)) {
        m_computed_mime_type = MimeType::create("text"_string, "plain"_string);
        return;
    }

    // 5. The computed MIME type is "application/octet-stream".
    // NOTE: This is the default MIME type of the computed MIME type.
    return;
}

// https://mimesniff.spec.whatwg.org/#context-specific-sniffing-algorithm
void Resource::context_specific_sniffing_algorithm(SniffingContext sniffing_context)
{
    // A context-specific sniffing algorithm determines the computed MIME type of a resource only if
    // the resource is a MIME type relevant to a particular context.
    if (sniffing_context == SniffingContext::None || sniffing_context == SniffingContext::Browsing) {
        // https://mimesniff.spec.whatwg.org/#sniffing-in-a-browsing-context
        // Use the MIME type sniffing algorithm.
        return mime_type_sniffing_algorithm();
    }

    // NOTE: Non-standard but if the client expects us to not sniff, we shouldn't be doing any
    //       context-specific sniffing if we don't have to.
    if (m_no_sniff && m_supplied_mime_type.has_value()) {
        m_computed_mime_type = m_supplied_mime_type.value();
        return;
    }

    if (sniffing_context == SniffingContext::Image)
        return rules_for_sniffing_images_specifically();
    if (sniffing_context == SniffingContext::AudioOrVideo)
        return rules_for_sniffing_audio_or_video_specifically();
    if (sniffing_context == SniffingContext::Font)
        return rules_for_sniffing_fonts_specifically();
    if (sniffing_context == SniffingContext::TextOrBinary)
        return rules_for_distinguishing_if_a_resource_is_text_or_binary();

    return;
}

// https://mimesniff.spec.whatwg.org/#sniffing-in-an-image-context
void Resource::rules_for_sniffing_images_specifically()
{
    // 1. If the supplied MIME type is an XML MIME type, the computed MIME type is the supplied MIME type.
    //    Abort these steps.
    // NOTE: Non-standard but due to the mime type detection algorithm we need this sanity check.
    if (m_supplied_mime_type.has_value() && m_supplied_mime_type->is_xml()) {
        m_computed_mime_type = m_supplied_mime_type.value();
        return;
    }

    // 2. Let image-type-matched be the result of executing the image type pattern matching algorithm with
    //    the resource header as the byte sequence to be matched.
    auto image_type_matched = match_an_image_type_pattern(resource_header());

    // 3. If image-type-matched is not undefined, the computed MIME type is image-type-matched.
    //    Abort these steps.
    if (image_type_matched.has_value()) {
        m_computed_mime_type = image_type_matched.release_value();
        return;
    }

    // 4. The computed MIME type is the supplied MIME type.
    // NOTE: Non-standard but due to the mime type detection algorithm we need this sanity check.
    if (m_supplied_mime_type.has_value()) {
        m_computed_mime_type = m_supplied_mime_type.value();
    }

    // NOTE: Non-standard but if the supplied mime type is undefined, we use computed mime type's default value.
    return;
}

// https://mimesniff.spec.whatwg.org/#sniffing-in-an-audio-or-video-context
void Resource::rules_for_sniffing_audio_or_video_specifically()
{
    // 1. If the supplied MIME type is an XML MIME type, the computed MIME type is the supplied MIME type.
    //    Abort these steps.
    // NOTE: Non-standard but due to the mime type detection algorithm we need this sanity check.
    if (m_supplied_mime_type.has_value() && m_supplied_mime_type->is_xml()) {
        m_computed_mime_type = m_supplied_mime_type.value();
        return;
    }

    // 2. Let audio-or-video-type-matched be the result of executing the audio or video type pattern matching
    //    algorithm with the resource header as the byte sequence to be matched.
    auto audio_or_video_type_matched = match_an_audio_or_video_type_pattern(resource_header());

    // 3. If audio-or-video-type-matched is not undefined, the computed MIME type is audio-or-video-type-matched.
    //    Abort these steps.
    if (audio_or_video_type_matched.has_value()) {
        m_computed_mime_type = audio_or_video_type_matched.release_value();
        return;
    }

    // 4. The computed MIME type is the supplied MIME type.
    // NOTE: Non-standard but due to the mime type detection algorithm we need this sanity check.
    if (m_supplied_mime_type.has_value()) {
        m_computed_mime_type = m_supplied_mime_type.value();
    }

    // NOTE: Non-standard but if the supplied mime type is undefined, we use computed mime type's default value.
    return;
}

// https://mimesniff.spec.whatwg.org/#sniffing-in-a-font-context
void Resource::rules_for_sniffing_fonts_specifically()
{
    // 1. If the supplied MIME type is an XML MIME type, the computed MIME type is the supplied MIME type.
    //    Abort these steps.
    // NOTE: Non-standard but due to the mime type detection algorithm we need this sanity check.
    if (m_supplied_mime_type.has_value() && m_supplied_mime_type->is_xml()) {
        m_computed_mime_type = m_supplied_mime_type.value();
        return;
    }

    // 2. Let font-type-matched be the result of executing the font type pattern matching algorithm with the
    //    resource header as the byte sequence to be matched.
    auto font_type_matched = match_a_font_type_pattern(resource_header());

    // 3. If font-type-matched is not undefined, the computed MIME type is font-type-matched.
    //    Abort these steps.
    if (font_type_matched.has_value()) {
        m_computed_mime_type = font_type_matched.release_value();
        return;
    }

    // 4. The computed MIME type is the supplied MIME type.
    // NOTE: Non-standard but due to the mime type detection algorithm we need this sanity check.
    if (m_supplied_mime_type.has_value()) {
        m_computed_mime_type = m_supplied_mime_type.value();
    }

    // NOTE: Non-standard but if the supplied mime type is undefined, we use computed mime type's default value.
    return;
}

}
