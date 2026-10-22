/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Page/WritingSuggestion.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::WritingSuggestion const& suggestion)
{
    TRY(encoder.encode(suggestion.replacement_start));
    TRY(encoder.encode(suggestion.replacement_length));
    TRY(encoder.encode(suggestion.text));
    return {};
}

template<>
ErrorOr<Web::WritingSuggestion> IPC::decode(Decoder& decoder)
{
    auto replacement_start = TRY(decoder.decode<u32>());
    auto replacement_length = TRY(decoder.decode<u32>());
    auto text = TRY(decoder.decode<Utf16String>());
    return Web::WritingSuggestion { replacement_start, replacement_length, move(text) };
}
