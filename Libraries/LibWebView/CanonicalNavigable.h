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
#include <LibWebView/CanonicalSessionHistoryEntry.h>
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
        // AD-HOC: The session history entry whose document a navigation reconstructing a child navigable's history
        //         populates.
        RefPtr<CanonicalSessionHistoryEntry> reconstructed_entry {};
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

    explicit CanonicalNavigable(Web::HTML::CrossProcessId id);
    virtual ~CanonicalNavigable();

    virtual bool is_top_level_traversable() const { return false; }

    Web::HTML::CrossProcessId id() const { return m_id; }
    void set_id(Web::HTML::CrossProcessId id) { m_id = id; }

    // The page whose document tree contains this frame: the page hosting its container document. When the frame is
    // local, this page also hosts the frame's active document.
    RefPtr<WebContentPage> reporting_page() const;

    CanonicalNavigable* parent() { return m_parent; }
    CanonicalNavigable const* parent() const { return m_parent; }
    Vector<NonnullOwnPtr<CanonicalNavigable>> const& children() const { return m_children; }

    CanonicalTraversable& top_level_traversable();
    CanonicalTraversable const& top_level_traversable() const;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-container-document
    // A navigable holds its container document, which outlives the navigable's node in the page hosting it: the frames
    // of a document that is gone stay until the page reports their destruction, or the traversable removes them.
    CanonicalDocument* container_document() const { return m_container_document.ptr(); }
    void set_container_document(Badge<CanonicalTraversable>, CanonicalDocument&);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-document
    CanonicalDocument& active_document() const;

    // The document state of the session history entry the navigable is navigating or traversing to, and the document
    // populated for it, which becomes the document state's document when the entry is activated. A document populated
    // for a navigation names it, and goes when the navigation is cleared; one a history job populated goes when the
    // job's operation finishes.
    RefPtr<CanonicalDocumentState> populating_document_state() const;
    RefPtr<CanonicalDocument> pending_document() const;
    void populate_document(NonnullRefPtr<CanonicalDocumentState>, NonnullRefPtr<CanonicalDocument>, Optional<Utf16String> navigation_id = {});
    void abandon_pending_document();
    void abandon_document_populated_for(CanonicalDocumentState const&);
    void place_pending_document(WebContentPage&);

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
    WebContentPage& remote_host() const;

    void hand_pending_webdriver_commands_to(WebContentPage& new_host);

    // The process to host a document the navigable is to display, or none for a process of its own. The specification
    // leaves the process running an agent to the user agent: a hosted agent's documents go where it is hosted, and
    // the rest is Ladybird's choice for an agent nobody hosts yet.
    RefPtr<WebContentClient> process_to_host(CanonicalDocument const&, Optional<URL::Origin> const& initiator_origin) const;
    // The page to host a document the navigable is to display, in the process to host it. For a child, the page holding
    // the container, the page hosting the displayed document, the process's page for the tab, or a page created for it;
    // for the traversable, see CanonicalTraversable::obtain_page_to_host_traversable.
    ErrorOr<NonnullRefPtr<WebContentPage>> obtain_page_to_host(CanonicalDocument const&, Optional<URL::Origin> const& initiator_origin);

    // The page hosting the navigable's next document when it is not the page hosting the displayed one. The displayed
    // document stays with its host until the next is activated, so that it is unloaded there before the container is
    // handed over.
    bool has_pending_host() const;
    bool pending_host_matches(WebContentPage const& page) const { return has_pending_host() && &pending_host() == &page; }
    WebContentPage& pending_host() const;
    // The document the pending host was to display never activated: a page created for it is discarded.
    void discard_pending_host();

    Optional<Compositing::DevicePixelRect> const& viewport_rect() const { return m_viewport_rect; }
    Compositing::DevicePixelRect const& viewport_intersection() const { return m_viewport_intersection; }
    double device_pixel_ratio() const { return m_device_pixel_ratio; }
    void set_viewport(Compositing::DevicePixelRect, Compositing::DevicePixelRect viewport_intersection, double device_pixel_ratio);
    void send_viewport_to_host() const;
    void send_viewport_to(WebContentPage&) const;

    // The navigable's replicated state: what its host reports, and what the UI process knows of it.
    Optional<Web::HTML::ReplicatedNavigableState> replicated_state() const;
    void send_replicated_state() const;
    Optional<Web::HTML::HostedNavigableState> const& hosted_state() const { return m_hosted_state; }
    void set_hosted_state(Web::HTML::HostedNavigableState);
    void update_hosted_state(Web::HTML::HostedNavigableState);
    void did_set_opener_browsing_context(Optional<Web::HTML::CrossProcessId> opener_navigable_id);
    // https://html.spec.whatwg.org/multipage/document-sequences.html#has-cross-site-ancestor
    bool active_document_has_cross_site_ancestor() const;
    // Whether the navigable's active session history entry is among its session history entries.
    bool has_session_history_entry_and_ready_for_navigation() const;
    void active_document_completely_finished_loading();
    void update_container_state(Web::HTML::ReplicatedContainerState);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-current-history-entry
    RefPtr<CanonicalSessionHistoryEntry> const& current_session_history_entry() const { return m_current_session_history_entry; }
    void set_current_session_history_entry(RefPtr<CanonicalSessionHistoryEntry> entry) { m_current_session_history_entry = move(entry); }

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-active-history-entry
    RefPtr<CanonicalSessionHistoryEntry> const& active_session_history_entry() const { return m_active_session_history_entry; }
    void set_active_session_history_entry(RefPtr<CanonicalSessionHistoryEntry> entry) { m_active_session_history_entry = move(entry); }

    bool current_session_history_entry_is(CanonicalSessionHistoryEntry const&) const;
    bool active_document_is(CanonicalSessionHistoryEntry const&) const;

    enum class DidPopulateDocument {
        No,
        Yes,
    };
    void did_commit_navigation(CanonicalSessionHistoryEntry&, Web::HTML::HostedNavigableState, Optional<Utf16String> const& navigation_id, DidPopulateDocument, RefPtr<WebContentPage> host);

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
    CanonicalNavigable* m_parent { nullptr };
    RefPtr<CanonicalDocument> m_container_document;
    Vector<NonnullOwnPtr<CanonicalNavigable>> m_children;

    Optional<Web::HTML::HostedNavigableState> m_hosted_state;
    // AD-HOC: A reload populates the document state of the active session history entry, whose document stays the
    //         navigable's active document until the populated one is activated.
    struct PopulatedDocument {
        NonnullRefPtr<CanonicalDocumentState> document_state;
        NonnullRefPtr<CanonicalDocument> document;
        Optional<Utf16String> navigation_id;
    };
    Optional<PopulatedDocument> m_populated_document;
    RefPtr<CanonicalSessionHistoryEntry> m_current_session_history_entry;
    RefPtr<CanonicalSessionHistoryEntry> m_active_session_history_entry;
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
};

}
