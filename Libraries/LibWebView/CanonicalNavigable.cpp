/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalNavigable.h>

#include <LibWeb/HTML/HistoryOperation.h>
#include <LibWeb/Page/ViewportIsFullscreen.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/SiteIsolation.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

CanonicalNavigable::CanonicalNavigable(Web::HTML::CrossProcessId id)
    : m_id(id)
{
}

RefPtr<WebContentPage> CanonicalNavigable::reporting_page() const
{
    if (!m_container_document)
        return {};
    return m_container_document->host();
}

CanonicalDocument& CanonicalNavigable::active_document() const
{
    // A navigable's active document is its active session history entry's document.
    VERIFY(m_active_session_history_entry && m_active_session_history_entry->document_state->document);
    return *m_active_session_history_entry->document_state->document;
}

CanonicalBrowsingContext& CanonicalNavigable::active_browsing_context() const
{
    // A navigable's active browsing context is its active document's browsing context.
    return active_document().browsing_context();
}

// https://html.spec.whatwg.org/multipage/browsers.html#obtain-browsing-context-navigation
// NB: The browsing context is returned with its active document, which nothing else holds for a new one.
CanonicalBrowsingContext::BrowsingContextAndDocument CanonicalNavigable::obtain_a_browsing_context_to_use_for_a_navigation_response(Web::HTML::OpenerPolicyEnforcementResult const& coop_enforcement_result)
{
    // 1. Let browsingContext be navigationParams's navigable's active browsing context.
    CanonicalBrowsingContext::BrowsingContextAndDocument browsing_context { active_browsing_context(), active_document() };

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
    auto new_browsing_context = CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document();

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

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#initialise-the-document-object
NonnullRefPtr<CanonicalDocument> CanonicalNavigable::create_and_initialize_a_document(NavigationLoader::ResponseDocument const& navigation_params)
{
    // 1. Let browsingContext be the result of obtaining a browsing context to use for a navigation response given navigationParams.
    auto browsing_context_and_document = obtain_a_browsing_context_to_use_for_a_navigation_response(navigation_params.coop_enforcement_result);
    auto& browsing_context = browsing_context_and_document.browsing_context;

    // 5. Let window be null.
    RefPtr<CanonicalWindow> window;

    // 6. If browsingContext's active document's is initial about:blank is true, and browsingContext's active document's
    //    origin is same origin-domain with navigationParams's origin, then set window to browsingContext's active window.
    if (browsing_context->active_document()->is_initial_about_blank()
        && browsing_context->active_document()->origin().is_same_origin_domain(navigation_params.origin)) {
        window = browsing_context->active_window();
    }
    // 7. Otherwise:
    else {
        // FIXME: 1. Let oacHeader be the result of getting a structured field value given `Origin-Agent-Cluster` and "item"
        //           from navigationParams's response's header list.
        // FIXME: 2. Let requestsOAC be true if oacHeader is not null and oacHeader[0] is the boolean true; otherwise false.
        // FIXME: 3. If navigationParams's reserved environment is a non-secure context, then set requestsOAC to false.
        auto requests_oac = false;

        // 4. Let agent be the result of obtaining a similar-origin window agent given navigationParams's origin,
        //    browsingContext's group, and requestsOAC.
        // AD-HOC: Only a top-level browsing context has a group. A child browsing context's is its top-level browsing
        //         context's.
        auto agent = browsing_context->top_level_browsing_context().group()->obtain_similar_origin_window_agent(navigation_params.origin, requests_oac);

        // 5. Let realmExecutionContext be the result of creating a new realm given agent and the following customizations:
        //    - For the global object, create a new Window object.
        //    - For the global this binding, use browsingContext's WindowProxy object.
        // 6. Set window to the global object of realmExecutionContext's Realm component.
        // NB: The realm is in the process hosting agent, which runs steps 7 to 10.
        window = CanonicalWindow::create(agent);
    }

    // 9. Let document be a new Document, with
    //    origin: navigationParams's origin
    //    browsing context: browsingContext
    // NB: The process hosting window's agent creates the document, with its other fields, and runs the remaining steps.

    // 22. Return document.
    return CanonicalDocument::create(navigation_params.origin, browsing_context, window.release_nonnull(), CanonicalDocument::IsInitialAboutBlank::No);
}

CanonicalNavigable::~CanonicalNavigable()
{
    if (m_active_session_history_entry)
        m_active_session_history_entry->document_state->document = nullptr;
}

bool CanonicalNavigable::has_remote_host() const
{
    if (!m_parent || !m_active_session_history_entry || !active_document().host())
        return false;
    return active_document().host() != reporting_page();
}

void CanonicalNavigable::stage_same_document_session_history_entry(Web::HTML::CrossProcessId operation_id, NonnullRefPtr<CanonicalSessionHistoryEntry> entry)
{
    m_pending_same_document_session_history_entries.append({ operation_id, move(entry) });
}

RefPtr<CanonicalSessionHistoryEntry> CanonicalNavigable::take_pending_same_document_session_history_entry(Web::HTML::CrossProcessId operation_id, Web::HTML::SessionHistoryEntryIdentity const& entry_identity)
{
    for (size_t i = 0; i < m_pending_same_document_session_history_entries.size(); ++i) {
        auto const& pending_entry = m_pending_same_document_session_history_entries[i];
        if (pending_entry.operation_id == operation_id && pending_entry.entry->identity() == entry_identity)
            return m_pending_same_document_session_history_entries.take(i).entry;
    }
    return {};
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

void CanonicalNavigable::set_container_document(Badge<CanonicalTraversable>, CanonicalDocument& document)
{
    m_container_document = document;
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

WebContentPage& CanonicalNavigable::remote_host() const
{
    VERIFY(has_remote_host());
    return *active_document().host();
}

void CanonicalNavigable::hand_pending_webdriver_commands_to(WebContentPage& new_host)
{
    auto& traversable = top_level_traversable();
    auto old_host = traversable.page_hosting(*this);
    if (!old_host)
        return;
    if (auto view = traversable.view(); view.has_value())
        view->move_pending_webdriver_commands_to_new_host({}, id(), *old_host, new_host);
}

RefPtr<WebContentClient> CanonicalNavigable::process_to_host(CanonicalDocument const& document, Optional<URL::Origin> const& initiator_origin) const
{
    RefPtr<WebContentPage> page_holding_navigable = parent() ? reporting_page() : top_level_traversable().page_hosting(*this);
    RefPtr<WebContentClient> process_holding_navigable = page_holding_navigable ? &page_holding_navigable->client() : nullptr;

    // The view displays a traversable in a process of its own, where the WindowProxies of its related browsing contexts
    // are not represented: related top-level browsing contexts share a process.
    // FIXME: Represent a group's tabs in every process holding one of them, and display a tab in the process hosting
    //        its document's agent, so that related tabs are isolated too.
    if (!parent() && active_browsing_context().group()->browsing_context_set().size() > 1)
        return process_holding_navigable;

    // An agent runs in one process: its documents go where it is hosted.
    if (auto process = document.relevant_global_object().agent().hosting_process())
        return process;

    // Documents that are not isolated go with the page holding the navigable.
    auto mode = site_isolation_mode();
    if (mode == SiteIsolationMode::Disabled || (mode == SiteIsolationMode::TopLevel && parent()))
        return process_holding_navigable;

    // An opaque origin keys an agent cluster of its own, which nothing but the documents it was created from can
    // address: it goes with the initiator's agent. A traversable's document of an opaque origin that no such agent
    // hosts, as a file's opened from the browser's UI is, keeps the process of a document of an opaque origin.
    if (document.origin().is_opaque()) {
        if (initiator_origin.has_value()) {
            if (auto initiator_agent = document.browsing_context().top_level_browsing_context().group()->similar_origin_window_agent_for(*initiator_origin)) {
                if (auto process = initiator_agent->hosting_process())
                    return process;
            }
        }
        if (!parent() && active_document().origin().is_opaque())
            return process_holding_navigable;
    }

    // A traversable's first document takes the process its initial about:blank came with, unless that document
    // inherited its creator's origin and shares the creator's process.
    if (!parent() && active_document().is_initial_about_blank() && active_document().origin().is_opaque())
        return process_holding_navigable;

    return nullptr;
}

ErrorOr<NonnullRefPtr<WebContentPage>> CanonicalNavigable::obtain_page_to_host(CanonicalDocument const& document, Optional<URL::Origin> const& initiator_origin)
{
    VERIFY(parent());
    auto& traversable = top_level_traversable();
    // A page beginning to host the navigable starts from a document standing in for the current entry's.
    auto current_entry_descriptor = [&] {
        auto current_step = traversable.session_history().current_step();
        VERIFY(current_step.has_value());
        auto const* current_entry = traversable.session_history().get_the_target_history_entry(*this, *current_step);
        VERIFY(current_entry);
        return current_entry->descriptor();
    };

    // The host takes the navigable's node over once the document it is to display is activated; until then, the page
    // hosting the displayed document keeps it.
    auto host = process_to_host(document, initiator_origin);
    if (host && host == &reporting_page()->client()) {
        // The page holding the container populates the document in a provisional navigable while another page hosts
        // the displayed document.
        if (has_remote_host())
            host->async_begin_hosting_navigable(reporting_page()->id(), id(), current_entry_descriptor(), traversable.system_visibility_state());
        return *reporting_page();
    }
    if (host && has_remote_host() && host == &remote_host().client())
        return remote_host();

    // A process holds one page per tab, with the tab's whole graph: the process displaying the tab hosts a document
    // in the view's page, another process in the page it has for the tab, or in a page created for it.
    Compositing::PageId page_id;
    if (host && host->page_id_for_traversable(traversable).has_value()) {
        page_id = *host->page_id_for_traversable(traversable);
        host->async_begin_hosting_navigable(page_id, id(), current_entry_descriptor(), traversable.system_visibility_state());
    } else if (host) {
        page_id = Application::the().allocate_page_id();
        host->async_create_embedded_page(page_id, traversable.remote_navigable_graph(), id(), current_entry_descriptor(), traversable.system_visibility_state());
        host->register_embedded_page(page_id, traversable);
        traversable.represent_openers_in(*host);
    } else {
        auto process = TRY(Application::the().launch_child_frame_web_content_process(reporting_page()->client().is_private(), traversable.remote_navigable_graph(), id(), current_entry_descriptor()));
        host = move(process.client);
        page_id = process.page_id;
        host->register_embedded_page(page_id, traversable);
        traversable.represent_openers_in(*host);
    }

    host->async_update_visibility_state(page_id, id(), traversable.system_visibility_state());
    return *host->page(page_id);
}

RefPtr<CanonicalDocumentState> CanonicalNavigable::populating_document_state() const
{
    return m_populated_document.has_value() ? m_populated_document->document_state.ptr() : nullptr;
}

RefPtr<CanonicalDocument> CanonicalNavigable::pending_document() const
{
    return m_populated_document.has_value() ? m_populated_document->document.ptr() : nullptr;
}

void CanonicalNavigable::populate_document(NonnullRefPtr<CanonicalDocumentState> document_state, NonnullRefPtr<CanonicalDocument> document, Optional<Utf16String> navigation_id)
{
    discard_pending_host();
    m_populated_document = PopulatedDocument { move(document_state), move(document), move(navigation_id) };
}

void CanonicalNavigable::abandon_pending_document()
{
    m_populated_document.clear();
}

void CanonicalNavigable::abandon_document_populated_for(CanonicalDocumentState const& document_state)
{
    if (populating_document_state() != &document_state)
        return;
    discard_pending_host();
    abandon_pending_document();
}

void CanonicalNavigable::place_pending_document(WebContentPage& page)
{
    auto document = pending_document();
    VERIFY(document);
    document->set_host(page);
    send_viewport_to_host();
}

bool CanonicalNavigable::has_pending_host() const
{
    auto document = pending_document();
    if (!document)
        return false;
    auto const& host = document->host();
    return host && host != active_document().host();
}

WebContentPage& CanonicalNavigable::pending_host() const
{
    VERIFY(has_pending_host());
    return *pending_document()->host();
}

void CanonicalNavigable::discard_pending_host()
{
    // The traversable's pending host is the page the view installed, which the view releases.
    if (is_top_level_traversable() || !has_pending_host())
        return;
    NonnullRefPtr page = pending_host();
    abandon_pending_document();
    page->async_discard_provisional_navigable(id());
    top_level_traversable().release_page_if_unused(move(page));
}

void CanonicalNavigable::set_viewport(Compositing::DevicePixelRect viewport_rect, Compositing::DevicePixelRect viewport_intersection, double device_pixel_ratio)
{
    m_viewport_rect = viewport_rect;
    m_viewport_intersection = viewport_intersection;
    m_device_pixel_ratio = device_pixel_ratio;
    send_viewport_to_host();
}

void CanonicalNavigable::send_viewport_to_host() const
{
    if (!m_viewport_rect.has_value())
        return;
    RefPtr<WebContentPage> remote_host = has_remote_host() ? &this->remote_host() : nullptr;
    if (remote_host)
        send_viewport_to(*remote_host);
    if (has_pending_host() && &pending_host() != remote_host.ptr())
        send_viewport_to(pending_host());
}

void CanonicalNavigable::send_viewport_to(WebContentPage& host) const
{
    host.async_set_hosted_root_viewport(id(), m_viewport_rect->size(), m_viewport_intersection, m_device_pixel_ratio);
}

void CanonicalNavigable::set_replicated_state(Web::HTML::ReplicatedNavigableState state)
{
    m_document_blob_url = BlobURLHandle::for_url(blob_url_store(), state.active_document_url);
    m_replicated_state = move(state);
}

void CanonicalNavigable::update_container_state(Web::HTML::ReplicatedContainerState state)
{
    if (!m_replicated_state.has_value())
        return;
    m_replicated_state->container = state;
    if (has_remote_host())
        remote_host().async_update_local_root_container_state(id(), move(state));
}

void CanonicalNavigable::update_replicated_state(Web::HTML::ReplicatedNavigableState state)
{
    auto opener_changed = !m_replicated_state.has_value() || m_replicated_state->opener_navigable_id != state.opener_navigable_id;
    set_replicated_state(move(state));

    // The process hosting the active document sets and disowns its browsing context's opener browsing context.
    RefPtr<CanonicalBrowsingContext> opener_browsing_context;
    if (m_replicated_state->opener_navigable_id.has_value()) {
        if (auto* opener_traversable = CanonicalTraversable::traversable_containing(*m_replicated_state->opener_navigable_id)) {
            if (auto opener = opener_traversable->find(*m_replicated_state->opener_navigable_id); opener.has_value())
                opener_browsing_context = opener->active_browsing_context();
        }
    }
    active_browsing_context().set_opener_browsing_context(move(opener_browsing_context));

    auto& traversable = top_level_traversable();
    Vector<NonnullRefPtr<WebContentClient>> clients;
    if (opener_changed) {
        traversable.for_each_hosting_page([&](WebContentPage& page) {
            if (!any_of(clients, [&](auto const& client) { return client.ptr() == &page.client(); }))
                clients.append(page.client());
        });
    }

    // Every process holding part of the tab holds the tab of a new opener before it hears of it.
    if (m_replicated_state->opener_navigable_id.has_value()) {
        for (auto& client : clients)
            traversable.represent_openers_in(client);
    }

    traversable.for_each_page_representing(*this, [&](WebContentPage& page) {
        page.async_update_remote_navigable(id(), *m_replicated_state);
    });

    // A process can stop needing the tab of the previous opener.
    for (auto& client : clients)
        client->release_unneeded_opener_pages();
}

void CanonicalNavigable::active_document_completely_finished_loading()
{
    // The navigable's container runs the load event steps in the page hosting its parent's document, which is among
    // the pages representing the navigable.
    top_level_traversable().for_each_page_representing(*this, [&](WebContentPage& page) {
        page.async_content_navigable_completely_finished_loading(id());
    });
}

bool CanonicalNavigable::current_session_history_entry_is(CanonicalSessionHistoryEntry const& entry) const
{
    return m_current_session_history_entry == &entry;
}

bool CanonicalNavigable::active_document_is(CanonicalSessionHistoryEntry const& entry) const
{
    return m_active_session_history_entry && entry.document_state->document == &active_document();
}

void CanonicalNavigable::did_commit_navigation(CanonicalSessionHistoryEntry& entry, Web::HTML::ReplicatedNavigableState replicated_state, Optional<Utf16String> const& navigation_id, DidPopulateDocument did_populate_document, RefPtr<WebContentPage> host)
{
    auto commits_ongoing_navigation = !m_ongoing_navigation.has_value()
        || !navigation_id.has_value()
        || navigation_id == m_ongoing_navigation->navigation_id;

    auto active_document_changed = !active_document_is(entry);
    NonnullRefPtr previous_document = active_document();

    RefPtr<CanonicalDocument> document;
    if (m_populated_document.has_value() && m_populated_document->document_state == entry.document_state)
        document = m_populated_document.release_value().document;
    VERIFY(document || !active_document_changed);
    if (!document)
        document = previous_document;

    // The commands WebDriver has waiting in the page that hosted the displaced document follow the navigable to the
    // page hosting the activated one.
    if (document != previous_document && host && host != previous_document->host())
        hand_pending_webdriver_commands_to(*host);

    // A document state holds its document while its entry is active.
    if (m_active_session_history_entry->document_state != entry.document_state)
        m_active_session_history_entry->document_state->document = nullptr;
    entry.document_state->document = document;
    m_active_session_history_entry = entry;
    if (document != previous_document) {
        document->make_active();
        if (!document->host())
            document->set_host(host);
    }
    update_replicated_state(move(replicated_state));

    // The displaced document is gone, and its child navigables with it. The page that hosted it reported their
    // destruction when it unloaded the document, unless another page hosts the activated document: that page holds
    // the navigable remotely from now on, and the child navigables of the displaced document go here.
    if (document != previous_document) {
        if (auto previous_host = previous_document->host(); previous_host != document->host()) {
            auto& traversable = top_level_traversable();
            traversable.remove_child_navigables_of(*this, *previous_document);
            if (previous_host && previous_host->is_open())
                traversable.stop_hosting_in_page(*this, previous_host.release_nonnull());
        }
    }

    // A navigation can commit while a newer navigation is already in flight. In that case update the replicated
    // state for the committed document without changing the newer navigation's transaction.
    if (!commits_ongoing_navigation)
        return;

    // The activated document's load becomes the navigable's tracked load. Reloads can reuse the document state,
    // while a same-document activation leaves the active document's load in place.
    if (active_document_changed || did_populate_document == DidPopulateDocument::Yes) {
        m_active_document_load = ActiveDocumentLoad {
            .navigation_id = m_ongoing_navigation.has_value() ? m_ongoing_navigation->navigation_id : Optional<Utf16String> {},
        };
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
    // NB: Taken before the handle covering this navigation's start is dropped below, so that a revoked entry is not
    //     let go of in between.
    auto blob_url = BlobURLHandle::for_url(blob_url_store(), ongoing_navigation.url);

    clear_ongoing_navigation();
    m_navigation_blob_url = move(blob_url);
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

void CanonicalNavigable::clear_ongoing_navigation_state()
{
    m_ongoing_navigation.clear();
    m_ongoing_navigation_traversal_operation_id.clear();

    // NB: The navigation this covered has either been announced, and is held below, or is not coming.
    m_pending_navigation_blob_url = {};
    m_navigation_blob_url = {};
}

void CanonicalNavigable::clear_ongoing_navigation()
{
    // The document populated for the navigation is not going to be activated.
    if (m_populated_document.has_value() && m_populated_document->navigation_id.has_value() && m_ongoing_navigation.has_value() && m_populated_document->navigation_id == m_ongoing_navigation->navigation_id)
        abandon_document_populated_for(*m_populated_document->document_state);
    clear_ongoing_navigation_state();
    discard_pending_host();
}

BlobURLStore* CanonicalNavigable::blob_url_store() const
{
    auto page = reporting_page();
    return page ? page->client().session().blob_url_store.ptr() : nullptr;
}

void CanonicalNavigable::retain_blob_url_token(URL::BlobURLEntry::Token token)
{
    if (auto* store = blob_url_store())
        m_pending_navigation_blob_url = BlobURLHandle { *store, token };
}

void CanonicalNavigable::set_navigation_population_worker(WebContentPage& page)
{
    auto& ongoing_navigation = ensure_ongoing_navigation();
    VERIFY(!ongoing_navigation.population_worker);
    ongoing_navigation.population_worker = page;
}

bool CanonicalNavigable::navigation_population_matches(WebContentPage const& page, Utf16String const& navigation_id) const
{
    return m_ongoing_navigation.has_value()
        && m_ongoing_navigation->navigation_id == navigation_id
        && m_ongoing_navigation->phase == OngoingNavigation::Phase::Populating
        && navigation_population_worker_matches(page);
}

bool CanonicalNavigable::navigation_population_worker_matches(WebContentPage const& page) const
{
    return m_ongoing_navigation.has_value() && m_ongoing_navigation->population_worker.ptr() == &page;
}

void CanonicalNavigable::set_navigation_host(WebContentPage& page)
{
    auto& ongoing_navigation = ensure_ongoing_navigation();
    ongoing_navigation.host = page;

    // The population worker conducts the navigation until the hosting process takes over.
    ongoing_navigation.population_worker = nullptr;
}

bool CanonicalNavigable::navigation_host_matches(WebContentPage const& page) const
{
    return m_ongoing_navigation.has_value() && m_ongoing_navigation->host.ptr() == &page;
}

bool CanonicalNavigable::navigation_owner_matches(WebContentPage const& page) const
{
    return navigation_population_worker_matches(page) || navigation_host_matches(page);
}

bool CanonicalNavigable::navigation_transaction_matches(Utf16String const& navigation_id, WebContentPage const& page) const
{
    return m_ongoing_navigation.has_value()
        && m_ongoing_navigation->navigation_id == navigation_id
        && m_ongoing_navigation->phase == OngoingNavigation::Phase::Populating
        && navigation_host_matches(page);
}

bool CanonicalNavigable::cancel_navigation_transaction_for_client(WebContentClient& client)
{
    if (!m_ongoing_navigation.has_value())
        return false;

    auto is_page_of_client = [&](RefPtr<WebContentPage> const& page) {
        return page && &page->client() == &client;
    };
    if (!is_page_of_client(m_ongoing_navigation->population_worker) && !is_page_of_client(m_ongoing_navigation->host))
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
