/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/ReplicatedNavigableState.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::ReplicatedNavigableState const& state)
{
    TRY(encoder.encode(state.target_name));
    TRY(encoder.encode(state.active_document_url));
    TRY(encoder.encode(state.active_document_origin));
    TRY(encoder.encode(state.active_document_is_fully_active));
    return {};
}

template<>
ErrorOr<Web::HTML::ReplicatedNavigableState> decode(Decoder& decoder)
{
    return Web::HTML::ReplicatedNavigableState {
        .target_name = TRY(decoder.decode<Utf16String>()),
        .active_document_url = TRY(decoder.decode<URL::URL>()),
        .active_document_origin = TRY(decoder.decode<URL::Origin>()),
        .active_document_is_fully_active = TRY(decoder.decode<bool>()),
    };
}

}
