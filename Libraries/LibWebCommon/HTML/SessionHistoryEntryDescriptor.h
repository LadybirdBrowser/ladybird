/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/RequestReferrer.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/DocumentStateClient.h>
#include <LibWebCommon/HTML/POSTResource.h>
#include <LibWebCommon/HTML/SerializationRecords.h>
#include <LibWebCommon/HTML/SerializedPolicyContainer.h>
#include <LibWebCommon/HTML/SessionHistoryEntryIdentity.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/history.html#scroll-restoration-mode
enum class ScrollRestorationMode {
    // https://html.spec.whatwg.org/multipage/history.html#dom-scrollrestoration-auto
    // The user agent is responsible for restoring the scroll position upon navigation.
    Auto,

    // https://html.spec.whatwg.org/multipage/history.html#dom-scrollrestoration-manual
    // The page is responsible for restoring the scroll position and the user agent does not attempt to do so automatically.
    Manual,
};

struct SessionHistoryNestedHistoryDescriptor;

// IPC-friendly descriptors for the parts of session history entries and document states that can survive
// WebContent process swaps.
//
// https://html.spec.whatwg.org/multipage/browsing-the-web.html#session-history-entry
// https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state
struct SessionHistoryDocumentStateDescriptor {
    // AD-HOC: The spec models shared document state by object identity. The UI-process mirror uses a stable
    //         descriptor ID so entries that share a document state can be reconstructed after IPC.
    CrossProcessId id;
    Variant<SerializedPolicyContainer, DocumentStateClient> history_policy_container { DocumentStateClient::Tag };
    Fetch::Infrastructure::RequestReferrerType request_referrer { Fetch::Infrastructure::RequestReferrer::Client };
    ReferrerPolicy::ReferrerPolicy request_referrer_policy { ReferrerPolicy::DEFAULT_REFERRER_POLICY };
    Optional<URL::Origin> initiator_origin;
    Optional<URL::Origin> origin;
    Optional<URL::URL> about_base_url;
    DocumentResource resource;
    bool reload_pending { false };
    bool ever_populated { false };
    Utf16String navigable_target_name;
    // AD-HOC: Only the UI process uses nested histories, so they are not encoded.
    Vector<SessionHistoryNestedHistoryDescriptor> nested_histories;
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-scroll-position
struct SessionHistoryEntryScrollPositionData {
    // FIXME: Track all restorable scrollable regions. Currently only the viewport is persisted.
    Optional<CSSPixelPoint> viewport_scroll_position;
};

struct SessionHistoryEntryPersistedState {
    // AD-HOC: Persisted state crosses the process boundary separately from its entry, so retain the entry's full
    // identity instead of relying on its Navigation API key, which replaceState() can share with another entry.
    SessionHistoryEntryIdentity entry_identity;
    SessionHistoryEntryScrollPositionData scroll_position_data;
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#session-history-entry
struct SessionHistoryEntryDescriptor {
    i32 step { 0 };
    URL::URL url;
    SessionHistoryDocumentStateDescriptor document_state;
    StorageSerializationRecord classic_history_api_state;
    StorageSerializationRecord navigation_api_state;
    Utf16String navigation_api_key;
    Utf16String navigation_api_id;
    ScrollRestorationMode scroll_restoration_mode { ScrollRestorationMode::Auto };
    SessionHistoryEntryScrollPositionData scroll_position_data;
};

struct PendingSessionHistoryEntryDescriptor {
    URL::URL url;
    SessionHistoryDocumentStateDescriptor document_state;
    StorageSerializationRecord classic_history_api_state;
    StorageSerializationRecord navigation_api_state;
    Utf16String navigation_api_key;
    Utf16String navigation_api_id;
    ScrollRestorationMode scroll_restoration_mode { ScrollRestorationMode::Auto };
    SessionHistoryEntryScrollPositionData scroll_position_data;
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#nested-history
struct SessionHistoryNestedHistoryDescriptor {
    CrossProcessId id;
    Vector<SessionHistoryEntryDescriptor> entries;
};

// https://html.spec.whatwg.org/multipage/history.html#session-history-entry

WEBCOMMON_API SessionHistoryEntryDescriptor create_initial_session_history_entry_descriptor(CrossProcessId document_state_id, Optional<URL::URL> about_base_url, Utf16String navigable_target_name);

WEBCOMMON_API PendingSessionHistoryEntryDescriptor create_pending_session_history_entry_descriptor(SessionHistoryEntryDescriptor);

WEBCOMMON_API SessionHistoryEntryDescriptor create_session_history_entry_descriptor(PendingSessionHistoryEntryDescriptor, i32 step);

WEBCOMMON_API SessionHistoryEntryIdentity session_history_entry_identity(SessionHistoryEntryDescriptor const&);

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::SessionHistoryEntryDescriptor const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::SessionHistoryEntryDescriptor> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::PendingSessionHistoryEntryDescriptor const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::PendingSessionHistoryEntryDescriptor> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::SessionHistoryEntryScrollPositionData const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::SessionHistoryEntryScrollPositionData> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::SessionHistoryEntryPersistedState const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::SessionHistoryEntryPersistedState> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::SessionHistoryDocumentStateDescriptor const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::SessionHistoryDocumentStateDescriptor> decode(Decoder&);

}
