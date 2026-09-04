/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalNavigable.h>

#include <LibWeb/HTML/HistoryOperation.h>
#include <LibWeb/Page/ViewportIsFullscreen.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

CanonicalNavigable::CanonicalNavigable(Web::HTML::CrossProcessId id, Optional<Web::HTML::CrossProcessId> parent_id, RefPtr<WebContentClient> reporting_client, u64 reporting_page_id)
    : m_id(id)
    , m_parent_id(parent_id)
    , m_reporting_client(move(reporting_client))
    , m_reporting_page_id(reporting_page_id)
{
}

CanonicalBrowsingContext& CanonicalNavigable::active_browsing_context() const
{
    // A navigable's active browsing context is its active document's browsing context.
    // NB: Only a top-level browsing context group switch changes it, so it is kept with the navigable.
    VERIFY(m_active_browsing_context);
    return *m_active_browsing_context;
}

void CanonicalNavigable::set_active_browsing_context(NonnullRefPtr<CanonicalBrowsingContext> browsing_context)
{
    m_active_browsing_context = move(browsing_context);
}

// https://html.spec.whatwg.org/multipage/browsers.html#obtain-browsing-context-navigation
NonnullRefPtr<CanonicalBrowsingContext> CanonicalNavigable::obtain_a_browsing_context_to_use_for_a_navigation_response(Web::HTML::OpenerPolicyEnforcementResult const& coop_enforcement_result)
{
    // 1. Let browsingContext be navigationParams's navigable's active browsing context.
    NonnullRefPtr browsing_context = active_browsing_context();

    // 2. If browsingContext is not a top-level browsing context, then return browsingContext.
    if (!is_top_level_traversable())
        return browsing_context;

    // 3. Let coopEnforcementResult be navigationParams's COOP enforcement result.
    // 4. Let swapGroup be coopEnforcementResult's needs a browsing context group switch.
    auto swap_group = coop_enforcement_result.needs_a_browsing_context_group_switch;

    // NB: Steps 5-8 only affect swapGroup through optional choices. This implementation does not take them.

    // 9. If swapGroup is false, then:
    if (!swap_group) {
        // FIXME: 1. If coopEnforcementResult's would need a browsing context group switch due to report-only is true,
        //           set browsingContext's virtual browsing context group ID to a new unique identifier.

        // 2. Return browsingContext.
        return browsing_context;
    }

    // 10. Let newBrowsingContext be the first return value of creating a new top-level browsing context and document.
    // NB: The navigation response's document replaces that document before any process creates it.
    auto new_browsing_context = CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(URL::Origin::create_opaque(), {});

    // 11. Let navigationCOOP be navigationParams's cross-origin opener policy.
    // FIXME: 12. If navigationCOOP's value is "same-origin-plus-COEP", then set newBrowsingContext's group's
    //            cross-origin isolation mode to either "logical" or "concrete". The choice of which is
    //            implementation-defined.

    // 13. Let sandboxFlags be a clone of navigationParams's final sandboxing flag set.
    // FIXME: 14. If sandboxFlags is not empty, then:
    //            1. Assert: navigationCOOP's value is "unsafe-none".
    //            2. Assert: newBrowsingContext's popup sandboxing flag set is empty.
    //            3. Set newBrowsingContext's popup sandboxing flag set to sandboxFlags.

    // 15. Return newBrowsingContext.
    return new_browsing_context;
}

CanonicalNavigable::~CanonicalNavigable()
{
    clear_ongoing_navigation();
}

WebContentClient& CanonicalNavigable::reporting_client() const
{
    VERIFY(m_reporting_client);
    return *m_reporting_client;
}

bool CanonicalNavigable::is_hosted_by(WebContentClient const& client, u64 page_id) const
{
    if (m_host_locality == HostLocality::Remote)
        return m_remote_client.ptr() == &client && m_remote_page_id == page_id;
    return m_reporting_client.ptr() == &client && m_reporting_page_id == page_id;
}

void CanonicalNavigable::stage_same_document_session_history_entry(Web::HTML::CrossProcessId operation_id, Web::HTML::SameDocumentNavigationEntry entry)
{
    m_pending_same_document_session_history_entries.append({ operation_id, move(entry) });
}

