/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/SessionHistoryEntry.h>

namespace Web::HTML {

struct SameDocumentNavigationEntry {
    URL::URL url;
    CrossProcessId document_state_id;
    StorageSerializationRecord classic_history_api_state;
    StorageSerializationRecord navigation_api_state;
    Utf16String navigation_api_key;
    Utf16String navigation_api_id;
    ScrollRestorationMode scroll_restoration_mode { ScrollRestorationMode::Auto };
    SessionHistoryEntryScrollPositionData scroll_position_data;
};

WEB_API SameDocumentNavigationEntry create_same_document_navigation_entry(SessionHistoryEntry const&);
WEB_API SessionHistoryEntryIdentity session_history_entry_identity(SameDocumentNavigationEntry const&);

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::SameDocumentNavigationEntry const&);

template<>
WEB_API ErrorOr<Web::HTML::SameDocumentNavigationEntry> decode(Decoder&);

}
