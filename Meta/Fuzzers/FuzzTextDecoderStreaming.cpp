/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/BoundedInput.h"
#include <AK/StringBuilder.h>
#include <AK/Utf16StringBuilder.h>
#include <LibTextCodec/Decoder.h>

static ErrorOr<String> decode(ReadonlyBytes bytes, StringView encoding, TextCodec::IgnoreBOM bom, TextCodec::ErrorMode mode, size_t chunk_size)
{
    TextCodec::StreamingDecoder decoder(encoding, bom, mode);
    StringBuilder output;
    // Empty input and terminal incomplete sequences must also reach finish().
    while (!bytes.is_empty()) {
        auto count = min(bytes.size(), chunk_size);
        output.append(TRY(decoder.to_utf8(bytes.slice(0, count))));
        bytes = bytes.slice(count);
    }
    output.append(TRY(decoder.finish()));
    return output.to_string();
}

static ErrorOr<Utf16String> decode_utf16(ReadonlyBytes bytes, StringView encoding, TextCodec::IgnoreBOM bom, TextCodec::ErrorMode mode, size_t chunk_size)
{
    TextCodec::StreamingDecoder decoder(encoding, bom, mode);
    Utf16StringBuilder output;
    while (!bytes.is_empty()) {
        auto count = min(bytes.size(), chunk_size);
        auto part = TRY(decoder.to_utf16(bytes.slice(0, count)));
        output.append(part.utf16_view());
        bytes = bytes.slice(count);
    }
    auto tail = TRY(decoder.finish_to_utf16());
    output.append(tail.utf16_view());
    return output.to_string();
}

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    if (size < 3 || size > 65536)
        return 0;
    Fuzzing::BoundedInput input({ data, size });
    constexpr StringView encodings[] = { "UTF-8"sv, "UTF-16LE"sv, "UTF-16BE"sv, "Shift_JIS"sv, "ISO-2022-JP"sv, "GB18030"sv, "Big5"sv, "EUC-KR"sv, "windows-1252"sv };
    auto encoding = encodings[input.byte() % array_size(encodings)];
    auto flags = input.byte();
    auto chunk_size = 1 + input.byte();
    auto bom = flags & 1 ? TextCodec::IgnoreBOM::Yes : TextCodec::IgnoreBOM::No;
    auto mode = flags & 2 ? TextCodec::ErrorMode::Fatal : TextCodec::ErrorMode::Replacement;
    auto bytes = input.remaining();
    auto whole = decode(bytes, encoding, bom, mode, max(size_t { 1 }, bytes.size()));
    auto split = decode(bytes, encoding, bom, mode, chunk_size);
    VERIFY(whole.is_error() == split.is_error());
    // Fatal errors can be detected in different calls; compare only final status.
    if (!whole.is_error())
        VERIFY(whole.value() == split.value());
    auto whole_utf16 = decode_utf16(bytes, encoding, bom, mode, max(size_t { 1 }, bytes.size()));
    auto split_utf16 = decode_utf16(bytes, encoding, bom, mode, chunk_size);
    VERIFY(whole_utf16.is_error() == split_utf16.is_error());
    VERIFY(whole.is_error() == whole_utf16.is_error());
    if (!whole_utf16.is_error()) {
        VERIFY(whole_utf16.value() == split_utf16.value());
        VERIFY(whole_utf16.value() == Utf16String::from_utf8(whole.value()));
    }
    return 0;
}
