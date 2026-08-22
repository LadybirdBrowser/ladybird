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
#include <AK/Weakable.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/ReplicatedNavigableState.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/PixelUnits.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API CanonicalNavigable
    : public Weakable<CanonicalNavigable> {
public:
    enum class HostLocality : u8 {
        Local,
        Remote,
    };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#ongoing-navigation
    struct OngoingNavigation {
        Optional<URL::URL> url {};
        Optional<Utf16String> navigation_id {};
        bool has_started { false };
        bool uses_replacement_process { false };
        bool is_uncommitted { false };
        HostLocality target_locality { HostLocality::Local };
        Optional<u64> remote_page_id {};
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
    bool current_session_history_entry_is(Web::HTML::SessionHistoryEntryDescriptor const&) const;
    bool active_document_is(Web::HTML::SessionHistoryEntryDescriptor const&) const;

    void did_commit_navigation(Web::HTML::ReplicatedNavigableState);

    Optional<OngoingNavigation>& ongoing_navigation() { return m_ongoing_navigation; }
    Optional<OngoingNavigation> const& ongoing_navigation() const { return m_ongoing_navigation; }
    OngoingNavigation& ensure_ongoing_navigation();
    void clear_ongoing_navigation() { m_ongoing_navigation.clear(); }
    bool has_uncommitted_navigation() const { return m_ongoing_navigation.has_value() && m_ongoing_navigation->is_uncommitted; }
    bool has_matching_ongoing_navigation(URL::URL const&, HostLocality) const;
    bool matches_ongoing_navigation(Optional<Utf16String> const& navigation_id) const;

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
    Optional<OngoingNavigation> m_ongoing_navigation;
    Optional<Web::DevicePixelRect> m_viewport_rect;
    double m_device_pixel_ratio { 1 };

    HostLocality m_host_locality { HostLocality::Local };
    RefPtr<WebContentClient> m_remote_client;
    u64 m_remote_page_id { 0 };
};

}
