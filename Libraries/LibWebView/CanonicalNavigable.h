/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/IterationDecision.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibCompositing/PageId.h>
#include <LibCompositing/PixelUnits.h>
#include <LibRequests/Forward.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CrossOrigin/OpenerPolicyEnforcementResult.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/NavigationPopulationRequest.h>
#include <LibWeb/HTML/ReplicatedNavigableState.h>
#include <LibWeb/HTML/SameDocumentNavigationEntry.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/NavigationLoader.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

class WEBVIEW_API CanonicalNavigable
    : public Weakable<CanonicalNavigable> {
public:
    AK_ALLOC_WITH_KMALLOC;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#ongoing-navigation
    // A navigation transaction, live from its admission until its target document is activated or the
    // navigation is canceled or superseded.
    struct OngoingNavigation {
        enum class Phase : u8 {
            Started,
            AwaitingUnloadCheck,
            Populating,
            AwaitingResponseBody,
        };

        Optional<URL::URL> url {};
        Optional<Utf16String> navigation_id {};
        Optional<Web::HTML::NavigationStartRequest> start_request {};
        u64 sequence_number { 0 };
        bool has_started { false };
        Phase phase { Phase::Started };
        OwnPtr<NavigationLoader> loader {};
        RefPtr<WebContentPage> population_worker {};
        RefPtr<WebContentPage> host {};
        // The Document the navigation's response creates, until it is made active.
        RefPtr<CanonicalDocument> document {};
    };

    // The active document's load, tracked from the document's activation until WebContent reports that
    // the load finished, failed, or was canceled. Kept apart from the ongoing navigation because the two
    // overlap: a newer navigation can be admitted, and a traversal can run, while the active document's
    // load is still in progress. A navigable has an active document from birth, so this always exists;
    // the navigation id is empty when no UI-recorded navigation produced the document (the initial
    // about:blank, or a document populated by a traversal).
    struct ActiveDocumentLoad {
        Optional<Utf16String> navigation_id {};
    };

    CanonicalNavigable(Web::HTML::CrossProcessId id, RefPtr<WebContentPage> reporting_page);
    virtual ~CanonicalNavigable();

    virtual bool is_top_level_traversable() const { return false; }

    Web::HTML::CrossProcessId id() const { return m_id; }
    void set_id(Web::HTML::CrossProcessId id) { m_id = id; }

    // The page whose document tree contains this frame. When the frame is local, this page also hosts the frame's
    // active document.
    RefPtr<WebContentPage> const& reporting_page() const { return m_reporting_page; }

    CanonicalNavigable* parent() { return m_parent; }
    CanonicalNavigable const* parent() const { return m_parent; }
    Vector<NonnullOwnPtr<CanonicalNavigable>> const& children() const { return m_children; }

    CanonicalTraversable& top_level_traversable();
    CanonicalTraversable const& top_level_traversable() const;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-document
    bool has_active_document() const { return m_active_document; }
    CanonicalDocument& active_document() const;
    void set_active_document(NonnullRefPtr<CanonicalDocument>);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-bc
    CanonicalBrowsingContext& active_browsing_context() const;

    CanonicalBrowsingContext::BrowsingContextAndDocument obtain_a_browsing_context_to_use_for_a_navigation_response(Web::HTML::OpenerPolicyEnforcementResult const&);
    NonnullRefPtr<CanonicalDocument> create_and_initialize_a_document(NavigationLoader::ResponseDocument const&);

    CanonicalNavigable& append_child(NonnullOwnPtr<CanonicalNavigable>);
    NonnullOwnPtr<CanonicalNavigable> remove_child(CanonicalNavigable&);
    bool is_ancestor_of(CanonicalNavigable const&) const;
    bool allowed_by_sandboxing_to_navigate(CanonicalNavigable const& target, Web::InitiatorSourceSnapshot const& source_snapshot_params) const;
    IterationDecision for_each_in_inclusive_subtree(Function<IterationDecision(CanonicalNavigable&)> const&);
    IterationDecision for_each_in_subtree(Function<IterationDecision(CanonicalNavigable&)> const&);
    IterationDecision for_each_in_inclusive_subtree(Function<IterationDecision(CanonicalNavigable const&)> const&) const;
    IterationDecision for_each_in_subtree(Function<IterationDecision(CanonicalNavigable const&)> const&) const;

    bool has_remote_host() const;
    bool is_hosted_by(WebContentPage const&) const;
    WebContentPage& remote_host() const;

    void detach_remote_host();
    void hand_pending_webdriver_commands_to(WebContentPage& new_host);

    // The page chosen to host the navigable's next document, from the response that names the document
    // until the document is activated. The displayed document stays with its host until then, so that it is
    // unloaded there before the container is handed over.
    bool has_pending_host() const { return m_pending_host; }
    bool pending_host_matches(WebContentPage const& page) const { return m_pending_host.ptr() == &page; }
    WebContentPage& pending_host() const;
    void set_pending_host(NonnullRefPtr<WebContentPage>);
    // The pending host took the container over, so its page is no longer pending.
    void clear_pending_host();
    // The document the pending host was to display never activated: a page created for it is discarded.
    void discard_pending_host();

    Optional<Compositing::DevicePixelRect> const& viewport_rect() const { return m_viewport_rect; }
    Compositing::DevicePixelRect const& viewport_intersection() const { return m_viewport_intersection; }
    double device_pixel_ratio() const { return m_device_pixel_ratio; }
    void set_viewport(Compositing::DevicePixelRect, Compositing::DevicePixelRect viewport_intersection, double device_pixel_ratio);
    void send_viewport_to_host() const;
    void send_viewport_to(WebContentPage&) const;

    Optional<Web::HTML::ReplicatedNavigableState> const& replicated_state() const { return m_replicated_state; }
    void set_replicated_state(Web::HTML::ReplicatedNavigableState);
    void update_replicated_state(Web::HTML::ReplicatedNavigableState);
    void active_document_completely_finished_loading();
    void update_container_state(Web::HTML::ReplicatedContainerState);

    Optional<Web::HTML::SessionHistoryEntryIdentity> const& current_session_history_entry_identity() const { return m_current_session_history_entry_identity; }
    Optional<Web::HTML::SessionHistoryEntryIdentity> const& active_session_history_entry_identity() const { return m_active_session_history_entry_identity; }
    void set_current_session_history_entry(Web::HTML::SessionHistoryEntryDescriptor const&);
    void set_current_session_history_entry_identity(Optional<Web::HTML::SessionHistoryEntryIdentity> identity) { m_current_session_history_entry_identity = move(identity); }
    void set_active_session_history_entry(Web::HTML::SessionHistoryEntryDescriptor const&);
    void set_active_session_history_entry_identity(Web::HTML::SessionHistoryEntryIdentity identity) { m_active_session_history_entry_identity = move(identity); }
    void clear_active_session_history_entry_identity() { m_active_session_history_entry_identity = {}; }
    bool current_session_history_entry_is(Web::HTML::SessionHistoryEntryDescriptor const&) const;
    bool active_document_is(Web::HTML::SessionHistoryEntryDescriptor const&) const;

    // AD-HOC: A synchronous same-document entry is script-addressable in WebContent before its queued spec
    // finalization runs. Keep its canonical staging state on the corresponding tree node until that queue position.
    struct PendingSameDocumentSessionHistoryEntry {
        Web::HTML::CrossProcessId operation_id;
        Web::HTML::SameDocumentNavigationEntry entry;
    };

    void stage_same_document_session_history_entry(Web::HTML::CrossProcessId operation_id, Web::HTML::SameDocumentNavigationEntry);
    Optional<Web::HTML::SameDocumentNavigationEntry> take_pending_same_document_session_history_entry(Web::HTML::CrossProcessId operation_id, Web::HTML::SessionHistoryEntryIdentity const&);
    bool update_pending_same_document_session_history_entry(Web::HTML::SessionHistoryEntryIdentity const&, Function<void(Web::HTML::SameDocumentNavigationEntry&)> const&);
    bool has_pending_same_document_session_history_entry(Web::HTML::SessionHistoryEntryIdentity const&) const;
    void remove_pending_same_document_session_history_entries(Web::HTML::CrossProcessId operation_id);
    Vector<PendingSameDocumentSessionHistoryEntry> take_pending_same_document_session_history_entries();
    void append_pending_same_document_session_history_entries(Vector<PendingSameDocumentSessionHistoryEntry>);
    Vector<PendingSameDocumentSessionHistoryEntry> const& pending_same_document_session_history_entries() const { return m_pending_same_document_session_history_entries; }

    enum class DidPopulateDocument {
        No,
        Yes,
    };
    void did_commit_navigation(Web::HTML::ReplicatedNavigableState, Optional<Utf16String> const& navigation_id, DidPopulateDocument, RefPtr<CanonicalDocument>, RefPtr<WebContentPage> host);

    Optional<OngoingNavigation>& ongoing_navigation() { return m_ongoing_navigation; }
    Optional<OngoingNavigation> const& ongoing_navigation() const { return m_ongoing_navigation; }
    bool ongoing_navigation_is_traversal() const { return m_ongoing_navigation_traversal_operation_id.has_value(); }
    OngoingNavigation& ensure_ongoing_navigation();

    // Held so that revoking a blob URL cannot take the entry away from a navigation on its way to this navigable, or
    // from the document it loaded. Session history holds none, so a revoked blob URL cannot be traversed back to.
    void retain_blob_url_token(URL::BlobURLEntry::Token);

    void set_ongoing_navigation(OngoingNavigation);
    void set_ongoing_navigation_to_traversal(Web::HTML::CrossProcessId operation_id);
    void clear_ongoing_navigation_traversal(Web::HTML::CrossProcessId operation_id);
    virtual void clear_ongoing_navigation();
    void clear_ongoing_navigation_state();
    void set_navigation_population_worker(WebContentPage&);
    bool navigation_population_matches(WebContentPage const&, Utf16String const& navigation_id) const;
    bool navigation_population_worker_matches(WebContentPage const&) const;
    void set_navigation_host(WebContentPage&);
    bool navigation_host_matches(WebContentPage const&) const;
    bool navigation_owner_matches(WebContentPage const&) const;
    bool navigation_transaction_matches(Utf16String const&, WebContentPage const&) const;
    bool cancel_navigation_transaction_for_client(WebContentClient&);
    void did_finish_navigation_transaction(Optional<Utf16String> const&, Web::HTML::HistoryStepResult);
    bool has_uncommitted_navigation() const { return m_ongoing_navigation.has_value(); }
    bool matches_ongoing_navigation(Optional<Utf16String> const& navigation_id) const;

    ActiveDocumentLoad const& active_document_load() const { return m_active_document_load; }
    void clear_active_document_load() { m_active_document_load = {}; }

private:
    Web::HTML::CrossProcessId m_id;
    RefPtr<WebContentPage> m_reporting_page;
    CanonicalNavigable* m_parent { nullptr };
    Vector<NonnullOwnPtr<CanonicalNavigable>> m_children;

    Optional<Web::HTML::ReplicatedNavigableState> m_replicated_state;
    RefPtr<CanonicalDocument> m_active_document;
    Optional<Web::HTML::SessionHistoryEntryIdentity> m_current_session_history_entry_identity;
    Optional<Web::HTML::SessionHistoryEntryIdentity> m_active_session_history_entry_identity;
    Vector<PendingSameDocumentSessionHistoryEntry> m_pending_same_document_session_history_entries;
    Optional<OngoingNavigation> m_ongoing_navigation;

    BlobURLStore* blob_url_store() const;
    BlobURLHandle m_pending_navigation_blob_url;
    BlobURLHandle m_navigation_blob_url;
    BlobURLHandle m_document_blob_url;
    Optional<Web::HTML::CrossProcessId> m_ongoing_navigation_traversal_operation_id;
    ActiveDocumentLoad m_active_document_load;
    Optional<Compositing::DevicePixelRect> m_viewport_rect;
    Compositing::DevicePixelRect m_viewport_intersection;
    double m_device_pixel_ratio { 1 };

    RefPtr<WebContentPage> m_pending_host;
};

}