Optional<Web::HTML::SameDocumentNavigationEntry> CanonicalNavigable::take_pending_same_document_session_history_entry(Web::HTML::CrossProcessId operation_id, Web::HTML::SessionHistoryEntryIdentity const& entry_identity)
{
    for (size_t i = 0; i < m_pending_same_document_session_history_entries.size(); ++i) {
        auto const& pending_entry = m_pending_same_document_session_history_entries[i];
        if (pending_entry.operation_id == operation_id
            && Web::HTML::session_history_entry_identity(pending_entry.entry) == entry_identity)
            return m_pending_same_document_session_history_entries.take(i).entry;
    }
    return {};
}

bool CanonicalNavigable::update_pending_same_document_session_history_entry(Web::HTML::SessionHistoryEntryIdentity const& entry_identity, Function<void(Web::HTML::SameDocumentNavigationEntry&)> const& update_entry)
{
    for (auto& pending_entry : m_pending_same_document_session_history_entries.in_reverse()) {
        if (Web::HTML::session_history_entry_identity(pending_entry.entry) != entry_identity)
            continue;
        update_entry(pending_entry.entry);
        return true;
    }
    return false;
}

bool CanonicalNavigable::has_pending_same_document_session_history_entry(Web::HTML::SessionHistoryEntryIdentity const& entry_identity) const
{
    for (auto const& pending_entry : m_pending_same_document_session_history_entries) {
        if (Web::HTML::session_history_entry_identity(pending_entry.entry) == entry_identity)
            return true;
    }
    return false;
}

void CanonicalNavigable::remove_pending_same_document_session_history_entries(Web::HTML::CrossProcessId operation_id)
{
    m_pending_same_document_session_history_entries.remove_all_matching([&](auto const& pending_entry) {
        return pending_entry.operation_id == operation_id;
    });
}

Vector<CanonicalNavigable::PendingSameDocumentSessionHistoryEntry> CanonicalNavigable::take_pending_same_document_session_history_entries()
{
    return move(m_pending_same_document_session_history_entries);
}

