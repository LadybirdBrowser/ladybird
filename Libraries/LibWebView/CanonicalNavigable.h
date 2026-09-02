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
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/NavigationPopulationRequest.h>
#include <LibWeb/HTML/ReplicatedNavigableState.h>
#include <LibWeb/HTML/SameDocumentNavigationEntry.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/PixelUnits.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>
#include <LibWebView/NavigationLoader.h>

namespace WebView {

class WEBVIEW_API CanonicalNavigable
    : public Weakable<CanonicalNavigable> {
public:
    enum class HostLocality : u8 {
        Local,
        Remote,
    };

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
        Optional<URL::URL> current_url {};
        Optional<Web::NavigationTarget> target {};
        Optional<Utf16String> navigation_id {};
        Optional<Web::HTML::NavigationStartRequest> start_request {};
        u64 sequence_number { 0 };
        bool has_started { false };
        Phase phase { Phase::Started };
        OwnPtr<NavigationLoader> loader {};
        WeakPtr<WebContentClient> population_worker_client {};
        u64 population_worker_page_id { 0 };
        WeakPtr<WebContentClient> host_client {};
        u64 host_page_id { 0 };
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

    CanonicalNavigable(Web::HTML::CrossProcessId id, Optional<Web::HTML::CrossProcessId> parent_id, RefPtr<WebContentClient> reporting_client, u64 reporting_page_id);
    virtual ~CanonicalNavigable();

    virtual bool is_top_level_traversable() const { return false; }

    Web::HTML::CrossProcessId id() const { return m_id; }
    Optional<Web::HTML::CrossProcessId> parent_id() const { return m_parent_id; }
    void set_id(Web::HTML::CrossProcessId id) { m_id = id; }

    // The WebContent process and page whose document tree contains this frame. When the
    // frame is local, this process also hosts the frame's active document.
    WebContentClient& reporting_client() const;
    WebContentClient* reporting_client_if_any() const { return m_reporting_client.ptr(); }
    u64 reporting_page_id() const { return m_reporting_page_id; }

    CanonicalNavigable* parent() { return m_parent; }
    CanonicalNavigable const* parent() const { return m_parent; }
    Vector<NonnullOwnPtr<CanonicalNavigable>> const& children() const { return m_children; }

    CanonicalTraversable& top_level_traversable();
    CanonicalTraversable const& top_level_traversable() const;

    CanonicalNavigable& append_child(NonnullOwnPtr<CanonicalNavigable>);
    NonnullOwnPtr<CanonicalNavigable> remove_child(CanonicalNavigable&);
    bool is_ancestor_of(CanonicalNavigable const&) const;
    bool allowed_by_sandboxing_to_navigate(CanonicalNavigable const& target, Web::InitiatorSourceSnapshot const& source_snapshot_params) const;
    IterationDecision for_each_in_inclusive_subtree(Function<IterationDecision(CanonicalNavigable&)> const&);
    IterationDecision for_each_in_subtree(Function<IterationDecision(CanonicalNavigable&)> const&);
    IterationDecision for_each_in_inclusive_subtree(Function<IterationDecision(CanonicalNavigable const&)> const&) const;
    IterationDecision for_each_in_subtree(Function<IterationDecision(CanonicalNavigable const&)> const&) const;

    bool has_remote_host() const { return m_host_locality == HostLocality::Remote && m_remote_client && m_remote_page_id != 0; }
    bool is_hosted_by(WebContentClient const&, u64 page_id) const;
    WebContentClient& remote_host_client() const;
    u64 remote_host_page_id() const { return m_remote_page_id; }

    void set_remote_host(NonnullRefPtr<WebContentClient>, u64 remote_page_id);
    void detach_remote_host();

    Optional<Web::DevicePixelRect> const& viewport_rect() const { return m_viewport_rect; }
    double device_pixel_ratio() const { return m_device_pixel_ratio; }
    void set_viewport(Web::DevicePixelRect, double device_pixel_ratio);

    Optional<Web::HTML::ReplicatedNavigableState> const& replicated_state() const { return m_replicated_state; }
    void set_replicated_state(Web::HTML::ReplicatedNavigableState);

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

    void did_commit_navigation(Web::HTML::ReplicatedNavigableState, Optional<Utf16String> const& navigation_id);

    Optional<OngoingNavigation>& ongoing_navigation() { return m_ongoing_navigation; }
    Optional<OngoingNavigation> const& ongoing_navigation() const { return m_ongoing_navigation; }
    bool ongoing_navigation_is_traversal() const { return m_ongoing_navigation_traversal_operation_id.has_value(); }
    OngoingNavigation& ensure_ongoing_navigation();
    void set_ongoing_navigation(OngoingNavigation);
    void set_ongoing_navigation_to_traversal(Web::HTML::CrossProcessId operation_id);
    void clear_ongoing_navigation_traversal(Web::HTML::CrossProcessId operation_id);
    void clear_ongoing_navigation();
    void set_navigation_population_worker(WebContentClient&, u64 page_id);
    bool navigation_population_matches(WebContentClient const&, u64 page_id, Utf16String const& navigation_id) const;
    bool navigation_population_worker_matches(WebContentClient const&, u64 page_id) const;
    void set_navigation_host(WebContentClient&, u64 page_id);
    bool navigation_host_matches(WebContentClient const&, u64 page_id) const;
    bool navigation_owner_matches(WebContentClient const&, u64 page_id) const;
    bool navigation_transaction_matches(Utf16String const&, WebContentClient const&, u64 page_id) const;
    bool cancel_navigation_transaction_for_client(WebContentClient&);
    void did_finish_navigation_transaction(Optional<Utf16String> const&, Web::HTML::HistoryStepResult);
    bool has_uncommitted_navigation() const { return m_ongoing_navigation.has_value(); }
    bool matches_ongoing_navigation(Optional<Utf16String> const& navigation_id) const;

    ActiveDocumentLoad const& active_document_load() const { return m_active_document_load; }
    void clear_active_document_load() { m_active_document_load = {}; }

private:
    Web::HTML::CrossProcessId m_id;
    Optional<Web::HTML::CrossProcessId> m_parent_id;
    RefPtr<WebContentClient> m_reporting_client;
    u64 m_reporting_page_id { 0 };
    CanonicalNavigable* m_parent { nullptr };
    Vector<NonnullOwnPtr<CanonicalNavigable>> m_children;

    Optional<Web::HTML::ReplicatedNavigableState> m_replicated_state;
    Optional<Web::HTML::SessionHistoryEntryIdentity> m_current_session_history_entry_identity;
    Optional<Web::HTML::SessionHistoryEntryIdentity> m_active_session_history_entry_identity;
    Vector<PendingSameDocumentSessionHistoryEntry> m_pending_same_document_session_history_entries;
    Optional<OngoingNavigation> m_ongoing_navigation;
    Optional<Web::HTML::CrossProcessId> m_ongoing_navigation_traversal_operation_id;
    ActiveDocumentLoad m_active_document_load;
    Optional<Web::DevicePixelRect> m_viewport_rect;
    double m_device_pixel_ratio { 1 };

    HostLocality m_host_locality { HostLocality::Local };
    RefPtr<WebContentClient> m_remote_client;
    u64 m_remote_page_id { 0 };
};

}
