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
#include <LibRequests/Forward.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Forward.h>
#include <LibWebCommon/HTML/CrossOrigin/OpenerPolicyEnforcementResult.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/HTML/NavigationPopulationRequest.h>
#include <LibWebCommon/HTML/PopulatedDocumentOrigin.h>
#include <LibWebCommon/HTML/PreparedNavigationDescriptor.h>
#include <LibWebCommon/HTML/ReplicatedNavigableState.h>
#include <LibWebCommon/HTML/SameDocumentNavigationEntry.h>
#include <LibWebCommon/HTML/SessionHistoryEntryDescriptor.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalNavigation.h>
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
    URL::URL const& active_document_url() const;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-target-name
    Utf16String const& target_name() const;

    RefPtr<CanonicalDocument> document_with_id(Web::HTML::CrossProcessId) const;
    void populate_document(NonnullRefPtr<CanonicalDocumentState>, NonnullRefPtr<CanonicalDocument>, Optional<URL::Origin> inline_content_origin = {});
    // Populates nothing for a document that would hold access the active document lacks.
    void populate_document_for_javascript_url(CanonicalNavigation&, NonnullRefPtr<CanonicalSessionHistoryEntry>, NonnullRefPtr<CanonicalDocument>);
    void did_create_populated_document_with_an_origin_of_its_own(WebContentPage const& host, Web::HTML::CrossProcessId document_id, Web::HTML::PopulatedDocumentOrigin, Web::HTML::EnvironmentId const& environment_id);
    void abandon_populated_document(NonnullRefPtr<CanonicalDocumentState>, CanonicalDocument const&);
    void place_populated_document(CanonicalDocument&, WebContentPage&);

    // The document states holding a populated document: the history entries' of the navigations under way, and the one a
    // history job is to activate.
    template<typename Callback>
    void for_each_populated_document_state(Callback callback) const
    {
        for (auto const& navigation : m_navigations) {
            if (navigation->history_entry && navigation->history_entry->document_state->populated_document.has_value())
                callback(*navigation->history_entry->document_state);
        }
        if (m_document_state_populated_by_history_job)
            callback(*m_document_state_populated_by_history_job);
    }

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-bc
    CanonicalBrowsingContext& active_browsing_context() const;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
    void navigate(URL::URL, Web::HTML::DocumentResource = {}, Web::Bindings::NavigationHistoryBehavior = Web::Bindings::NavigationHistoryBehavior::Auto);
    Web::HTML::PreparedNavigationDescriptor prepare_navigation(URL::URL, Web::HTML::DocumentResource = {}, Web::Bindings::NavigationHistoryBehavior = Web::Bindings::NavigationHistoryBehavior::Auto);
    void begin_navigation(Web::HTML::PreparedNavigationDescriptor);

    CanonicalBrowsingContext::BrowsingContextAndDocument obtain_a_browsing_context_to_use_for_a_navigation_response(NavigationLoader::ResponseDocument const&);
    NonnullRefPtr<CanonicalDocument> create_and_initialize_a_document(NavigationLoader::ResponseDocument const&);

    CanonicalNavigable& append_child(NonnullOwnPtr<CanonicalNavigable>);
    NonnullOwnPtr<CanonicalNavigable> remove_child(CanonicalNavigable&);
    bool is_ancestor_of(CanonicalNavigable const&) const;
    bool allowed_by_sandboxing_to_navigate(CanonicalNavigable const& target, Web::InitiatorSourceSnapshot const& source_snapshot_params) const;
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#snapshotting-target-snapshot-params
    Web::HTML::TargetSnapshotParams snapshot_target_snapshot_params() const;
    IterationDecision for_each_in_inclusive_subtree(Function<IterationDecision(CanonicalNavigable&)> const&);
    IterationDecision for_each_in_subtree(Function<IterationDecision(CanonicalNavigable&)> const&);
    IterationDecision for_each_in_inclusive_subtree(Function<IterationDecision(CanonicalNavigable const&)> const&) const;
    IterationDecision for_each_in_subtree(Function<IterationDecision(CanonicalNavigable const&)> const&) const;

    bool has_remote_host() const;
    WebContentPage& remote_host() const;

    void hand_pending_webdriver_commands_to(WebContentPage& new_host);

    // The process to host a document the navigable is to display, or none for a process of its own. The specification
    // leaves the process running an agent to the user agent: a hosted agent's documents go where it is hosted, and
    // the rest is Ladybird's choice for an agent nobody hosts yet. Local file content has processes of its own.
    RefPtr<WebContentClient> process_to_host(CanonicalDocument const&, Optional<URL::Origin> const& initiator_origin) const;
    // The page to host a document the navigable is to display, in the process to host it. For a child, the page holding
    // the container, the page hosting the displayed document, the process's page for the tab, or a page created for it;
    // for the traversable, see CanonicalTraversable::obtain_page_to_host_traversable.
    ErrorOr<NonnullRefPtr<WebContentPage>> obtain_page_to_host(CanonicalDocument const&, Optional<URL::Origin> const& initiator_origin);

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#attempt-to-populate-the-history-entry's-document
    // The page to run the task step 5 queues, with the document it populates if the response creates one.
    struct PageToPopulateDocument {
        NonnullRefPtr<WebContentPage> page;
        RefPtr<CanonicalDocument> document;
    };
    ErrorOr<PageToPopulateDocument> obtain_page_to_populate_document(CanonicalSessionHistoryEntry&, NavigationLoader&, WebContentPage& page_that_created_navigation_params);

    // A page hosting a document populated for the navigable when it is not the page hosting the displayed one. The
    // displayed document stays with its host until the next is activated, so that it is unloaded there before the
    // container is handed over.
    template<typename Callback>
    void for_each_pending_host(Callback callback) const
    {
        for_each_populated_document_state([&](CanonicalDocumentState const& document_state) {
            if (auto const& host = document_state.populated_document->document->host(); host && host != active_document().host())
                callback(*host);
        });
    }
    bool pending_host_matches(WebContentPage const&) const;
    void discard_pending_host();
    void discard_pending_host(WebContentPage const&);

    Optional<Web::DevicePixelRect> const& viewport_rect() const { return m_viewport_rect; }
    Web::DevicePixelRect const& viewport_intersection() const { return m_viewport_intersection; }
    double device_pixel_ratio() const { return m_device_pixel_ratio; }
    void set_viewport(Web::DevicePixelRect, Web::DevicePixelRect viewport_intersection, double device_pixel_ratio);
    void send_viewport_to_host() const;
    void send_viewport_to(WebContentPage&) const;
    void set_stats_displayed(bool);
    void send_stats_displayed_to_hosts() const;
    void send_stats_displayed_to(WebContentPage&) const;

    // The navigable's replicated state: what its host reports, and what the UI process knows of it.
    Optional<Web::HTML::ReplicatedNavigableState> replicated_state() const;
    void send_replicated_state() const;
    Optional<Web::HTML::HostedNavigableState> const& hosted_state() const { return m_hosted_state; }
    void set_hosted_state(Web::HTML::HostedNavigableState);
    void did_lose_active_document();
    void update_hosted_state(Web::HTML::HostedNavigableState);
    void did_set_opener_browsing_context(Optional<Web::HTML::CrossProcessId> opener_navigable_id);
    // https://html.spec.whatwg.org/multipage/document-sequences.html#has-cross-site-ancestor
    bool active_document_has_cross_site_ancestor() const;
    // Whether the navigable's active session history entry is among its session history entries.
    bool has_session_history_entry_and_ready_for_navigation() const;
    bool is_script_closable() const;
    void document_completely_finished_loading(CanonicalDocument&);
    void update_container_state(Web::HTML::ReplicatedContainerState);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-current-history-entry
    RefPtr<CanonicalSessionHistoryEntry> const& current_session_history_entry() const { return m_current_session_history_entry; }
    void set_current_session_history_entry(RefPtr<CanonicalSessionHistoryEntry> entry) { m_current_session_history_entry = move(entry); }

    // https://html.spec.whatwg.org/multipage/document-sequences.html#nav-active-history-entry
    RefPtr<CanonicalSessionHistoryEntry> const& active_session_history_entry() const { return m_active_session_history_entry; }
    void set_active_session_history_entry(RefPtr<CanonicalSessionHistoryEntry> entry) { m_active_session_history_entry = move(entry); }

    bool current_session_history_entry_is(CanonicalSessionHistoryEntry const&) const;
    bool active_document_is(CanonicalSessionHistoryEntry const&) const;

    // A history operation activated the entry. It commits the navigation it names, or with none named, the ongoing
    // navigation admitted before the operation's sequence number.
    void did_commit_navigation(CanonicalSessionHistoryEntry&, Web::HTML::HostedNavigableState, u64 operation_sequence_number, Optional<Utf16String const&> navigation_id, RefPtr<WebContentPage> host);

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#ongoing-navigation
    // NB: "traversal" names the history operation that set it, so that another operation leaves it alone.
    struct Traversal {
        Web::HTML::CrossProcessId operation_id;

        bool operator==(Traversal const&) const = default;
    };
    using OngoingNavigation = Variant<Empty, Traversal, Utf16String>;
    OngoingNavigation const& ongoing_navigation() const { return m_ongoing_navigation; }
    Optional<Utf16String const&> ongoing_navigation_id() const;

    RefPtr<CanonicalNavigation> navigation_with_id(Utf16String const&) const;
    // The navigation the ongoing navigation names, until its finalization is appended.
    RefPtr<CanonicalNavigation> uncommitted_navigation() const;
    bool has_uncommitted_navigation() const { return uncommitted_navigation(); }
    RefPtr<CanonicalNavigation> navigation_being_finalized() const;

    // Held so that revoking a blob URL cannot take the entry away from a navigation on its way to this navigable, or
    // from the document it loaded. Session history holds none, so a revoked blob URL cannot be traversed back to.
    void retain_blob_url_token(URL::BlobURLEntry::Token);

    void set_ongoing_navigation(CanonicalNavigation::Start);
    void begin_evaluating_javascript_url(URL::URL const&, Utf16String const& navigation_id, WebContentPage& host);
    void set_ongoing_navigation_to_traversal(Web::HTML::CrossProcessId operation_id);
    void clear_ongoing_navigation_traversal(Web::HTML::CrossProcessId operation_id);
    void clear_ongoing_navigation();
    void cancel_navigation_for_client(WebContentClient&);
    void did_finish_finalizing_navigation(Utf16String const& navigation_id);
    void did_cancel_navigation(Utf16String navigation_id);
    Optional<Utf16String> tracked_load_navigation_id() const;

    ActiveDocumentLoad const& active_document_load() const { return m_active_document_load; }
    void clear_active_document_load() { m_active_document_load = {}; }