void CanonicalNavigable::append_pending_same_document_session_history_entries(Vector<PendingSameDocumentSessionHistoryEntry> entries)
{
    m_pending_same_document_session_history_entries.extend(move(entries));
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-top
CanonicalTraversable& CanonicalNavigable::top_level_traversable()
{
    // 1. Let navigable be inputNavigable.
    auto* navigable = this;

    // 2. While navigable's parent is not null, set navigable to navigable's parent.
    while (navigable->parent())
        navigable = navigable->parent();

    // 3. Return navigable.
    VERIFY(navigable->is_top_level_traversable());
    return static_cast<CanonicalTraversable&>(*navigable);
}

CanonicalTraversable const& CanonicalNavigable::top_level_traversable() const
{
    return const_cast<CanonicalNavigable&>(*this).top_level_traversable();
}

CanonicalNavigable& CanonicalNavigable::append_child(NonnullOwnPtr<CanonicalNavigable> child)
{
    VERIFY(!child->m_parent);
    child->m_parent = this;
    m_children.append(move(child));
    return *m_children.last();
}

NonnullOwnPtr<CanonicalNavigable> CanonicalNavigable::remove_child(CanonicalNavigable& child)
{
    for (size_t i = 0; i < m_children.size(); ++i) {
        if (m_children[i].ptr() != &child)
            continue;

        auto removed_child = m_children.take(i);
        VERIFY(removed_child->m_parent == this);
        removed_child->m_parent = nullptr;
        return removed_child;
    }

    VERIFY_NOT_REACHED();
}

bool CanonicalNavigable::is_ancestor_of(CanonicalNavigable const& potential_descendant) const
{
    for (auto const* parent = potential_descendant.parent(); parent; parent = parent->parent()) {
        if (parent == this)
            return true;
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#allowed-to-navigate
bool CanonicalNavigable::allowed_by_sandboxing_to_navigate(CanonicalNavigable const& target, Web::InitiatorSourceSnapshot const& source_snapshot_params) const
{
    auto const& source = *this;

    // 1. If source is target, then return true.
    if (&source == &target)
        return true;

    // 2. If source is an ancestor of target, then return true.
    if (source.is_ancestor_of(target))
        return true;

    // 3. If target is an ancestor of source, then:
    if (target.is_ancestor_of(source)) {
        // 1. If target is not a top-level traversable, then return true.
        if (!target.is_top_level_traversable())
            return true;

        // 2. If sourceSnapshotParams's has transient activation is true, and sourceSnapshotParams's sandboxing flags's
        //    sandboxed top-level navigation with user activation browsing context flag is set, then return false.
        if (source_snapshot_params.has_transient_activation
            && has_flag(source_snapshot_params.sandboxing_flags, Web::HTML::SandboxingFlagSet::SandboxedTopLevelNavigationWithUserActivation)) {
            return false;
        }

        // 3. If sourceSnapshotParams's has transient activation is false, and sourceSnapshotParams's sandboxing flags's
        //    sandboxed top-level navigation without user activation browsing context flag is set, then return false.
        if (!source_snapshot_params.has_transient_activation
            && has_flag(source_snapshot_params.sandboxing_flags, Web::HTML::SandboxingFlagSet::SandboxedTopLevelNavigationWithoutUserActivation)) {
            return false;
        }

        // 4. Return true.
        return true;
    }

    // 4. If target is a top-level traversable:
    if (target.is_top_level_traversable()) {
        // FIXME: 1. If source is the one permitted sandboxed navigator of target, then return true.

        // 2. If sourceSnapshotParams's sandboxing flags's sandboxed navigation browsing context flag is set, then return false.
        if (has_flag(source_snapshot_params.sandboxing_flags, Web::HTML::SandboxingFlagSet::SandboxedNavigation))
            return false;

        // 3. Return true.
        return true;
    }

    // 5. If sourceSnapshotParams's sandboxing flags's sandboxed navigation browsing context flag is set, then return false.
    if (has_flag(source_snapshot_params.sandboxing_flags, Web::HTML::SandboxingFlagSet::SandboxedNavigation))
        return false;

    // 6. Return true.
    return true;
}

IterationDecision CanonicalNavigable::for_each_in_inclusive_subtree(Function<IterationDecision(CanonicalNavigable&)> const& callback)
{
    if (callback(*this) == IterationDecision::Break)
        return IterationDecision::Break;

    return for_each_in_subtree(callback);
}

IterationDecision CanonicalNavigable::for_each_in_subtree(Function<IterationDecision(CanonicalNavigable&)> const& callback)
{
    for (auto const& child : m_children) {
        if (child->for_each_in_inclusive_subtree(callback) == IterationDecision::Break)
            return IterationDecision::Break;
    }

    return IterationDecision::Continue;
}

IterationDecision CanonicalNavigable::for_each_in_inclusive_subtree(Function<IterationDecision(CanonicalNavigable const&)> const& callback) const
{
    if (callback(*this) == IterationDecision::Break)
        return IterationDecision::Break;

    return for_each_in_subtree(callback);
}

IterationDecision CanonicalNavigable::for_each_in_subtree(Function<IterationDecision(CanonicalNavigable const&)> const& callback) const
{
    for (auto const& child : m_children) {
        if (child->for_each_in_inclusive_subtree(callback) == IterationDecision::Break)
            return IterationDecision::Break;
    }

    return IterationDecision::Continue;
}

WebContentClient& CanonicalNavigable::remote_host_client() const
{
    VERIFY(m_remote_client);
    return *m_remote_client;
}

void CanonicalNavigable::set_remote_host(NonnullRefPtr<WebContentClient> remote_client, u64 remote_page_id)
{
    detach_remote_host();

    m_host_locality = HostLocality::Remote;
    m_remote_client = move(remote_client);
    m_remote_page_id = remote_page_id;
}

void CanonicalNavigable::detach_remote_host()
{
    if (has_remote_host()) {
        m_remote_client->async_set_page_parent_context(m_remote_page_id, {});
        m_remote_client->async_discard_embedded_page(m_remote_page_id);
        // The page stops being a history job endpoint now; queued history work must not start against it. Its
        // client outlives the discard acknowledgement, so a shared process is not closed under the page.
        m_remote_client->prepare_for_detached_close(m_remote_page_id);
        m_remote_client->unregister_embedded_page(m_remote_page_id);
        top_level_traversable().did_lose_history_job_endpoint(*m_remote_client, m_remote_page_id);
    }

    m_host_locality = HostLocality::Local;
    m_remote_client = nullptr;
    m_remote_page_id = 0;
}

void CanonicalNavigable::set_viewport(Web::DevicePixelRect viewport_rect, double device_pixel_ratio)
{
    m_viewport_rect = viewport_rect;
    m_device_pixel_ratio = device_pixel_ratio;

    if (has_remote_host()) {
        m_remote_client->async_set_viewport(
            m_remote_page_id,
            viewport_rect.size(),
            device_pixel_ratio,
            Web::ViewportIsFullscreen::No);
    }
}

void CanonicalNavigable::set_replicated_state(Web::HTML::ReplicatedNavigableState state)
{
    m_active_session_history_entry_identity = state.active_session_history_entry_identity;
    m_replicated_state = move(state);
}

void CanonicalNavigable::set_current_session_history_entry(Web::HTML::SessionHistoryEntryDescriptor const& entry)
{
    m_current_session_history_entry_identity = Web::HTML::session_history_entry_identity(entry);
}

void CanonicalNavigable::set_active_session_history_entry(Web::HTML::SessionHistoryEntryDescriptor const& entry)
{
    m_active_session_history_entry_identity = Web::HTML::session_history_entry_identity(entry);
}

bool CanonicalNavigable::current_session_history_entry_is(Web::HTML::SessionHistoryEntryDescriptor const& entry) const
{
    return m_current_session_history_entry_identity.has_value()
        && *m_current_session_history_entry_identity == Web::HTML::session_history_entry_identity(entry);
}

bool CanonicalNavigable::active_document_is(Web::HTML::SessionHistoryEntryDescriptor const& entry) const
{
    return m_active_session_history_entry_identity.has_value()
        && m_active_session_history_entry_identity->document_state_id == entry.document_state.id;
}

void CanonicalNavigable::did_commit_navigation(Web::HTML::ReplicatedNavigableState replicated_state, Optional<Utf16String> const& navigation_id, RefPtr<CanonicalBrowsingContext> destination_browsing_context)
{
    auto commits_ongoing_navigation = !m_ongoing_navigation.has_value()
        || !navigation_id.has_value()
        || navigation_id == m_ongoing_navigation->navigation_id;

    auto previous_active_document_state_id = m_active_session_history_entry_identity.has_value()
        ? Optional<Web::HTML::CrossProcessId> { m_active_session_history_entry_identity->document_state_id }
        : Optional<Web::HTML::CrossProcessId> {};

    if (!destination_browsing_context && navigation_id.has_value() && commits_ongoing_navigation && m_ongoing_navigation.has_value())
        destination_browsing_context = m_ongoing_navigation->destination_browsing_context;
    if (destination_browsing_context)
        set_active_browsing_context(destination_browsing_context.release_nonnull());
    set_replicated_state(move(replicated_state));

    auto& traversable = top_level_traversable();
    auto endpoint = traversable.history_job_endpoint_for(*this);
    if (endpoint.client) {
        // FIXME: Pass the document's requestsOAC value once Origin-Agent-Cluster is implemented.
        auto browsing_context_group = traversable.active_browsing_context().group();
        VERIFY(browsing_context_group);
        auto agent = browsing_context_group->obtain_similar_origin_window_agent(m_replicated_state->active_document_origin, false);
        agent->set_hosting_process_if_unset(*endpoint.client);
    }

    // A navigation can commit while a newer navigation is already in flight. In that case update the replicated
    // state for the committed document without changing the newer navigation's transaction.
    if (!commits_ongoing_navigation)
        return;

    // The activated document's load becomes the view's tracked load. A same-document activation leaves the
    // active document's load running, so its tracked load stays in place.
    if (is_top_level_traversable()) {
        auto active_document_changed = !previous_active_document_state_id.has_value()
            || !m_active_session_history_entry_identity.has_value()
            || m_active_session_history_entry_identity->document_state_id != *previous_active_document_state_id;
        if (active_document_changed) {
            m_active_document_load = ActiveDocumentLoad {
                .navigation_id = m_ongoing_navigation.has_value() ? m_ongoing_navigation->navigation_id : Optional<Utf16String> {},
            };
        }
    }

    clear_ongoing_navigation();
}

CanonicalNavigable::OngoingNavigation& CanonicalNavigable::ensure_ongoing_navigation()
{
    if (!m_ongoing_navigation.has_value())
        m_ongoing_navigation = OngoingNavigation {};
    return *m_ongoing_navigation;
}

void CanonicalNavigable::set_ongoing_navigation(OngoingNavigation ongoing_navigation)
{
    clear_ongoing_navigation();
    m_ongoing_navigation = move(ongoing_navigation);
}

void CanonicalNavigable::set_ongoing_navigation_to_traversal(Web::HTML::CrossProcessId operation_id)
{
    m_ongoing_navigation_traversal_operation_id = operation_id;
}

void CanonicalNavigable::clear_ongoing_navigation_traversal(Web::HTML::CrossProcessId operation_id)
{
    if (m_ongoing_navigation_traversal_operation_id == operation_id)
        m_ongoing_navigation_traversal_operation_id.clear();
}

void CanonicalNavigable::clear_ongoing_navigation()
{
    m_ongoing_navigation.clear();
    m_ongoing_navigation_traversal_operation_id.clear();
}

void CanonicalNavigable::set_navigation_population_worker(WebContentClient& client, u64 page_id)
{
    auto& ongoing_navigation = ensure_ongoing_navigation();
    VERIFY(!ongoing_navigation.population_worker_client);
    ongoing_navigation.population_worker_client = client;
    ongoing_navigation.population_worker_page_id = page_id;
}

bool CanonicalNavigable::navigation_population_matches(WebContentClient const& client, u64 page_id, Utf16String const& navigation_id) const
{
    return m_ongoing_navigation.has_value()
        && m_ongoing_navigation->navigation_id == navigation_id
        && m_ongoing_navigation->phase == OngoingNavigation::Phase::Populating
        && navigation_population_worker_matches(client, page_id);
}

bool CanonicalNavigable::navigation_population_worker_matches(WebContentClient const& client, u64 page_id) const
{
    return m_ongoing_navigation.has_value()
        && m_ongoing_navigation->population_worker_client.ptr() == &client
        && m_ongoing_navigation->population_worker_page_id == page_id;
}

void CanonicalNavigable::set_navigation_host(WebContentClient& client, u64 page_id)
{
    auto& ongoing_navigation = ensure_ongoing_navigation();
    ongoing_navigation.host_client = client;
    ongoing_navigation.host_page_id = page_id;

    // The population worker conducts the navigation until the hosting process takes over.
    ongoing_navigation.population_worker_client = {};
    ongoing_navigation.population_worker_page_id = 0;
}

bool CanonicalNavigable::navigation_host_matches(WebContentClient const& client, u64 page_id) const
{
    return m_ongoing_navigation.has_value()
        && m_ongoing_navigation->host_client.ptr() == &client
        && m_ongoing_navigation->host_page_id == page_id;
}

bool CanonicalNavigable::navigation_owner_matches(WebContentClient const& client, u64 page_id) const
{
    return navigation_population_worker_matches(client, page_id) || navigation_host_matches(client, page_id);
}

bool CanonicalNavigable::navigation_transaction_matches(Utf16String const& navigation_id, WebContentClient const& client, u64 page_id) const
{
    return m_ongoing_navigation.has_value()
        && m_ongoing_navigation->navigation_id == navigation_id
        && m_ongoing_navigation->phase == OngoingNavigation::Phase::Populating
        && navigation_host_matches(client, page_id);
}

bool CanonicalNavigable::cancel_navigation_transaction_for_client(WebContentClient& client)
{
    if (!m_ongoing_navigation.has_value())
        return false;

    auto depends_on_client = m_ongoing_navigation->population_worker_client.ptr() == &client
        || m_ongoing_navigation->host_client.ptr() == &client;
    if (!depends_on_client)
        return false;

    clear_ongoing_navigation();
    return true;
}

void CanonicalNavigable::did_finish_navigation_transaction(Optional<Utf16String> const& navigation_id, Web::HTML::HistoryStepResult result)
{
    if (!navigation_id.has_value())
        return;

    // A transaction still live at its operation's completion never activated its document.
    if (m_ongoing_navigation.has_value() && m_ongoing_navigation->navigation_id == navigation_id)
        clear_ongoing_navigation();

    if (result != Web::HTML::HistoryStepResult::Applied
        && m_active_document_load.navigation_id == navigation_id) {
        clear_active_document_load();
    }
}

bool CanonicalNavigable::matches_ongoing_navigation(Optional<Utf16String> const& navigation_id) const
{
    // A live transaction owns the view's loading state, so completion signals must name it.
    if (m_ongoing_navigation.has_value())
        return m_ongoing_navigation->has_started && navigation_id == m_ongoing_navigation->navigation_id;

    // Otherwise completion signals concern the active document's tracked load.
    return navigation_id == m_active_document_load.navigation_id;
}

}
