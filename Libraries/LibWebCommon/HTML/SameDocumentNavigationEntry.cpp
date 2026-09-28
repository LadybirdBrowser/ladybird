/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/HTML/SameDocumentNavigationEntry.h>

namespace Web::HTML {

SessionHistoryEntryIdentity session_history_entry_identity(SameDocumentNavigationEntry const& entry)
{
    return {
        .document_state_id = entry.document_state_id,
        .navigation_api_id = entry.navigation_api_id,
    };
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::SameDocumentNavigationEntry const& entry)
{
    TRY(encoder.encode(entry.url));
    TRY(encoder.encode(entry.document_state_id));
    TRY(encoder.encode(entry.classic_history_api_state));
    TRY(encoder.encode(entry.navigation_api_state));
    TRY(encoder.encode(entry.navigation_api_key));
    TRY(encoder.encode(entry.navigation_api_id));
    TRY(encoder.encode(entry.scroll_restoration_mode));
    TRY(encoder.encode(entry.scroll_position_data));
    return {};
}

template<>
ErrorOr<Web::HTML::SameDocumentNavigationEntry> IPC::decode(Decoder& decoder)
{
    return Web::HTML::SameDocumentNavigationEntry {
        .url = TRY(decoder.decode<URL::URL>()),
        .document_state_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .classic_history_api_state = TRY(decoder.decode<Web::HTML::StorageSerializationRecord>()),
        .navigation_api_state = TRY(decoder.decode<Web::HTML::StorageSerializationRecord>()),
        .navigation_api_key = TRY(decoder.decode<Utf16String>()),
        .navigation_api_id = TRY(decoder.decode<Utf16String>()),
        .scroll_restoration_mode = TRY(decoder.decode<Web::HTML::ScrollRestorationMode>()),
        .scroll_position_data = TRY(decoder.decode<Web::HTML::SessionHistoryEntryScrollPositionData>()),
    };
}