private:
    void send_completely_finished_loading_to_container() const;

    Web::HTML::CrossProcessId m_id;
    CanonicalNavigable* m_parent { nullptr };
    RefPtr<CanonicalDocument> m_container_document;
    Vector<NonnullOwnPtr<CanonicalNavigable>> m_children;

    Optional<Web::HTML::HostedNavigableState> m_hosted_state;
    void abandon_populated_document(NonnullRefPtr<CanonicalDocumentState>);
    RefPtr<CanonicalDocumentState> m_document_state_populated_by_history_job;
    RefPtr<CanonicalSessionHistoryEntry> m_current_session_history_entry;
    RefPtr<CanonicalSessionHistoryEntry> m_active_session_history_entry;
    void set_the_ongoing_navigation(OngoingNavigation);
    void notify_view_of_ongoing_navigation() const;
    void end_navigation(CanonicalNavigation&);
    OngoingNavigation m_ongoing_navigation;
    Vector<NonnullRefPtr<CanonicalNavigation>> m_navigations;

    RefPtr<WebContentClient> process_to_host_for_site_isolation(CanonicalDocument const&, Optional<URL::Origin> const& initiator_origin) const;
    ErrorOr<NonnullRefPtr<WebContentPage>> obtain_page_to_host_in(RefPtr<WebContentClient> process);

    BlobURLStore* blob_url_store() const;
    BlobURLHandle m_pending_navigation_blob_url;
    BlobURLHandle m_document_blob_url;
    ActiveDocumentLoad m_active_document_load;
    bool m_stats_displayed { false };
    Optional<Web::DevicePixelRect> m_viewport_rect;
    Web::DevicePixelRect m_viewport_intersection;
    double m_device_pixel_ratio { 1 };
};

}
