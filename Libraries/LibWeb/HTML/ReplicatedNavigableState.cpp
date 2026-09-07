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
    TRY(encoder.encode(state.active_session_history_entry_identity));
    TRY(encoder.encode(state.top_level_creation_url));
    TRY(encoder.encode(state.top_level_origin));
    TRY(encoder.encode(state.has_cross_site_ancestor));
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
        .active_session_history_entry_identity = TRY(decoder.decode<Web::HTML::SessionHistoryEntryIdentity>()),
        .top_level_creation_url = TRY(decoder.decode<URL::URL>()),
        .top_level_origin = TRY(decoder.decode<URL::Origin>()),
        .has_cross_site_ancestor = TRY(decoder.decode<bool>()),
    };
}

}
