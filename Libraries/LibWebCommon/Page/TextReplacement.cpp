/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Page/TextReplacement.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::TextReplacement const& replacement)
{
    TRY(encoder.encode(replacement.start));
    TRY(encoder.encode(replacement.length));
    TRY(encoder.encode(replacement.text));
    return {};
}

template<>
ErrorOr<Web::TextReplacement> IPC::decode(Decoder& decoder)
{
    auto start = TRY(decoder.decode<u32>());
    auto length = TRY(decoder.decode<u32>());
    auto text = TRY(decoder.decode<Utf16String>());
    return Web::TextReplacement { start, length, move(text) };
}
