/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibWeb/Loader/DownloadFilename.h>

namespace Web {

ByteString truncate_filename_to_byte_length(ByteString filename, size_t maximum_byte_length)
{
    if (filename.length() <= maximum_byte_length)
        return filename;

    auto is_utf8_continuation_byte = [](char byte) {
        return (static_cast<u8>(byte) & 0b1100'0000) == 0b1000'0000;
    };

    // Avoid truncating in the middle of a multi-byte UTF-8 sequence.
    auto length = maximum_byte_length;
    for (size_t i = 0; i < 3; ++i) {
        if (length == 0 || !is_utf8_continuation_byte(filename[length]))
            break;
        --length;
    }
    return filename.substring(0, length);
}

ByteString sanitize_suggested_download_filename(ByteString filename)
{
    filename = LexicalPath::basename(move(filename));

    StringBuilder builder;
    for (auto byte : filename.bytes()) {
        if (byte == '\0' || byte == '/' || byte == '\\')
            builder.append('_');
        else
            builder.append(static_cast<char>(byte));
    }

    auto sanitized = builder.to_byte_string();
    if (sanitized.is_empty() || sanitized == "."sv || sanitized == ".."sv)
        return "download";

    if (sanitized.length() > maximum_filename_byte_length) {
        // Preserve the extension when truncating, so the file type is not lost.
        auto lexical_filename = LexicalPath { sanitized };
        auto extension = lexical_filename.extension();
        if (!extension.is_empty() && extension.length() + 1 < maximum_filename_byte_length) {
            auto truncated_title = truncate_filename_to_byte_length(lexical_filename.title(), maximum_filename_byte_length - extension.length() - 1);
            return ByteString::formatted("{}.{}", truncated_title, extension);
        }
        return truncate_filename_to_byte_length(move(sanitized), maximum_filename_byte_length);
    }
    return sanitized;
}

}
