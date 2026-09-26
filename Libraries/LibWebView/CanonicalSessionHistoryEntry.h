/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

class CanonicalSessionHistoryEntry;

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#nested-history
struct CanonicalNestedHistory {
    Web::HTML::CrossProcessId id;
    Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> entries;
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-2
class WEBVIEW_API CanonicalDocumentState final : public RefCounted<CanonicalDocumentState> {
public:
    static NonnullRefPtr<CanonicalDocumentState> create(Web::HTML::CrossProcessId);
    static NonnullRefPtr<CanonicalDocumentState> create(Web::HTML::CrossProcessId, NonnullRefPtr<CanonicalDocument>);

    ~CanonicalDocumentState();

    Web::HTML::SessionHistoryDocumentStateDescriptor descriptor() const;

    // NB: The identity the processes hosting the document state's documents give it.
    Web::HTML::CrossProcessId id;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state-document
    RefPtr<CanonicalDocument> document;

    Variant<Web::HTML::SerializedPolicyContainer, Web::HTML::DocumentState::Client> history_policy_container { Web::HTML::DocumentState::Client::Tag };
    Web::Fetch::Infrastructure::Request::ReferrerType request_referrer { Web::Fetch::Infrastructure::Request::Referrer::Client };
    Web::ReferrerPolicy::ReferrerPolicy request_referrer_policy { Web::ReferrerPolicy::DEFAULT_REFERRER_POLICY };
    Optional<URL::Origin> initiator_origin;
    Optional<URL::Origin> origin;
    Optional<URL::URL> about_base_url;
    Web::HTML::DocumentResource resource;
    bool reload_pending { false };
    bool ever_populated { false };
    Utf16String navigable_target_name;
    Vector<CanonicalNestedHistory> nested_histories;

private:
    explicit CanonicalDocumentState(Web::HTML::CrossProcessId);
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#session-history-entry
class WEBVIEW_API CanonicalSessionHistoryEntry final : public RefCounted<CanonicalSessionHistoryEntry> {
public:
    // Entries that name one document state share it, as they share the specification's document state object.
    using DocumentStates = HashMap<Web::HTML::CrossProcessId, NonnullRefPtr<CanonicalDocumentState>>;

    // Whether a descriptor naming a document state that exists updates it, as one from the process that holds the
    // document state does, or refers to it as it is.
    enum class UpdateDocumentState : bool {
        No,
        Yes,
    };

    static NonnullRefPtr<CanonicalSessionHistoryEntry> create(NonnullRefPtr<CanonicalDocumentState>);
    static ErrorOr<NonnullRefPtr<CanonicalSessionHistoryEntry>> create_from_descriptor(Web::HTML::SessionHistoryEntryDescriptor const&);
    static ErrorOr<NonnullRefPtr<CanonicalSessionHistoryEntry>> create_from_descriptor(Web::HTML::SessionHistoryEntryDescriptor const&, DocumentStates&, UpdateDocumentState = UpdateDocumentState::No);

    ~CanonicalSessionHistoryEntry();

    Web::HTML::SessionHistoryEntryDescriptor descriptor() const;
    Web::HTML::SessionHistoryEntryIdentity identity() const { return { document_state->id, navigation_api_id }; }

    i32 step { 0 };
    URL::URL url;
    NonnullRefPtr<CanonicalDocumentState> document_state;
    Web::HTML::StorageSerializationRecord classic_history_api_state;
    Web::HTML::StorageSerializationRecord navigation_api_state;
    Utf16String navigation_api_key;
    Utf16String navigation_api_id;
    Web::HTML::ScrollRestorationMode scroll_restoration_mode { Web::HTML::ScrollRestorationMode::Auto };
    Web::HTML::SessionHistoryEntryScrollPositionData scroll_position_data;

private:
    explicit CanonicalSessionHistoryEntry(NonnullRefPtr<CanonicalDocumentState>);
};

}
