/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <AK/StringBuilder.h>
#include <LibCore/EventLoop.h>
#include <LibWebCommon/WebView/SiteIsolation.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalEnvironmentSettingsObject.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/StorageJar.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/document-sequences.html#top-level-traversable-set
static Vector<NonnullOwnPtr<CanonicalTraversable>>& user_agent_top_level_traversable_set()
{
    static NeverDestroyed<Vector<NonnullOwnPtr<CanonicalTraversable>>> set;
    return *set;
}

CanonicalTraversable::CanonicalTraversable(Web::HTML::CrossProcessId id, NonnullRefPtr<CanonicalSessionHistoryEntry> initial_history_entry)
    : CanonicalNavigable(id)
    , m_session_history(move(initial_history_entry))
    , m_session_storage(StorageJar::create())
{
}

void CanonicalTraversable::clone_session_storage_from(CanonicalTraversable const& other)
{
    // https://storage.spec.whatwg.org/#legacy-clone-a-traversable-storage-shed
    // 1. For each key → shelf of A's storage shed:
    //    1. Let newShelf be the result of running create a storage shelf with "session".
    //    2. Set newShelf's bucket map["default"]'s bottle map["sessionStorage"]'s map to a clone of
    //       shelf's bucket map["default"]'s bottle map["sessionStorage"]'s map.
    //    3. Set B's storage shed[key] to newShelf.
    m_session_storage->clone_from(*other.m_session_storage);
}

// https://html.spec.whatwg.org/multipage/interaction.html#system-visibility-state
void CanonicalTraversable::set_system_visibility_state(Web::HTML::VisibilityState visibility_state)
{
    if (m_system_visibility_state == visibility_state)
        return;
    m_system_visibility_state = visibility_state;
    for_each_hosting_page([&](WebContentPage& page) {
        page.async_set_system_visibility_state(visibility_state);
    });

    // When a user agent determines that the system visibility state for
    // traversable navigable traversable has changed to newState, it must run the following steps:

    // 1. Let navigables be the inclusive descendant navigables of traversable's active document.
    // 2. For each navigable of navigables:
    for_each_in_inclusive_subtree([&](CanonicalNavigable& navigable) {
        // 1. Let document be navigable's active document.
        // 2. Queue a global task on the user interaction task source given document's relevant global object
        //    to update the visibility state of document with newState.
        // NB: The page hosting document runs the task.
        if (auto const& host = navigable.active_document().host())
            host->async_update_visibility_state(navigable.id(), visibility_state);
        return IterationDecision::Continue;
    });
}

// https://html.spec.whatwg.org/multipage/interaction.html#system-focus
void CanonicalTraversable::set_has_system_focus(bool has_system_focus, RefPtr<WebContentPage> requesting_page)
{
    m_has_system_focus = has_system_focus;

    // NB: Every page holding part of the tab answers for the traversable's system focus. A page asking for the change
    //     made it when it asked.
    for_each_hosting_page([&](WebContentPage& page) {
        if (page != requesting_page)
            page.async_set_has_focus(has_system_focus);
    });
}

void CanonicalTraversable::set_focused_navigable(CanonicalNavigable& navigable, WebContentPage& requesting_page)
{
    if (m_focused_navigable_id == navigable.id())
        return;
    m_focused_navigable_id = navigable.id();

    // The page that moved focus ran the focus update steps for the documents it hosts. The other pages run them for
    // theirs.
    for_each_hosting_page([&](WebContentPage& page) {
        if (&page != &requesting_page)
            page.async_set_focused_navigable(navigable.id());
    });
}

// The page keyboard and text input go to.
RefPtr<WebContentPage> CanonicalTraversable::focused_navigable_host() const
{
    if (m_focused_navigable_id.has_value()) {
        if (auto navigable = find(*m_focused_navigable_id); navigable.has_value()) {
            if (auto page = page_hosting(*navigable); page && page->is_open())
                return page;
        }
    }
    return page_hosting(*this);
}

// The origin, in the view's viewport, of the viewport of the local root holding the focused navigable in its page.
Web::DevicePixelPoint CanonicalTraversable::focused_navigable_host_offset() const
{
    if (!m_focused_navigable_id.has_value())
        return {};
    auto navigable = find(*m_focused_navigable_id);
    if (!navigable.has_value())
        return {};
    return local_root_offset(*navigable);
}

// The origin, in the view's viewport, of the viewport of the local root holding the navigable in its page.
Web::DevicePixelPoint CanonicalTraversable::local_root_offset(CanonicalNavigable const& navigable) const
{
    Web::DevicePixelPoint offset;
    for (auto const* ancestor = &navigable; ancestor; ancestor = ancestor->parent()) {
        if (ancestor->has_remote_host() && ancestor->viewport_rect().has_value())
            offset.translate_by(ancestor->viewport_rect()->location());
    }
    return offset;
}

CanonicalNavigable& CanonicalTraversable::insert(NonnullRefPtr<WebContentPage> reporting_page, CanonicalNavigable& parent, CanonicalDocument& container_document, Web::HTML::CrossProcessId frame_id, Web::HTML::HostedNavigableState hosted_state, NonnullRefPtr<CanonicalSessionHistoryEntry> active_session_history_entry, NonnullRefPtr<CanonicalDocument> document)
{
    VERIFY(!find(frame_id).has_value());

    document->set_host(reporting_page);
    active_session_history_entry->document_state->document = move(document);
    auto navigable = make<CanonicalNavigable>(frame_id);
    navigable->set_container_document({}, container_document);
    navigable->set_current_session_history_entry(active_session_history_entry);
    navigable->set_active_session_history_entry(move(active_session_history_entry));
    navigable->set_hosted_state(move(hosted_state));

    auto& navigable_ref = parent.append_child(move(navigable));
    m_navigable_index.set(navigable_ref.id(), navigable_ref.make_weak_ptr());

    for_each_page_representing(navigable_ref, [&](WebContentPage& page) {
        page.async_insert_remote_navigable({ .id = navigable_ref.id(), .parent_id = parent.id(), .replicated_state = *navigable_ref.replicated_state() });
    });
    return navigable_ref;
}

// INTEROP: Reloading rebuilds child frames from the new document instead of restoring their previous entries.
void CanonicalTraversable::adopt_nested_history_for_created_child(CanonicalNavigable const& parent, CanonicalDocument const& container_document, Web::HTML::CrossProcessId child_navigable_id)
{
    if (container_document.is_completely_loaded())
        return;
    RefPtr<CanonicalDocumentState> document_state;
    parent.for_each_populated_document_state([&](CanonicalDocumentState& populated_document_state) {
        if (populated_document_state.populated_document->document == &container_document)
            document_state = populated_document_state;
    });
    if (!document_state && &parent.active_document() == &container_document)
        document_state = parent.active_session_history_entry()->document_state;
    if (!document_state)
        return;

    size_t position = 0;
    for (auto const& child : parent.children()) {
        if (child->container_document() == &container_document)
            ++position;
    }
    auto& nested_histories = document_state->nested_histories;
    if (position >= nested_histories.size() || find(nested_histories[position].id).has_value())
        return;
    nested_histories[position].id = child_navigable_id;
}

Vector<Web::HTML::RemoteNavigableDescriptor> CanonicalTraversable::remote_navigable_graph() const
{
    Vector<Web::HTML::RemoteNavigableDescriptor> graph;
    for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        VERIFY(navigable.replicated_state().has_value());
        graph.append({
            .id = navigable.id(),
            .parent_id = navigable.parent() ? Optional<Web::HTML::CrossProcessId> { navigable.parent()->id() } : Optional<Web::HTML::CrossProcessId> {},
            .replicated_state = *navigable.replicated_state(),
        });
        return IterationDecision::Continue;
    });
    return graph;
}

void CanonicalTraversable::for_each_hosting_page(Function<void(WebContentPage&)> const& callback) const
{
    Vector<NonnullRefPtr<WebContentPage>> pages;
    auto visit = [&](WebContentPage& page) {
        if (!page.is_open() || any_of(pages, [&](auto const& visited) { return visited.ptr() == &page; }))
            return;
        pages.append(page);
        callback(page);
    };
    if (auto page = display_page(); page)
        visit(*page);
    for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        // The page hosting a navigable's document holds the tab's graph, and a page chosen to host its next document
        // from the moment it is chosen.
        if (auto const& host = navigable.active_document().host())
            visit(*host);
        navigable.for_each_pending_host(visit);
        return IterationDecision::Continue;
    });
    for (auto const& page : m_representing_pages)
        visit(page);
}

CanonicalTraversable* CanonicalTraversable::traversable_containing(Web::HTML::CrossProcessId navigable_id)
{
    CanonicalTraversable* traversable = nullptr;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (!view.traversable().find(navigable_id).has_value())
            return IterationDecision::Continue;
        traversable = &view.traversable();
        return IterationDecision::Break;
    });
    return traversable;
}

CanonicalNavigable* CanonicalTraversable::navigable_with_active_browsing_context(CanonicalBrowsingContext const& browsing_context)
{
    CanonicalNavigable* result = nullptr;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        view.traversable().for_each_in_inclusive_subtree([&](CanonicalNavigable& navigable) {
            if (&navigable.active_browsing_context() != &browsing_context)
                return IterationDecision::Continue;
            result = &navigable;
            return IterationDecision::Break;
        });
        return result ? IterationDecision::Break : IterationDecision::Continue;
    });
    return result;
}

// A process holding part of a tab holds every tab of the tab's browsing context group, in pages hosting none of
// them, so that a document here finds the group's navigables by target name and reaches an opener's WindowProxy.
void CanonicalTraversable::represent_group_in(WebContentClient& client)
{
    auto group = active_browsing_context().group();
    if (!group)
        return;
    for (auto* top_level_browsing_context : group->browsing_context_set()) {
        auto* navigable = navigable_with_active_browsing_context(*top_level_browsing_context);
        if (!navigable)
            continue;
        auto& traversable = navigable->top_level_traversable();
        if (client.page_id_for_traversable(traversable).has_value())
            continue;
        auto page_id = Application::the().allocate_page_id();
        client.async_create_representing_page(page_id, traversable.remote_navigable_graph());
        client.register_embedded_page(page_id, traversable);
        traversable.m_representing_pages.append(*client.page(page_id));
    }
}

// Every process holding part of a tab of the group holds the tab that joined it.
void CanonicalTraversable::represent_group_everywhere()
{
    auto group = active_browsing_context().group();
    if (!group)
        return;
    Vector<NonnullRefPtr<WebContentClient>> clients;
    for (auto* top_level_browsing_context : group->browsing_context_set()) {
        auto* navigable = navigable_with_active_browsing_context(*top_level_browsing_context);
        if (!navigable)
            continue;
        navigable->top_level_traversable().for_each_hosting_page([&](WebContentPage& page) {
            if (!any_of(clients, [&](auto const& client) { return client.ptr() == &page.client(); }))
                clients.append(page.client());
        });
    }
    for (auto& client : clients)
        represent_group_in(*client);
}

void CanonicalTraversable::did_switch_browsing_context_group()
{
    for (auto& page : Vector { m_representing_pages })
        release_page_if_unused(page);

    Vector<NonnullRefPtr<WebContentClient>> clients;
    for_each_hosting_page([&](WebContentPage& page) {
        if (!any_of(clients, [&](auto const& client) { return client.ptr() == &page.client(); }))
            clients.append(page.client());
    });
    for (auto& client : clients)
        client->release_unneeded_representing_pages();

    represent_group_everywhere();
}

void CanonicalTraversable::forget_representing_page(WebContentPage& page)
{
    m_representing_pages.remove_all_matching([&](auto const& representing_page) { return representing_page.ptr() == &page; });
}

void CanonicalTraversable::discard_representing_pages()
{
    for (auto& page : exchange(m_representing_pages, {})) {
        if (page->is_open())
            page->discard();
    }
}

void CanonicalTraversable::for_each_page_representing(CanonicalNavigable const& navigable, Function<void(WebContentPage&)> const& callback) const
{
    for_each_hosting_page([&](WebContentPage& page) {
        if (represents(navigable, page))
            callback(page);
    });
}

bool CanonicalTraversable::represents(CanonicalNavigable const& navigable, WebContentPage const& page) const
{
    if (hosts(navigable, page))
        return false;
    // The view's page holds the traversable's node from the moment it displays the tab.
    if (&navigable == this)
        return display_page().ptr() != &page;
    if (auto const* container_document = navigable.container_document(); container_document && container_document->host() == &page)
        return true;
    auto const& parent = *navigable.parent();
    if (hosts(parent, page) || parent.pending_host_matches(page) || (&parent == this && display_page().ptr() == &page))
        return false;
    return represents(parent, page);
}

bool CanonicalTraversable::hosts(CanonicalNavigable const& navigable, WebContentPage const& page) const
{
    if (auto const& host = navigable.active_document().host())
        return host == &page;
    return page_hosting(navigable) == &page;
}

bool CanonicalTraversable::page_hosts_any(WebContentPage const& page) const
{
    bool hosts_any = false;
    for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        if (hosts(navigable, page) || navigable.pending_host_matches(page)) {
            hosts_any = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return hosts_any;
}

void CanonicalTraversable::stop_hosting_in_page(CanonicalNavigable& navigable, NonnullRefPtr<WebContentPage> page)
{
    // The page retired the local navigable that displayed the document when it unloaded it. This is the state the
    // navigable's next document activated with.
    VERIFY(navigable.replicated_state().has_value());
    page->async_stop_hosting_navigable(navigable.id(), *navigable.replicated_state());

    if (!page_hosts_any(page)) {
        release_page_if_unused(move(page));
        return;
    }

    // The page represents the navigable's subtree from now on, which its next document's host reported while the
    // page hosted the displaced document.
    navigable.for_each_in_subtree([&](CanonicalNavigable const& descendant) {
        if (represents(descendant, page)) {
            VERIFY(descendant.replicated_state().has_value());
            page->async_insert_remote_navigable({ .id = descendant.id(), .parent_id = descendant.parent()->id(), .replicated_state = *descendant.replicated_state() });
        }
        return IterationDecision::Continue;
    });
}

void CanonicalTraversable::release_page_if_unused(NonnullRefPtr<WebContentPage> page)
{
    // A closed page has been let go of already, and the client of one whose process was lost can be gone, e.g. when
    // recovering from the crash abandons a history job that populated a document in the page.
    if (!page->is_open())
        return;
    if (page_hosts_any(page))
        return;
    // A page left hosting none of the tab's documents goes on representing the tab to a process holding part of
    // another tab of its group, whose documents can hold the tab's WindowProxies.
    if (page->client().holds_part_of_a_tab_in_the_group_of(*this)) {
        if (!is_representing_page(page))
            m_representing_pages.append(page);
        return;
    }
    if (is_representing_page(page))
        forget_representing_page(page);
    page->discard();
    did_lose_page(page, WebContentProcessLost::No);
}

Optional<ViewImplementation&> CanonicalTraversable::view() const
{
    if (!m_view)
        return {};
    return *m_view;
}

void CanonicalTraversable::set_view(Badge<ViewImplementation>, ViewImplementation& view)
{
    m_view = &view;
}

RefPtr<WebContentPage> CanonicalTraversable::display_page() const
{
    // The tab is displayed by the page hosting the traversable's document, or by the page standing in for the document
    // a crashed process destroyed until a document activates.
    auto entry = active_session_history_entry();
    if (entry && entry->document_state->document && entry->document_state->document->host())
        return entry->document_state->document->host();
    return m_page_standing_in_for_lost_document;
}

void CanonicalTraversable::set_page_standing_in_for_lost_document(Badge<ViewImplementation>, NonnullRefPtr<WebContentPage> page)
{
    did_lose_active_document();
    m_page_standing_in_for_lost_document = move(page);
}

void CanonicalTraversable::did_activate_document_in(Badge<CanonicalNavigable>, WebContentPage& host)
{
    // The page that stood in for the lost document is done, unless the document activated in it.
    auto stand_in_page = move(m_page_standing_in_for_lost_document);
    if (stand_in_page && stand_in_page != &host && stand_in_page->is_open())
        stop_hosting_in_page(*this, stand_in_page.release_nonnull());
}

ErrorOr<NonnullRefPtr<WebContentPage>> CanonicalTraversable::obtain_page_to_host_traversable(RefPtr<WebContentClient> host)
{
    auto view = this->view();
    if (!view.has_value())
        return Error::from_string_literal("No view displays the traversable");
    auto display_page = this->display_page();
    if (host && display_page && display_page->is_open() && host == &display_page->client())
        return display_page.release_nonnull();

    auto current_entry_descriptor = m_session_history.current_entry().descriptor();

    Web::PageId page_id;
    if (host && host->page_id_for_traversable(*this).has_value()) {
        page_id = *host->page_id_for_traversable(*this);
        host->async_begin_hosting_navigable(page_id, id(), current_entry_descriptor, system_visibility_state());
    } else if (host) {
        page_id = Application::the().allocate_page_id();
        host->async_create_embedded_page(page_id, remote_navigable_graph(), id(), current_entry_descriptor, system_visibility_state());
        host->register_embedded_page(page_id, *this);
        represent_group_in(*host);
    } else {
        auto process = TRY(Application::the().launch_child_frame_web_content_process(view->is_private(), remote_navigable_graph(), id(), current_entry_descriptor, system_visibility_state()));
        host = move(process.client);
        page_id = process.page_id;
        host->register_embedded_page(page_id, *this);
        represent_group_in(*host);
    }
    auto& page = *host->page(page_id);
    give_stand_in_its_opener(page);
    view->prepare_page_for_tab(page);
    return page;
}

Optional<CanonicalNavigable&> CanonicalTraversable::find(Web::HTML::CrossProcessId navigable_id)
{
    if (id() == navigable_id)
        return *this;

    auto navigable = m_navigable_index.get(navigable_id);
    if (!navigable.has_value() || !navigable.value())
        return {};

    return *navigable.value();
}

Optional<CanonicalNavigable const&> CanonicalTraversable::find(Web::HTML::CrossProcessId navigable_id) const
{
    if (id() == navigable_id)
        return *this;

    auto navigable = m_navigable_index.get(navigable_id);
    if (!navigable.has_value() || !navigable.value())
        return {};

    return *navigable.value();
}

void CanonicalTraversable::remove(CanonicalNavigable& navigable)
{
    VERIFY(&navigable != this);
    while (!navigable.children().is_empty())
        remove(*navigable.children().last());
    end_history_jobs(navigable);
    navigable.clear_ongoing_navigation();
    navigable.discard_pending_host();

    // The page hosting the navigable's document, when that is not the page holding its container, retires the
    // navigable's node when told, unloading the document if it still displays it; it holds the tab's graph without the
    // navigable from then on, or is released once it hosts nothing of the tab.
    RefPtr<WebContentPage> host = navigable.has_remote_host() ? &navigable.remote_host() : nullptr;
    if (host && host->is_open()) {
        VERIFY(navigable.replicated_state().has_value());
        host->async_stop_hosting_navigable(navigable.id(), *navigable.replicated_state());
    } else {
        host = nullptr;
    }

    // Every page holding the tab drops the navigable, but the page holding its container, which drops it on its own:
    // it reported the destruction, or the navigable is a child of a host on its way out.
    for_each_hosting_page([&](WebContentPage& page) {
        if (page == navigable.reporting_page())
            return;
        page.async_remove_remote_navigable(navigable.id());
    });
    remove_from_index(navigable);
    // The pages of the tab forget a destroyed focused navigable on their own.
    if (m_focused_navigable_id.has_value() && !find(*m_focused_navigable_id).has_value())
        m_focused_navigable_id.clear();

    auto* parent = navigable.parent();
    VERIFY(parent);
    (void)parent->remove_child(navigable);
    if (host)
        release_page_if_unused(host.release_nonnull());
}

void CanonicalTraversable::remove_page(WebContentPage& page)
{
    forget_representing_page(page);

    Vector<Web::HTML::CrossProcessId> reported_by_page;
    Vector<Web::HTML::CrossProcessId> hosted_by_page;
    Vector<Web::HTML::CrossProcessId> pending_in_page;
    for_each_in_subtree([&](CanonicalNavigable const& navigable) {
        if (navigable.reporting_page().ptr() == &page)
            reported_by_page.append(navigable.id());
        if (navigable.has_remote_host() && &navigable.remote_host() == &page)
            hosted_by_page.append(navigable.id());
        if (navigable.pending_host_matches(page))
            pending_in_page.append(navigable.id());
        return IterationDecision::Continue;
    });

    for (auto navigable_id : pending_in_page) {
        if (auto navigable = find(navigable_id); navigable.has_value())
            navigable->discard_pending_host(page);
    }

    // The documents the page hosted are destroyed, their child navigables first.
    for (auto navigable_id : reported_by_page) {
        if (auto navigable = find(navigable_id); navigable.has_value())
            remove(*navigable);
    }

    for (auto navigable_id : hosted_by_page) {
        if (auto navigable = find(navigable_id); navigable.has_value())
            stand_in_for_lost_document(*navigable);
    }
}

// NB: A navigable whose document another page hosts has a parent, whose document's page stands in for the lost one.
void CanonicalTraversable::stand_in_for_lost_document(CanonicalNavigable& navigable)
{
    auto reporting_page = navigable.reporting_page();
    navigable.hand_pending_webdriver_commands_to(*reporting_page);
    navigable.did_lose_active_document();
    if (auto state = navigable.replicated_state(); state.has_value())
        reporting_page->async_update_remote_navigable(navigable.id(), *state);
    // NB: The canonical session history can still lack the nested history of a newly created navigable.
    auto const* current_entry = m_session_history.get_the_target_history_entry(navigable, m_session_history.current_step());
    if (!current_entry)
        return;
    reporting_page->async_begin_hosting_navigable(navigable.id(), current_entry->descriptor(), system_visibility_state());
}

// The page's process holds the opener's tab once it represents the tab's group.
void CanonicalTraversable::give_stand_in_its_opener(WebContentPage& page)
{
    if (auto state = replicated_state(); state.has_value() && state->opener_navigable_id.has_value())
        page.async_set_opener_of_navigable(id(), *state->opener_navigable_id);
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#destroy-a-document-and-its-descendants
// The child navigables of a document that is gone went with it. The page that hosted the document destroyed them when
// it unloaded it, and reports no destruction for them.
void CanonicalTraversable::remove_child_navigables_of(CanonicalNavigable& navigable, CanonicalDocument const& document)
{
    Vector<Web::HTML::CrossProcessId> child_navigable_ids;
    for (auto const& child : navigable.children()) {
        if (child->container_document() == &document)
            child_navigable_ids.append(child->id());
    }
    for (auto child_navigable_id : child_navigable_ids) {
        if (auto child = find(child_navigable_id); child.has_value())
            remove(*child);
    }
}

void CanonicalTraversable::remove_from_index(CanonicalNavigable& navigable)
{
    navigable.for_each_in_inclusive_subtree([&](CanonicalNavigable& child) {
        m_navigable_index.remove(child.id());
        return IterationDecision::Continue;
    });
}

void CanonicalTraversable::session_history_changed()
{
    // The traversable's replicated state carries its session history entries's size.
    send_replicated_state();

    if (on_session_history_changed)
        on_session_history_changed();
}

Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> CanonicalTraversable::queued_same_document_session_history_entries(CanonicalNavigable const& navigable) const
{
    Vector<NonnullRefPtr<CanonicalSessionHistoryEntry>> entries;
    m_history_traversal_queue.for_each_synchronous_navigation_target_entry(navigable.id(), [&](CanonicalSessionHistoryEntry& entry) {
        entries.append(entry);
    });
    return entries;
}

ByteString CanonicalTraversable::queued_same_document_session_history_entries_for_debug() const
{
    StringBuilder builder;
    builder.append('[');
    bool first = true;
    for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        for (auto const& entry : queued_same_document_session_history_entries(navigable)) {
            if (!first)
                builder.append(", "sv);
            first = false;
            builder.appendff("{{navigable={}, url={}, document_state={}, navigation_id={}}}",
                navigable.id(), entry->url, entry->document_state->id, entry->navigation_api_id);
        }
        return IterationDecision::Continue;
    });
    builder.append(']');
    return builder.to_byte_string();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#reload
// NB: This is a reload from the browser's UI. One by script fires the navigate event in the process hosting the active
//     document, which then asks for the reload history step.
void CanonicalTraversable::reload()
{
    auto user_involvement = Web::HTML::UserNavigationInvolvement::BrowserUI;

    // NB: A reload from the browser's UI is the newest navigation. The navigation under way is abandoned, and the
    //     active document's load is no longer the one tracked.
    if (has_uncommitted_navigation())
        clear_ongoing_navigation();
    clear_active_document_load();

    // 2. Set navigable's active session history entry's document state's reload pending to true.
    active_session_history_entry()->document_state->reload_pending = true;
    session_history_changed();

    // 3. Let traversable be navigable's traversable navigable.
    // 4. Append the following session history traversal steps to traversable:
    //    1. Apply the reload history step to traversable given userInvolvement.
    auto parameters = Web::ReloadHistoryOperationParameters {
        .navigable_id = id(),
        .user_involvement = user_involvement,
    };
    enqueue_history_operation(Application::the().allocate_ui_process_cross_process_id(), parameters, {});
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#nav-stop
void CanonicalTraversable::stop_loading()
{
    // 1. Let document be navigable's active document.
    // 2. If document's unload counter is 0, and navigable's ongoing navigation is a navigation ID, then set the ongoing
    //    navigation for navigable to null.
    auto stopped_navigation = active_document().unload_counter() == 0 && has_uncommitted_navigation();
    if (stopped_navigation)
        clear_ongoing_navigation();
    else
        clear_active_document_load();

    // 3. Abort a document and its descendants given document.
    // NB: The page hosting document runs this step.
    if (auto page = display_page(); page && page->is_open())
        page->async_stop_loading();

    if (auto view = this->view(); view.has_value())
        view->did_stop_loading({}, stopped_navigation);
}

bool CanonicalTraversable::is_origin_held_by_a_document(URL::Origin const& origin)
{
    auto holds = [&](CanonicalDocument const& document) { return document.origin().is_same_origin(origin); };
    for (auto const& traversable : user_agent_top_level_traversable_set()) {
        auto decision = traversable->for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
            auto held = holds(navigable.active_document());
            navigable.for_each_populated_document_state([&](CanonicalDocumentState const& document_state) {
                held = held || holds(document_state.populated_document->document);
            });
            return held ? IterationDecision::Break : IterationDecision::Continue;
        });
        if (decision == IterationDecision::Break)
            return true;
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-traversable
CanonicalTraversable& CanonicalTraversable::create_a_new_top_level_traversable(Web::HTML::CrossProcessId id, Optional<CanonicalNavigable&> opener, Web::HTML::SessionHistoryEntryDescriptor initial_history_entry, Web::HTML::CrossProcessId document_id)
{
    // 1. Let document be null.
    RefPtr<CanonicalDocument> document;

    // 2. If opener is null, then set document to the second return value of creating a new top-level browsing context and document.
    if (!opener.has_value()) {
        document = CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(document_id, initial_history_entry.document_state.origin).document;
    }
    // 3. Otherwise, set document to the second return value of creating a new auxiliary browsing context and document given opener.
    else {
        document = CanonicalBrowsingContext::create_a_new_auxiliary_browsing_context_and_document(*opener, document_id).document;
    }

    // 4. Let documentState be a new document state, with
    //    document: document
    //    initiator origin: null if opener is null; otherwise, document's origin
    //    origin: document's origin
    //    navigable target name: targetName
    //    about base URL: document's about base URL
    // NB: initialHistoryEntry carries targetName, document's about base URL, and the entry's navigation API key and ID.
    auto history_entry = MUST(CanonicalSessionHistoryEntry::create_from_descriptor(initial_history_entry));
    history_entry->document_state->document = document;
    history_entry->document_state->initiator_origin = opener.has_value() ? Optional<URL::Origin> { document->origin() } : Optional<URL::Origin> {};
    history_entry->document_state->origin = document->origin();

    // 5. Let traversable be a new traversable navigable.
    // NB: Its session history entries start out as steps 7-9 leave them.
    history_entry->step = 0;
    auto traversable = make<CanonicalTraversable>(id, history_entry);

    // 6. Initialize the navigable traversable given documentState.
    traversable->set_current_session_history_entry(history_entry);
    traversable->set_active_session_history_entry(history_entry);
    // NB: The initial document is active from the traversable's creation, before a process hosts it.
    traversable->set_hosted_state({
        .active_document_is_fully_active = true,
        .is_closing = false,
        .container = {},
        .delays_the_load_event_of_its_container = false,
        // The process hosting the traversable reports the compositor context it paints through.
        .compositor_context_id = {},
    });

    // 7. Let initialHistoryEntry be traversable's active session history entry.
    // 8. Set initialHistoryEntry's step to 0.
    // 9. Append initialHistoryEntry to traversable's session history entries.
    // NB: Done as traversable was created.

    // 10. If opener is non-null, then legacy-clone a traversable storage shed given opener's top-level traversable and traversable. [STORAGE]
    if (opener.has_value())
        traversable->clone_session_storage_from(opener->top_level_traversable());

    // 11. Append traversable to the user agent's top-level traversable set.
    auto& traversable_ref = *traversable;
    user_agent_top_level_traversable_set().append(move(traversable));

    // FIXME: 12. Invoke WebDriver BiDi navigable created with traversable and openerNavigableForWebDriver.

    // 13. Return traversable.
    return traversable_ref;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-top-level-traversable
void CanonicalTraversable::remove_from_user_agent_top_level_traversable_set(CanonicalTraversable& traversable)
{
    // 5. Remove traversable from the user agent's top-level traversable set.
    // NB: The process hosting the traversable's document runs the other steps.
    user_agent_top_level_traversable_set().remove_first_matching([&](auto const& entry) { return entry.ptr() == &traversable; });
}

// The entry of navigable's session history entries that the process hosting navigable's active document names, or a
// same-document entry it created whose finalization is still queued.
RefPtr<CanonicalSessionHistoryEntry> CanonicalTraversable::session_history_entry_named(CanonicalNavigable const& navigable, Function<bool(CanonicalSessionHistoryEntry const&)> const& matches)
{
    VERIFY(&navigable.top_level_traversable() == this);

    // NB: A synchronous navigation's entry waits with the steps finalizing it before it is among navigable's session
    //     history entries.
    for (auto const& entry : queued_same_document_session_history_entries(navigable)) {
        if (matches(*entry))
            return entry;
    }
    auto entries = m_session_history.get_session_history_entries(navigable);
    if (!entries.has_value())
        return nullptr;
    for (auto const& entry : *entries) {
        if (matches(*entry))
            return entry;
    }
    return nullptr;
}

RefPtr<CanonicalSessionHistoryEntry> CanonicalTraversable::session_history_entry_named(CanonicalNavigable const& navigable, Web::HTML::SessionHistoryEntryIdentity const& entry_identity)
{
    return session_history_entry_named(navigable, [&](auto const& entry) { return entry.identity() == entry_identity; });
}

bool CanonicalTraversable::update_session_history_entry_navigation_api_state(CanonicalNavigable& navigable, Web::HTML::SessionHistoryEntryIdentity const& entry_identity, Web::HTML::StorageSerializationRecord navigation_api_state)
{
    auto entry = session_history_entry_named(navigable, entry_identity);
    if (!entry)
        return false;
    entry->navigation_api_state = move(navigation_api_state);
    session_history_changed();
    return true;
}

bool CanonicalTraversable::update_session_history_entry_scroll_restoration_mode(CanonicalNavigable& navigable, Web::HTML::SessionHistoryEntryIdentity const& entry_identity, Web::HTML::ScrollRestorationMode scroll_restoration_mode)
{
    auto entry = session_history_entry_named(navigable, entry_identity);
    if (!entry)
        return false;
    entry->scroll_restoration_mode = scroll_restoration_mode;
    session_history_changed();
    return true;
}

bool CanonicalTraversable::update_session_history_entry_persisted_state(CanonicalNavigable& navigable, Web::HTML::SessionHistoryEntryPersistedState const& persisted_state)
{
    auto entry = session_history_entry_named(navigable, persisted_state.entry_identity);
    if (!entry)
        return false;
    entry->scroll_position_data = persisted_state.scroll_position_data;
    session_history_changed();
    return true;
}

bool CanonicalTraversable::update_session_history_entry_document_state_navigable_target_name(CanonicalNavigable& navigable, Web::HTML::SessionHistoryEntryIdentity const& entry_identity, Utf16String navigable_target_name)
{
    auto entry = session_history_entry_named(navigable, entry_identity);
    if (!entry)
        return false;
    entry->document_state->navigable_target_name = move(navigable_target_name);
    // The navigable's target name is its active entry's document state's.
    if (entry->document_state == navigable.active_session_history_entry()->document_state)
        navigable.send_replicated_state();
    session_history_changed();
    return true;
}

bool CanonicalTraversable::set_session_history_entry_document_state_reload_pending(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, bool reload_pending)
{
    auto entry = session_history_entry_named(navigable, [&](auto const& entry) { return entry.navigation_api_key == navigation_api_key; });
    if (!entry)
        return false;
    entry->document_state->reload_pending = reload_pending;
    session_history_changed();
    return true;
}

Optional<i32> CanonicalTraversable::append_nested_history(CanonicalNavigable const& parent_navigable, CanonicalDocumentState& parent_document_state, Web::HTML::CrossProcessId child_navigable_id)
{
    VERIFY(&parent_navigable.top_level_traversable() == this);

    auto child_navigable = find(child_navigable_id);
    if (!child_navigable.has_value() || child_navigable->parent() != &parent_navigable)
        return {};
    // https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-new-child-navigable
    // 10. Let historyEntry be navigable's active session history entry.
    // NB: Read at the steps' queue position: the entry that initializing the navigable created is still its active
    //     entry, as a synchronous navigation of the navigable finalizes only once its nested history exists.
    auto target_step = m_session_history.append_nested_history(parent_navigable, parent_document_state, child_navigable_id, *child_navigable->active_session_history_entry());
    if (!target_step.has_value())
        return {};

    // The navigable has a session history entry from now on, which its replicated state tells its container.
    child_navigable->send_replicated_state();
    session_history_changed();
    return target_step;
}

bool CanonicalTraversable::remove_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id)
{
    VERIFY(&parent_navigable.top_level_traversable() == this);

    auto removed = m_session_history.remove_nested_history(parent_navigable, parent_document_state_id, child_navigable_id);
    if (removed)
        session_history_changed();
    return removed;
}

void CanonicalTraversable::reconstruct_the_history_to_step(i32 step)
{
    m_history_traversal_queue.append_session_history_traversal_steps([this, step](NonnullRefPtr<Core::Promise<Empty>> promise) {
        if (!m_session_history.used_steps().contains_slow(step)) {
            promise->resolve({});
            return;
        }
        set_current_session_history_entry({});
        run_browser_history_traversal_at_queue_position(step, false, next_sequence_number(), nullptr, move(promise));
    });
}

ErrorOr<URL::URL> CanonicalTraversable::restore_session_history_from_ui_snapshot(SessionHistorySnapshot snapshot)
{
    TRY(m_session_history.restore_from_ui_snapshot(move(snapshot.entries), move(snapshot.used_steps), snapshot.current_used_step_index, [] { return Application::the().allocate_ui_process_cross_process_id(); }));
    session_history_changed();

    return m_session_history.current_entry().url;
}

void CanonicalTraversable::reset_session_history_for_testing(
    Web::HTML::SessionHistoryEntryDescriptor active_entry)
{
    abandon_history_operations();
    auto entry = MUST(CanonicalSessionHistoryEntry::create_from_descriptor(active_entry));
    entry->document_state->document = active_document();
    set_current_session_history_entry(entry);
    set_active_session_history_entry(entry);
    m_session_history = TraversableSessionHistory(move(entry));
    session_history_changed();
}

bool CanonicalTraversable::initialize_session_history_for_testing(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index)
{
    abandon_history_operations();
    if (!m_session_history.initialize_for_testing(move(entries), move(used_steps), current_used_step_index))
        return false;
    auto& active_entry = m_session_history.current_entry();
    if (auto const& previous_active_entry = active_session_history_entry())
        active_entry.document_state->document = previous_active_entry->document_state->document;
    set_current_session_history_entry(active_entry);
    set_active_session_history_entry(active_entry);
    session_history_changed();
    return true;
}

StringView CanonicalTraversable::browser_history_traversal_stage_to_string(BrowserHistoryTraversalDiagnostic::Stage stage)
{
    switch (stage) {
    case BrowserHistoryTraversalDiagnostic::Stage::ApplyingInWebContent:
        return "applying-in-webcontent"sv;
    case BrowserHistoryTraversalDiagnostic::Stage::CheckingCancelation:
        return "checking-cancelation"sv;
    }
    VERIFY_NOT_REACHED();
}

struct HistoryOperation {
    AK_ALLOC_WITH_KMALLOC;

    // A traversal from the browser's UI, to the step selected at its queue position.
    struct BrowserTraversal {
        i32 target_step { 0 };
        bool check_for_cancelation { false };
        Function<void()> on_ready;
    };

    using Parameters = FlattenVariant<Web::HistoryOperationParameters, Variant<BrowserTraversal>>;

    HistoryOperation(Web::HTML::CrossProcessId operation_id, Parameters parameters, RefPtr<WebContentPage> requesting_page, u64 sequence_number, RefPtr<CanonicalSessionHistoryEntry> target_entry)
        : operation_id(operation_id)
        , parameters(move(parameters))
        , target_entry(move(target_entry))
        , requesting_page(move(requesting_page))
        , sequence_number(sequence_number)
    {
    }

    Web::HTML::CrossProcessId operation_id;
    Parameters parameters;
    // The entry synchronous navigation steps finalize.
    RefPtr<CanonicalSessionHistoryEntry> target_entry;
    // Jobs resolve their endpoints at dispatch. The requesting page owns the process-local operation state.
    RefPtr<WebContentPage> requesting_page;
    Vector<NonnullRefPtr<WebContentPage>> completion_endpoints;
    u64 sequence_number;
    Optional<Web::HTML::CrossProcessId> beforeunload_check_id;

    ChangingNavigable* pending_changing_job(Web::HTML::CrossProcessId navigable_id) const
    {
        auto* changing_navigable = algorithm ? algorithm->changing_navigable(navigable_id) : nullptr;
        return changing_navigable && changing_navigable->has_pending_job() ? changing_navigable : nullptr;
    }

    OwnPtr<ApplyHistoryStep> algorithm;
    RefPtr<Core::Promise<Empty>> queue_promise;

    bool is_browser_traversal() const { return parameters.has<BrowserTraversal>(); }
    BrowserTraversal& browser_traversal() { return parameters.get<BrowserTraversal>(); }
    BrowserTraversal const& browser_traversal() const { return parameters.get<BrowserTraversal>(); }
};

CanonicalTraversable::~CanonicalTraversable()
{
    // https://html.spec.whatwg.org/multipage/document-sequences.html#discard-a-browsing-context
    // The closed tab's browsing context is discarded, and removed from its group. It lives on as the opener browsing
    // context of the browsing contexts it opened.
    if (auto entry = active_session_history_entry(); entry && entry->document_state->document)
        entry->document_state->document->browsing_context().remove();

    // The tab closed the pages that held it before letting go of its traversable, so no open page names it.
    WebContentClient::for_each_client([&](WebContentClient& client) {
        client.for_each_page([&](WebContentPage& page) {
            VERIFY(&page.traversable() != this);
            return IterationDecision::Continue;
        });
        return IterationDecision::Continue;
    });
}

size_t CanonicalTraversable::effective_current_session_history_step_index() const
{
    if (m_browser_history_traversal) {
        if (auto target = m_session_history.traversal_target_for_step(m_browser_history_traversal->browser_traversal().target_step); target.has_value())
            return target->target_step_index;
    }
    return m_session_history.current_used_step_index();
}

HistoryOperation* CanonicalTraversable::ongoing_browser_history_traversal()
{
    return m_browser_history_traversal;
}

// The used step at delta from base_step, or none when there is no such step: steps 1-4 of traverse the history by a
// delta, from the step given.
static Optional<i32> used_step_at_delta(TraversableSessionHistory const& session_history, i32 base_step, int delta)
{
    auto all_steps = session_history.used_steps();
    auto base_step_index = all_steps.find_first_index(base_step);
    if (!base_step_index.has_value())
        return {};
    auto target_step_index = static_cast<i64>(*base_step_index) + static_cast<i64>(delta);
    if (target_step_index < 0 || static_cast<u64>(target_step_index) >= all_steps.size())
        return {};
    return all_steps[static_cast<size_t>(target_step_index)];
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta
void CanonicalTraversable::traverse_the_history_by_delta(int delta, CheckForCancelation check_for_cancelation, Function<void()> on_ready)
{
    // INTEROP: A press while a traversal from the browser's UI is applying supersedes it, and counts from the step
    //          that traversal was to reach, as in Chromium.
    if (auto* operation = ongoing_browser_history_traversal()) {
        auto target_step = used_step_at_delta(m_session_history, operation->browser_traversal().target_step, delta);
        if (!target_step.has_value()) {
            if (on_ready)
                on_ready();
            return;
        }
        supersede_browser_history_traversal(*operation, *target_step, move(on_ready));
        return;
    }

    // 1. Let sourceSnapshotParams and initiatorToCheck be null.
    // 2. Let userInvolvement be "browser UI".
    // 4. Append the following session history traversal steps to traversable:
    m_history_traversal_queue.append_session_history_traversal_steps([this, delta, check_for_cancelation, on_ready = move(on_ready)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_browser_ui_traversal_at_queue_position([this, delta]() -> Optional<i32> {
            // 1. Let allSteps be the result of getting all used history steps for traversable.
            // 2. Let currentStepIndex be the index of traversable's current session history step within allSteps.
            // 3. Let targetStepIndex be currentStepIndex plus delta.
            // 4. If allSteps[targetStepIndex] does not exist, then abort these steps.
            // 5. Apply the traverse history step allSteps[targetStepIndex] to traversable, given sourceSnapshotParams,
            //    initiatorToCheck, and userInvolvement.
            return used_step_at_delta(m_session_history, m_session_history.current_step(), delta);
        },
            check_for_cancelation, move(on_ready), move(promise));
    });
}

void CanonicalTraversable::traverse_the_history_to_step(i32 step, CheckForCancelation check_for_cancelation, Function<void()> on_ready)
{
    if (auto* operation = ongoing_browser_history_traversal()) {
        if (!m_session_history.used_steps().contains_slow(step)) {
            if (on_ready)
                on_ready();
            return;
        }
        supersede_browser_history_traversal(*operation, step, move(on_ready));
        return;
    }

    m_history_traversal_queue.append_session_history_traversal_steps([this, step, check_for_cancelation, on_ready = move(on_ready)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_browser_ui_traversal_at_queue_position([this, step]() -> Optional<i32> {
            if (!m_session_history.used_steps().contains_slow(step))
                return {};
            return step;
        },
            check_for_cancelation, move(on_ready), move(promise));
    });
}

// The steps a traversal from the browser's UI appends. Its step is selected at the queue position, once a navigation
// the traversal cancels is gone.
void CanonicalTraversable::run_browser_ui_traversal_at_queue_position(Function<Optional<i32>()> select_target_step, CheckForCancelation check_for_cancelation, Function<void()> on_ready, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    auto view = this->view();
    VERIFY(view.has_value());
    auto canceled_uncommitted_navigation = check_for_cancelation == CheckForCancelation::Yes && has_uncommitted_navigation();
    if (canceled_uncommitted_navigation)
        stop_loading();

    auto current_step = m_session_history.current_step();
    auto target_step = select_target_step();
    if (!target_step.has_value() || target_step == current_step) {
        auto reason = canceled_uncommitted_navigation
            ? "traverse-canceled-pending-navigation"sv
            : target_step == current_step ? "traverse-net-zero"sv
                                          : "traverse-no-entry"sv;
        view->dump_session_history(reason);
        if (on_ready)
            on_ready();
        if (view->on_browser_history_traversal_complete)
            view->on_browser_history_traversal_complete();
        promise->resolve({});
        return;
    }

    run_browser_history_traversal_at_queue_position(*target_step, check_for_cancelation == CheckForCancelation::Yes, next_sequence_number(), move(on_ready), move(promise));
}

void CanonicalTraversable::supersede_browser_history_traversal(HistoryOperation& operation, i32 target_step, Function<void()> on_ready)
{
    VERIFY(operation.queue_promise);
    auto promise = operation.queue_promise.release_nonnull();
    auto operation_id = operation.operation_id;

    if (has_uncommitted_navigation())
        stop_loading();
    finish_history_operation(operation_id);
    run_browser_history_traversal_at_queue_position(target_step, true, next_sequence_number(), move(on_ready), move(promise));
}

Optional<CanonicalTraversable::BrowserHistoryTraversalDiagnostic> CanonicalTraversable::browser_history_traversal_for_testing() const
{
    if (!m_browser_history_traversal)
        return {};
    auto target_step = m_browser_history_traversal->browser_traversal().target_step;
    auto target = m_session_history.traversal_target_for_step(target_step);
    if (!target.has_value())
        return {};
    return BrowserHistoryTraversalDiagnostic {
        .target_step = target_step,
        .target_step_index = target->target_step_index,
        .changes_top_level_entry = target->changes_top_level_entry,
        .stage = m_browser_history_traversal->beforeunload_check_id.has_value()
            ? BrowserHistoryTraversalDiagnostic::Stage::CheckingCancelation
            : BrowserHistoryTraversalDiagnostic::Stage::ApplyingInWebContent,
    };
}

// The navigation a cross-document navigation's finalization finalizes.
RefPtr<CanonicalNavigation> CanonicalTraversable::finalized_navigation(HistoryOperation const& operation) const
{
    auto const* parameters = operation.parameters.get_pointer<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>();
    if (!parameters)
        return nullptr;
    auto navigable = find(parameters->navigable_id);
    if (!navigable.has_value())
        return nullptr;
    auto navigation = navigable->navigation_with_id(parameters->navigation_id);
    return navigation && navigation->is_finalizing() ? navigation : nullptr;
}

void CanonicalTraversable::recover_from_web_content_process_crash()
{
    if (has_uncommitted_navigation())
        clear_ongoing_navigation();
    clear_active_document_load();

    // The step a traversal the crash interrupted was applying is applied again, otherwise the current step is.
    auto target_step = m_session_history.current_step();
    if (auto* traversal = ongoing_browser_history_traversal())
        target_step = traversal->browser_traversal().target_step;
    abandon_history_operations();

    // The tab's document is reloaded, the crashed process having destroyed the one its entry holds.
    auto* reloading_entry = m_session_history.get_the_target_history_entry(*this, target_step);
    VERIFY(reloading_entry);
    reloading_entry->document_state->reload_pending = true;
    session_history_changed();
    enqueue_browser_history_traversal(target_step, false);
}

RefPtr<WebContentPage> CanonicalTraversable::page_hosting(CanonicalNavigable const& navigable) const
{
    // A remote child is reported by the process containing its frame, but its active document lives in the
    // embedded page. Apply and commit work must follow the active document.
    if (navigable.has_remote_host())
        return navigable.remote_host();

    if (auto page = navigable.reporting_page())
        return page;

    // NB: The traversable is the view's root navigable; the process hosting its documents is the view's client
    //     rather than a reporting client recorded in the tree.
    if (&navigable == this)
        return display_page();
    return {};
}

void CanonicalTraversable::did_lose_page(WebContentPage& page, WebContentProcessLost process_lost)
{
    if (auto view = this->view(); view.has_value())
        view->did_lose_page({}, page, process_lost);
}

void CanonicalTraversable::did_not_receive_reply(OwedReply const& owed)
{
    // The tab's document is recovered by applying the interrupted step again in a page obtained after the crash.
    auto awaits_crash_recovery = !display_page()->is_open();

    switch (owed.kind) {
    case OwedReply::Kind::BeforeunloadCheck: {
        auto check = m_pending_beforeunload_checks.find(owed.id);
        if (check == m_pending_beforeunload_checks.end() || (check->value.target_entry && awaits_crash_recovery))
            return;
        dispatch_next_beforeunload_group(owed.id);
        return;
    }
    case OwedReply::Kind::DescendantUnload:
        complete_document_unload_task(owed.id);
        return;
    case OwedReply::Kind::NonchangingUpdate: {
        auto* operation = find_history_operation(owed.id);
        if (!operation || awaits_crash_recovery)
            return;
        if (operation->algorithm)
            operation->algorithm->nonchanging_navigable_updated();
        return;
    }
    case OwedReply::Kind::ChangingJob:
    case OwedReply::Kind::ContinuationApplied: {
        auto* operation = find_history_operation(owed.id);
        if (!operation || awaits_crash_recovery)
            return;
        if (auto* changing_navigable = operation->pending_changing_job(owed.navigable_id))
            operation->algorithm->changing_navigable_job_completed(*changing_navigable, Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped);
        return;
    }
    }
    VERIFY_NOT_REACHED();
}

HistoryOperation* CanonicalTraversable::find_history_operation(Web::HTML::CrossProcessId operation_id)
{
    auto operation = m_history_operations.find(operation_id);
    if (operation == m_history_operations.end())
        return nullptr;
    return operation->value.ptr();
}

ApplyHistoryStep* CanonicalTraversable::find_history_step_run(Web::HTML::CrossProcessId operation_id)
{
    auto* operation = find_history_operation(operation_id);
    return operation ? operation->algorithm.ptr() : nullptr;
}

void CanonicalTraversable::add_history_operation_completion_endpoint(HistoryOperation& operation, NonnullRefPtr<WebContentPage> endpoint)
{
    if (any_of(operation.completion_endpoints, [&](auto const& completion_endpoint) { return completion_endpoint == endpoint; }))
        return;
    operation.completion_endpoints.append(move(endpoint));
}

void CanonicalTraversable::did_finish_history_navigation_params_creation(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryNavigationPopulation population)
{
    auto navigable_id = population.request.navigable_id;
    auto discard = [&] { NavigationLoader::discard(source_page.client().is_private(), population.result); };
    auto* operation = find_history_operation(operation_id);
    auto* changing_navigable = operation ? operation->pending_changing_job(navigable_id) : nullptr;
    if (!changing_navigable || changing_navigable->population_loader
        || !source_page.take_owed_reply({ OwedReply::Kind::ChangingJob, operation_id, navigable_id })) {
        discard();
        return;
    }
    // NB: The process populating the entry echoes its request. A file: URL in it that is not the entry's would give the
    //     process a document created from another file: URL, and possibly with it local file access.
    if (population.request.history_entry.url.scheme() == "file"sv && population.request.history_entry.url != changing_navigable->target_entry->url) {
        discard();
        did_fail_history_navigation_population(*operation, *changing_navigable);
        return;
    }
    // The document's sandboxing flags are at least those the UI process snapshots for the navigable.
    population.request.target_snapshot_params.sandboxing_flags |= changing_navigable->navigable().snapshot_target_snapshot_params().sandboxing_flags;
    auto& loader = changing_navigable->population_loader;
    loader = NavigationLoader::create(source_page.client().is_private(), move(population.request));
    loader->did_finish_navigation_params_creation(move(population.result));
    loader->acquire_response_body([this, operation_id, navigable_id](bool succeeded) {
        auto* operation = find_history_operation(operation_id);
        auto* changing_navigable = operation ? operation->pending_changing_job(navigable_id) : nullptr;
        if (!changing_navigable)
            return;
        if (!succeeded) {
            did_fail_history_navigation_population(*operation, *changing_navigable);
            return;
        }
        continue_history_navigation_population(*operation, *changing_navigable);
    });
}

void CanonicalTraversable::did_fail_history_navigation_population(HistoryOperation& operation, ChangingNavigable& changing_navigable)
{
    operation.algorithm->changing_navigable_job_completed(changing_navigable, Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped);
}

void CanonicalTraversable::continue_history_navigation_population(HistoryOperation& operation, ChangingNavigable& changing_navigable)
{
    if (!changing_navigable.population_loader)
        return;
    RefPtr<WebContentPage> endpoint = changing_job_endpoint(changing_navigable);
    auto loader = move(changing_navigable.population_loader);

    NonnullRefPtr target_entry = *changing_navigable.target_entry;
    auto operation_id = operation.operation_id;
    auto navigable_id = changing_navigable.id;
    auto navigable = changing_navigable.navigable().make_weak_ptr();
    loader->obtain_the_response_policies(*navigable);
    loader->check_the_responses_adherence_to_its_embedder_policy(*navigable);
    auto host_or_error = navigable->obtain_page_to_populate_document(target_entry, *loader, *endpoint);

    // Obtaining the page can replace the tab's process, whose change callbacks can end the job, and with its operation
    // the record.
    if (!find_history_operation(operation_id) || !operation.pending_changing_job(navigable_id)) {
        if (navigable && !host_or_error.is_error() && host_or_error.value().document)
            navigable->abandon_populated_document(target_entry->document_state, *host_or_error.value().document);
        return;
    }
    if (host_or_error.is_error()) {
        did_fail_history_navigation_population(operation, changing_navigable);
        return;
    }
    auto host = host_or_error.release_value();
    if (host.document)
        changing_navigable.populated_document = host.document;
    endpoint = host.page;
    add_history_operation_completion_endpoint(operation, *endpoint);
    changing_navigable.population_loader = move(loader);
    auto finish_handoff = [this, operation_id, navigable_id, weak_loader = changing_navigable.population_loader->make_weak_ptr(), endpoint](bool succeeded) {
        auto* operation = find_history_operation(operation_id);
        auto* changing_navigable = operation ? operation->pending_changing_job(navigable_id) : nullptr;
        if (!changing_navigable || !weak_loader || changing_navigable->population_loader.ptr() != weak_loader.ptr())
            return;
        if (!succeeded || !endpoint->is_open()) {
            did_fail_history_navigation_population(*operation, *changing_navigable);
            return;
        }
        auto& loader = *changing_navigable->population_loader;
        endpoint->owe_reply({ OwedReply::Kind::ChangingJob, operation_id, navigable_id });
        endpoint->async_continue_history_navigation_population(operation_id, changing_navigable->target_entry_descriptor(), operation->algorithm->navigation_type(),
            changing_navigable->navigable().ongoing_navigation().has<CanonicalNavigable::Traversal>(), Web::HTML::HistoryNavigationPopulation { loader.request(), loader.take_result() });
    };
    if (auto client_id = endpoint->client().request_server_site_bindings().client_id(); client_id.has_value()) {
        if (auto request_server = RequestServerManager::the().instance_for_client(*client_id)) {
            changing_navigable.population_loader->move_response_body_to(*request_server, move(finish_handoff));
            return;
        }
    }
    finish_handoff(false);
}

bool CanonicalTraversable::run_changing_navigable_history_step_job(ApplyHistoryStep& run, ChangingNavigable& changing_navigable)
{
    auto& operation = run.operation();
    auto navigable_id = changing_navigable.id;
    auto& navigable = changing_navigable.navigable();
    auto endpoint = changing_job_endpoint(changing_navigable);
    if (!endpoint->is_open())
        return false;

    if (navigable.is_top_level_traversable()) {
        if (!operation.parameters.has<Web::ReloadHistoryOperationParameters>()
            && changing_navigable.target_entry->document_state->document.ptr() != &navigable.active_document()) {
            if (auto view = this->view(); view.has_value())
                view->reset_blocked_request_count();
        }
        if (operation.is_browser_traversal()) {
            if (auto callback = move(operation.browser_traversal().on_ready))
                callback();
        }
    }

    add_history_operation_completion_endpoint(operation, *endpoint);

    // A reload's top-level job repopulates the view's document; begin the recorded load where the job is
    // dispatched instead of having the job echo it back.
    if (navigable.is_top_level_traversable()
        && operation.parameters.has<Web::ReloadHistoryOperationParameters>()) {
        // A load a history operation starts, rather than a navigation, is tracked as the active document's.
        clear_active_document_load();
        endpoint->begin_top_level_load(changing_navigable.target_entry->url);
    }

    Optional<Utf16String> finalized_navigation_id;
    if (auto const* parameters = operation.parameters.get_pointer<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>(); parameters && parameters->navigable_id == navigable_id) {
        finalized_navigation_id = parameters->navigation_id;
        if (auto const& populated_document = changing_navigable.target_entry->document_state->populated_document; populated_document.has_value())
            changing_navigable.populated_document = populated_document->document;
    }
    endpoint->owe_reply({ OwedReply::Kind::ChangingJob, operation.operation_id, navigable_id });
    endpoint->async_run_changing_navigable_history_job(
        operation.operation_id, navigable_id,
        changing_navigable.target_entry_descriptor(), run.user_involvement(),
        run.navigation_type(),
        finalized_navigation_id,
        navigable.ongoing_navigation().has<CanonicalNavigable::Traversal>(),
        run.source_snapshot_params());
    return true;
}

void CanonicalTraversable::apply_changing_navigable_history_step_continuation(ApplyHistoryStep& run, ChangingNavigable& changing_navigable)
{
    VERIFY(changing_navigable.continuation.has_value());

    // 10. If changingNavigableContinuation's update-only is true, or targetEntry's document is displayedDocument:
    if (changing_navigable.update_only || changing_navigable.target_document_id == changing_navigable.displayed_document->id()) {
        // 1. Set navigable's ongoing navigation to null.
        // NB: The canonical traversal claim is released when the continuation is applied. The queued task mirrors
        //     the null transition into the process-local projection.

        // 2. Queue a global task on the navigation and traversal task source given navigable's active window to
        //    perform afterPotentialUnloads.
        send_changing_navigable_continuation_task(run.operation(), changing_navigable);
    } else {
        // 11. Otherwise:
        // 1. Assert: navigationType is not null.
        // AD-HOC: Except for a navigable created by a document repopulated for its entry, which displays its initial
        //         about:blank until an update for a navigable's creation or destruction restores the entry its nested
        //         history kept. See https://github.com/whatwg/html/issues/12724.
        VERIFY(run.navigation_type().has_value() || changing_navigable.navigable().active_document().is_initial_about_blank());

        // 2. Deactivate displayedDocument, given userInvolvement, targetEntry, navigationType, and
        //    afterPotentialUnloads.
        deactivate_a_document_for_cross_document_navigation(run.operation(), changing_navigable);
    }
}

void CanonicalTraversable::send_changing_navigable_continuation_task(HistoryOperation& operation, ChangingNavigable& changing_navigable)
{
    VERIFY(changing_navigable.has_pending_job());
    VERIFY(changing_navigable.continuation.has_value());
    auto endpoint = changing_job_endpoint(changing_navigable);
    auto continuation = *changing_navigable.continuation;
    endpoint->owe_reply({ OwedReply::Kind::ContinuationApplied, operation.operation_id, changing_navigable.id });
    endpoint->async_apply_changing_navigable_continuation(
        operation.operation_id, changing_navigable.id, changing_navigable.target_entry->identity(),
        continuation.history_object_length_and_index.script_history_length,
        continuation.history_object_length_and_index.script_history_index,
        move(continuation.entries_for_navigation_api));
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#unload-a-document-and-its-descendants
// 6. Queue a global task on the navigation and traversal task source given document's relevant global object to perform
//    the following steps:
//    1. If firePageSwapSteps is given, then run firePageSwapSteps.
//    2. Unload document, passing along newDocument if it is not null.
//    3. If afterAllUnloads was given, then run it.
// NB: For displayedDocument, in the process running the changing navigable's job, where afterPotentialUnloads runs.
void CanonicalTraversable::send_displayed_document_unload_task(HistoryOperation& operation, ChangingNavigable& changing_navigable)
{
    VERIFY(changing_navigable.has_pending_job());
    VERIFY(changing_navigable.continuation.has_value());
    auto endpoint = changing_job_endpoint(changing_navigable);

    auto const& document_state = *changing_navigable.target_entry->document_state;
    if (changing_navigable.target_entry_reload_pending && operation.parameters.has<Web::ReloadHistoryOperationParameters>()) {
        // INTEROP: Reloading rebuilds the child history tree from the replacement document, as in WebKit.
        //          Keep the old entries until population succeeds so an abandoned reload preserves them.
        Vector<Web::HTML::CrossProcessId> nested_history_ids;
        for (auto const& nested_history : document_state.nested_histories)
            nested_history_ids.append(nested_history.id);
        for (auto nested_history_id : nested_history_ids)
            remove_nested_history(changing_navigable.navigable(), document_state.id, nested_history_id);
    }

    auto continuation = *changing_navigable.continuation;
    endpoint->owe_reply({ OwedReply::Kind::ContinuationApplied, operation.operation_id, changing_navigable.id });
    endpoint->async_unload_displayed_document_and_apply_changing_navigable_continuation(
        operation.operation_id, changing_navigable.id, changing_navigable.target_entry->identity(),
        continuation.history_object_length_and_index.script_history_length,
        continuation.history_object_length_and_index.script_history_index,
        move(continuation.entries_for_navigation_api));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#deactivate-a-document-for-a-cross-document-navigation
void CanonicalTraversable::deactivate_a_document_for_cross_document_navigation(HistoryOperation& operation, ChangingNavigable& changing_navigable)
{
    // 1. Let navigable be displayedDocument's node navigable.
    // 2. Let potentiallyTriggerViewTransition be false.
    // FIXME: 3. Let isBrowserUINavigation be true if userNavigationInvolvement is "browser UI"; otherwise false.
    // FIXME: 4. Set potentiallyTriggerViewTransition to the result of calling can navigation trigger a
    //           cross-document view-transition? given displayedDocument, targetEntry's document, navigationType,
    //           and isBrowserUINavigation.

    // 5. If potentiallyTriggerViewTransition is false, then:
    // FIXME: 1. Let firePageSwapBeforeUnload be the following step:
    //            1. Fire the pageswap event given displayedDocument, targetEntry, navigationType, and null.

    // 2. Set navigable's ongoing navigation to null.
    // NB: The process running the job also clears its projection of the navigable's ongoing navigation, before it runs
    //     any unload task step 3 sends it.
    changing_navigable.navigable().clear_ongoing_navigation_traversal(operation.operation_id);
    auto endpoint = changing_job_endpoint(changing_navigable);
    endpoint->async_clear_ongoing_history_traversal(operation.operation_id, changing_navigable.id);

    // 3. Unload a document and its descendants given displayedDocument, targetEntry's document,
    //    afterPotentialUnloads, and firePageSwapBeforeUnload.
    unload_a_document_and_its_descendants(changing_navigable.navigable(), endpoint, Web::HTML::ChildNavigableDestruction::No, [this, operation_id = operation.operation_id, navigable_id = changing_navigable.id] {
        // NB: The process running the job unloads the document it displays: displayedDocument, or the stand-in of a
        //     host chosen for the new document.
        auto* operation = find_history_operation(operation_id);
        if (auto* changing_navigable = operation ? operation->pending_changing_job(navigable_id) : nullptr)
            send_displayed_document_unload_task(*operation, *changing_navigable);
    });

    // FIXME: 6. Otherwise, queue a global task on the navigation and traversal task source given navigable's active window to run the steps:
    //            1. Let proceedWithNavigationAfterViewTransitionCapture be the following step:
    //               1. Append the following session history traversal steps to navigable's traversable navigable:
    //                  1. Set navigable's ongoing navigation to null.
    //                  2. Unload a document and its descendants given displayedDocument, targetEntry's document, and afterPotentialUnloads.
    //            2. Let viewTransition be the result of setting up a cross-document view-transition given displayedDocument,
    //               targetEntry's document, navigationType, and proceedWithNavigationAfterViewTransitionCapture.
    //            3. Fire the pageswap event given displayedDocument, targetEntry, navigationType, and viewTransition.
    //            4. If viewTransition is null, then run proceedWithNavigationAfterViewTransitionCapture.
}

// The process running a changing navigable's job activated targetEntry's document and applied the continuation's
// remaining steps.
void CanonicalTraversable::did_activate_history_entry(HistoryOperation& operation, ChangingNavigable& changing_navigable, NonnullRefPtr<WebContentPage> source_page, Web::HTML::HostedNavigableState activated_navigable_state)
{
    auto& navigable = changing_navigable.navigable();
    auto& target_entry = *changing_navigable.target_entry;

    Optional<Utf16String const&> navigation_id;
    if (auto navigation = finalized_navigation(operation))
        navigation_id = navigation->navigation_id;
    auto const* replaced_document = &navigable.active_document();
    // NB: The page that activated the document hosts it: the job's, which hosts the document the job populated, or else
    //     the navigable's.
    navigable.did_commit_navigation(target_entry, move(activated_navigable_state), operation.sequence_number, navigation_id, move(source_page));

    // NB: Another operation's job for the document this replaced cannot run: destroying the document drops its tasks.
    //     This operation's job has ended already.
    if (&navigable.active_document() != replaced_document)
        end_history_jobs(navigable);

    if (&navigable == this) {
        if (auto view = this->view(); view.has_value()) {
            if (&navigable.active_document() != replaced_document)
                view->did_change_active_document({});
            // NB: The address bar can already show a pending navigation's URL while the old document
            //     is still visible. Only apply the destination's zoom after its document is activated.
            view->apply_zoom_for_current_host();
            view->m_external_url_request_policy.clear_page_request_allowance();
            if (view->on_top_level_navigation_commit)
                view->on_top_level_navigation_commit();
        }
    }
}

// https://html.spec.whatwg.org/multipage/document-lifecycle.html#unload-a-document-and-its-descendants
void CanonicalTraversable::unload_a_document_and_its_descendants(CanonicalNavigable& navigable, RefPtr<WebContentPage> continuing_endpoint, Web::HTML::ChildNavigableDestruction child_navigable_destruction, Function<void()> queue_document_unload_task)
{
    // 1. Assert: this is running within document's node navigable's traversable navigable's session history
    //    traversal queue.
    // NB: The UI process owns that queue. The recursion runs here because the descendant subtrees can be hosted by
    //     other WebContent processes.

    // NB: A document is unloaded in the page hosting it, whichever that is, before the document replacing it
    //     activates. That is the page continuing the invoking algorithm for the traversable's document, unless another
    //     page displays it while the view's process populates the document replacing it.
    NonnullRefPtr document = navigable.active_document();
    RefPtr<WebContentPage> document_host = document->host();
    if (!document_host)
        document_host = page_hosting(navigable);
    document->increment_unload_counter();

    unload_the_documents_of_child_navigables(navigable, [this, document, navigable_id = navigable.id(), continuing_endpoint = move(continuing_endpoint), child_navigable_destruction, document_host = move(document_host), queue_document_unload_task = move(queue_document_unload_task)] mutable {
        // 6. Queue a global task on the navigation and traversal task source given document's relevant global object
        //    to perform the following steps:
        //    1. If firePageSwapSteps is given, then run firePageSwapSteps.
        //    2. Unload document, passing along newDocument if it is not null.
        //    3. If afterAllUnloads was given, then run it.
        // NB: queue_document_unload_task dispatches this task to the page continuing the invoking algorithm. When
        //     another page hosts the document, that page unloads it first.
        if (!document_host || document_host == continuing_endpoint) {
            document->decrement_unload_counter();
            queue_document_unload_task();
            return;
        }
        // NB: Another page replaces or destroys the document. This one represents the navigable remotely from the
        //     moment the document is unloaded.
        run_document_unload_task(move(document_host), navigable_id, child_navigable_destruction, Web::HTML::StopHostingAfterUnload::Yes, [document, queue_document_unload_task = move(queue_document_unload_task)] {
            document->decrement_unload_counter();
            queue_document_unload_task();
        });
    });
}

namespace {

// numberUnloaded, and the steps waiting until it equals the number of child navigables.
struct ChildNavigableUnloads : RefCounted<ChildNavigableUnloads> {
    ChildNavigableUnloads(size_t child_navigable_count, Function<void()> after_child_navigable_unloads)
        : child_navigable_count(child_navigable_count)
        , after_child_navigable_unloads(move(after_child_navigable_unloads))
    {
    }

    size_t number_unloaded { 0 };
    size_t child_navigable_count { 0 };
    Function<void()> after_child_navigable_unloads;
};

}

// Steps 2 to 5 of unload a document and its descendants, for navigable's active document.
void CanonicalTraversable::unload_the_documents_of_child_navigables(CanonicalNavigable const& navigable, Function<void()> after_child_navigable_unloads)
{
    // 2. Let childNavigables be document's child navigables.
    Vector<Web::HTML::CrossProcessId> child_navigables;
    for (auto const& child_navigable : navigable.children())
        child_navigables.append(child_navigable->id());
    if (child_navigables.is_empty()) {
        after_child_navigable_unloads();
        return;
    }

    // 3. Let numberUnloaded be 0.
    auto unloads = adopt_ref(*new ChildNavigableUnloads(child_navigables.size(), move(after_child_navigable_unloads)));

    // 4. For each childNavigable of childNavigables [[ in what order? ]], queue a global task on the navigation and
    //    traversal task source given childNavigable's active window to perform the following steps:
    for (auto child_navigable_id : child_navigables) {
        // 1. Let incrementUnloaded be an algorithm step which increments numberUnloaded.
        auto increment_unloaded = [unloads] {
            ++unloads->number_unloaded;

            // 5. Wait until numberUnloaded equals childNavigable's size.
            if (unloads->number_unloaded == unloads->child_navigable_count)
                unloads->after_child_navigable_unloads();
        };

        // 2. Unload a document and its descendants given childNavigable's active document, null, and
        //    incrementUnloaded.
        // NB: A child navigable that is gone by then counts as unloaded.
        auto child_navigable = find(child_navigable_id);
        if (!child_navigable.has_value()) {
            increment_unloaded();
            continue;
        }
        // NB: A page unloads the documents it hosts even when an operation's jobs left it for the process replacing it.
        auto endpoint = page_hosting(*child_navigable);
        unload_the_documents_of_child_navigables(*child_navigable, [this, endpoint = move(endpoint), child_navigable_id, increment_unloaded = move(increment_unloaded)] mutable {
            // 6. Queue a global task on the navigation and traversal task source given document's relevant global
            //    object to perform the following steps:
            //    1. If firePageSwapSteps is given, then run firePageSwapSteps.
            //    2. Unload document, passing along newDocument if it is not null.
            //    3. If afterAllUnloads was given, then run it.
            run_document_unload_task(move(endpoint), child_navigable_id, Web::HTML::ChildNavigableDestruction::No, Web::HTML::StopHostingAfterUnload::No, move(increment_unloaded));
        });
    }
}

// The task step 6 of unload a document and its descendants queues, in the page hosting the document.
void CanonicalTraversable::run_document_unload_task(RefPtr<WebContentPage> endpoint, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChildNavigableDestruction child_navigable_destruction, Web::HTML::StopHostingAfterUnload stop_hosting_after_unload, Function<void()> after_unload)
{
    if (!endpoint || !endpoint->is_open()) {
        after_unload();
        return;
    }
    auto unload_id = Application::the().allocate_ui_process_cross_process_id();
    m_document_unload_tasks.set(unload_id, move(after_unload));
    endpoint->owe_reply({ OwedReply::Kind::DescendantUnload, unload_id, navigable_id });
    endpoint->async_run_descendant_unload_task(unload_id, navigable_id, child_navigable_destruction, stop_hosting_after_unload);
}

void CanonicalTraversable::complete_document_unload_task(Web::HTML::CrossProcessId unload_id)
{
    if (auto after_unload = m_document_unload_tasks.take(unload_id); after_unload.has_value())
        (*after_unload)();
}

// Whether the page hosting the navigable's active document is unloading it, or has, for a document another page hosts.
// That page stops hosting the navigable once the document is unloaded, and the other page takes it over only when the
// document replacing it is activated.
bool CanonicalTraversable::is_handing_navigable_to_another_page(CanonicalNavigable const& navigable) const
{
    for (auto const& operation : m_history_operations) {
        auto const* changing_navigable = operation.value->pending_changing_job(navigable.id());
        if (changing_navigable
            && changing_navigable->continuation.has_value()
            // NB: Step 14.11 of apply the history step applies the continuation, which deactivates displayedDocument.
            && !changing_navigable->update_only
            && changing_navigable->target_document_id != changing_navigable->displayed_document->id()
            && changing_job_endpoint(*changing_navigable) != navigable.active_document().host()) {
            return true;
        }
    }
    return false;
}

// A history step's changing job populates its target entry's document in the process hosting the navigable, which need
// not have hosted local file content. For an entry whose document state held local file content, that process reads the
// file the entry's file: URL names.
bool CanonicalTraversable::page_populates_a_history_entry_for_local_file(WebContentPage const& page, ByteString const& path) const
{
    for (auto const& [operation_id, operation] : m_history_operations) {
        if (!operation->algorithm)
            continue;
        for (auto const& changing_navigable : operation->algorithm->changing_navigables()) {
            if (!changing_navigable->has_pending_job())
                continue;
            auto const& target_entry = *changing_navigable->target_entry;
            if (target_entry.document_state->held_local_file_content
                && target_entry.url.scheme() == "file"sv
                && target_entry.url.file_path() == path
                && changing_job_endpoint(*changing_navigable) == &page)
                return true;
        }
    }
    return false;
}

RefPtr<WebContentPage> CanonicalTraversable::changing_job_endpoint(CanonicalNavigable const& navigable, CanonicalDocumentState const& target_document_state) const
{
    if (auto const& populated_document = target_document_state.populated_document; populated_document.has_value() && populated_document->document->host())
        return populated_document->document->host();
    return page_hosting(navigable);
}

// NB: A pending job's navigable is in the tree, so its document's page, or the page holding its container, is there: a
//     navigable whose container's page is lost is removed with it, ending its job.
NonnullRefPtr<WebContentPage> CanonicalTraversable::changing_job_endpoint(ChangingNavigable const& changing_navigable) const
{
    auto endpoint = changing_job_endpoint(changing_navigable.navigable(), *changing_navigable.target_entry->document_state);
    VERIFY(endpoint);
    return endpoint.release_nonnull();
}

void CanonicalTraversable::did_receive_descendant_unload_task_complete(WebContentPage& source_page, Web::HTML::CrossProcessId unload_id, Web::HTML::CrossProcessId navigable_id)
{
    if (source_page.take_owed_reply({ OwedReply::Kind::DescendantUnload, unload_id, navigable_id }))
        complete_document_unload_task(unload_id);
}

void CanonicalTraversable::did_receive_child_navigable_unload_request(WebContentPage& source_page, Web::HTML::CrossProcessId navigable_id)
{
    auto navigable = find(navigable_id);
    if (!navigable.has_value()) {
        source_page.async_continue_child_navigable_destruction(navigable_id);
        return;
    }

    // AD-HOC: Child removal unloads the document tree before continuing the destroy a child navigable algorithm.
    // NB: The container holds a local navigable exactly when the requesting page hosts the document, so that page
    //     unloads it as it continues, having informed its navigation API itself; a remote navigable's document is
    //     unloaded in its host, which informs the navigation API there first.
    unload_a_document_and_its_descendants(*navigable, source_page, Web::HTML::ChildNavigableDestruction::Yes, [source_page = NonnullRefPtr<WebContentPage>(source_page), navigable_id] {
        source_page->async_continue_child_navigable_destruction(navigable_id);
    });
}

void CanonicalTraversable::end_history_jobs(CanonicalNavigable const& navigable, CanonicalDocument const* populated_document)
{
    // NB: A job ends now, but its run goes on later: It can go on to dispatch other jobs, which must not see the tree
    //     mid-change. An update of a nonchanging navigable needs no ending: the page it was sent to replies to it even
    //     if the navigable is gone, or is closed, which ends it.
    Vector<Web::HTML::CrossProcessId> operations_with_ended_jobs;
    for (auto& entry : m_history_operations) {
        auto& operation = *entry.value;
        if (auto* changing_navigable = operation.pending_changing_job(navigable.id()); changing_navigable && (!populated_document || changing_navigable->populated_document == populated_document)) {
            operation.algorithm->end_changing_navigable_job(*changing_navigable);
            operations_with_ended_jobs.append(operation.operation_id);
        }
    }
    if (operations_with_ended_jobs.is_empty())
        return;

    Core::deferred_invoke([this, weak_this = make_weak_ptr<CanonicalTraversable>(), operations_with_ended_jobs = move(operations_with_ended_jobs)] {
        if (!weak_this)
            return;
        for (auto operation_id : operations_with_ended_jobs) {
            if (auto* operation = find_history_operation(operation_id))
                operation->algorithm->process_changing_navigable_continuations();
        }
    });
}

void CanonicalTraversable::check_if_unloading_is_canceled(ApplyHistoryStep& run, Vector<Web::HTML::CrossProcessId> navigables_that_need_before_unload, NonnullRefPtr<CanonicalSessionHistoryEntry> target_entry, Web::HTML::UserNavigationInvolvement user_involvement_for_navigate_event, Function<void(Web::HTML::CheckIfUnloadingIsCanceledResult)> on_complete)
{
    auto& operation = run.operation();
    auto check_id = check_if_unloading_is_canceled(move(navigables_that_need_before_unload), move(target_entry), user_involvement_for_navigate_event, {}, Web::HTML::UnloadPromptShown::No,
        [this, operation_id = operation.operation_id, on_complete = move(on_complete)](Web::HTML::CheckIfUnloadingIsCanceledResult result, Web::HTML::UnloadPromptShown) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            operation->beforeunload_check_id.clear();
            on_complete(result);
        });
    // NB: A check that finishes before it returns has no ID left to record. The operation it may have finished is not
    //     destroyed until later.
    operation.beforeunload_check_id = check_id;
}

void CanonicalTraversable::queue_navigation_api_state_clear_task(ApplyHistoryStep& run, ChangingNavigable& changing_navigable)
{
    auto endpoint = page_hosting(changing_navigable.navigable());
    VERIFY(endpoint);
    add_history_operation_completion_endpoint(run.operation(), *endpoint);
    endpoint->async_queue_navigation_api_state_clear_task(changing_navigable.id);
}

bool CanonicalTraversable::update_nonchanging_navigable_history_step_state(ApplyHistoryStep& run, Web::HTML::CrossProcessId navigable_id, Web::HTML::HistoryObjectLengthAndIndex history_object_length_and_index)
{
    auto& operation = run.operation();
    auto navigable = find(navigable_id);
    RefPtr<WebContentPage> endpoint = navigable.has_value() ? page_hosting(*navigable) : nullptr;
    if (!endpoint || !endpoint->is_open())
        return false;
    add_history_operation_completion_endpoint(operation, *endpoint);
    endpoint->owe_reply({ OwedReply::Kind::NonchangingUpdate, operation.operation_id, navigable_id });
    endpoint->async_update_nonchanging_navigable_history_state(operation.operation_id, navigable_id,
        history_object_length_and_index.script_history_length, history_object_length_and_index.script_history_index);
    return true;
}

void CanonicalTraversable::run_history_operation_at_queue_position(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters request, RefPtr<WebContentPage> requesting_page, u64 sequence_number, RefPtr<CanonicalSessionHistoryEntry> target_entry, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    // The traversal queue can outlive an embedded page which appended work to it. Such a page cannot receive
    // completion, so discard its queued operation instead of waiting forever.
    if (requesting_page && !requesting_page->is_open()) {
        if (auto const* parameters = request.get_pointer<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>()) {
            if (auto navigable = find(parameters->navigable_id); navigable.has_value())
                navigable->did_finish_finalizing_navigation(parameters->navigation_id);
        }
        promise->resolve({});
        return;
    }

    // Operation ids are namespaced per initiating process, so a requested id that is already live can only come
    // from a misbehaving process. Drop the request rather than let it alias the existing operation.
    if (m_history_operations.contains(operation_id)) {
        dbgln("Refusing history operation with duplicate id {}", operation_id);
        promise->resolve({});
        return;
    }
    auto owned_operation = make<HistoryOperation>(operation_id, move(request), move(requesting_page), sequence_number, move(target_entry));
    auto* operation = owned_operation.ptr();
    m_history_operations.set(operation_id, move(owned_operation));
    operation->queue_promise = promise;
    run_history_operation(*operation);
}

void CanonicalTraversable::run_browser_history_traversal_at_queue_position(i32 target_step, bool check_for_cancelation, u64 sequence_number, Function<void()> on_ready, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    auto operation_id = Application::the().allocate_ui_process_cross_process_id();
    auto owned_operation = make<HistoryOperation>(operation_id, HistoryOperation::BrowserTraversal { target_step, check_for_cancelation, move(on_ready) }, RefPtr<WebContentPage> {}, sequence_number, RefPtr<CanonicalSessionHistoryEntry> {});
    auto* operation = owned_operation.ptr();
    m_history_operations.set(operation_id, move(owned_operation));
    VERIFY(!m_browser_history_traversal);
    m_browser_history_traversal = operation;
    operation->queue_promise = promise;
    auto view = this->view();
    VERIFY(view.has_value());
    view->will_apply_history_traversal_step(operation_id);
    run_history_operation(*operation);
}

void CanonicalTraversable::append_history_queue_steps(SessionHistoryTraversalSteps steps)
{
    m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#url-and-history-update-steps
// 3. Let newEntry be a new session history entry, with
//      document state: activeEntry's document state
// NB: Navigating to a fragment does the same. The process hosting the navigable's active document reports the entry's
//     other fields.
NonnullRefPtr<CanonicalSessionHistoryEntry> CanonicalTraversable::session_history_entry_for(CanonicalNavigable const& navigable, Web::HTML::SameDocumentNavigationEntry const& same_document_entry) const
{
    auto entry = CanonicalSessionHistoryEntry::create(navigable.active_session_history_entry()->document_state);
    entry->url = same_document_entry.url;
    entry->directive_state_id = same_document_entry.directive_state_id;
    entry->directive_state_value = same_document_entry.directive_state_value;
    entry->classic_history_api_state = same_document_entry.classic_history_api_state;
    entry->navigation_api_state = same_document_entry.navigation_api_state;
    entry->navigation_api_key = same_document_entry.navigation_api_key;
    entry->navigation_api_id = same_document_entry.navigation_api_id;
    entry->scroll_restoration_mode = same_document_entry.scroll_restoration_mode;
    entry->scroll_position_data = same_document_entry.scroll_position_data;
    return entry;
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
// NB: The page conducting a navigation reports historyEntry once attempting to populate its document is over. The
//     completion steps, which append the steps finalizing the navigation, run here.
void CanonicalTraversable::did_attempt_to_populate_the_history_entry_document(WebContentPage& source_page, Web::FinalizeCrossDocumentNavigationHistoryOperationParameters const& parameters)
{
    auto navigable = find(parameters.navigable_id);
    if (!navigable.has_value())
        return;
    auto navigation = navigable->navigation_with_id(parameters.navigation_id);
    if (!navigation || navigation->is_finalizing() || navigation->host() != &source_page)
        return;

    auto is_javascript_url_navigation = navigation->state.has<CanonicalNavigation::EvaluatingJavaScriptURL>();
    RefPtr<NavigationLoader> loader;
    if (auto* populating = navigation->state.get_pointer<CanonicalNavigation::Populating>())
        loader = move(populating->loader);
    navigation->state = CanonicalNavigation::Finalizing { move(loader) };

    // NB: Populating historyEntry's document saves state in the entry, which the page that populated it reports.
    auto history_entry_descriptor = Web::HTML::create_session_history_entry_descriptor(parameters.history_entry, 0);
    if (!is_javascript_url_navigation) {
        if (!navigation->history_entry)
            return;
        if (history_entry_descriptor.document_state.id == navigation->history_entry->document_state->id
            && navigation->history_entry->update_from_descriptor(history_entry_descriptor).is_error()) {
            return;
        }
        navigation->history_entry->document_state->save_the_origin_of_the_populated_document();
        return;
    }

    auto history_entry_or_error = CanonicalSessionHistoryEntry::create_from_descriptor(history_entry_descriptor);
    if (history_entry_or_error.is_error())
        return;
    auto history_entry = history_entry_or_error.release_value();

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate-to-a-javascript:-url
    // NB: The process running a javascript: URL navigation evaluates it, creates newDocument in navigable's active
    //     browsing context, and reports historyEntry. The UI process runs the steps that decide newDocument's origin,
    //     and populates the document it holds for historyEntry's document state.

    // 3. Set the ongoing navigation for targetNavigable to null.
    // NB: The process running the navigation set it before evaluating the URL.
    if (auto ongoing_navigation_id = navigable->ongoing_navigation_id(); ongoing_navigation_id.has_value() && *ongoing_navigation_id == parameters.navigation_id)
        navigable->clear_ongoing_navigation();

    auto const& initiator_origin = history_entry->document_state->initiator_origin;

    // 4. If initiatorOrigin is not same origin-domain with targetNavigable's active document's origin, then return.
    if (!initiator_origin.has_value() || !initiator_origin->is_same_origin_domain(navigable->active_document().origin()))
        return;

    if (!parameters.document_id.has_value())
        return;

    // 8. Assert: initiatorOrigin is newDocument's origin.
    // NB: The UI process creates its newDocument with initiatorOrigin, and with the opener policy evaluating a
    //     javascript: URL gives it: targetNavigable's active document's opener policy.
    auto const& coop = navigable->active_document().opener_policy();
    NavigationLoader::ResponseDocument response_document {
        .is_inline_content = false,
        .coop_enforcement_result = { .url = history_entry->url, .origin = *initiator_origin, .opener_policy = coop },
        .response_url = history_entry->url,
        .request_current_url = {},
        .origin = *initiator_origin,
        .opener_policy = coop,
        .embedder_policy = navigable->active_document().embedder_policy(),
        .final_sandboxing_flag_set = navigable->active_document().active_sandboxing_flag_set(),
        .environment_id = parameters.environment_id,
        .document_id = *parameters.document_id,
    };

    // 11. Let documentState be a new document state with
    //     [...]
    //     initiator origin: initiatorOrigin
    //     origin: initiatorOrigin
    history_entry->document_state->origin = initiator_origin;

    // 12. Let historyEntry be a new session history entry, with
    //     URL: entryToReplace's URL
    //     document state: documentState
    navigable->populate_document_for_javascript_url(*navigation, move(history_entry), navigable->create_and_initialize_a_document(response_document));
}

void CanonicalTraversable::enqueue_history_operation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters request, RefPtr<WebContentPage> requesting_page)
{
    auto sequence_number = next_sequence_number();

    // https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-child-navigable
    // Steps 6-7 remove the nested history before step 9 appends traversal steps. Apply the canonical counterpart while
    // admitting the destruction request, rather than deferring it to the operation's eventual queue position.
    if (request.has<Web::NavigableDestructionHistoryOperationParameters>()) {
        auto const& parameters = request.get<Web::NavigableDestructionHistoryOperationParameters>();
        if (auto parent_navigable = find(parameters.parent_navigable_id); parent_navigable.has_value())
            remove_nested_history(*parent_navigable, parameters.parent_document_state_id, parameters.navigable_id);
    }

    // AD-HOC: The update for a navigable's creation populates the entry its nested history kept, if a document
    //         repopulated for its entry created it, in place of the navigation its container starts meanwhile. That
    //         navigation's start arrives after this request, and is canceled before it fetches anything.
    if (auto const* parameters = request.get_pointer<Web::NavigableCreationHistoryOperationParameters>()) {
        auto navigable = find(parameters->navigable_id);
        if (navigable.has_value() && !navigable->ongoing_navigation_id().has_value()
            && m_session_history.get_the_target_history_entry(*navigable, m_session_history.current_step())) {
            navigable->set_ongoing_navigation_to_traversal(operation_id);
        }
    }

    if (auto const* parameters = request.get_pointer<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>())
        did_attempt_to_populate_the_history_entry_document(*requesting_page, *parameters);

    Optional<Web::HTML::CrossProcessId> synchronous_navigation_target;
    RefPtr<CanonicalSessionHistoryEntry> target_entry;
    if (request.has<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>()) {
        auto const& parameters = request.get<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>();
        synchronous_navigation_target = parameters.navigable_id;

        // https://html.spec.whatwg.org/multipage/browsing-the-web.html#url-and-history-update-steps
        // NB: The process hosting navigable's active document ran these steps, and reports newEntry with the
        //     synchronous navigation steps it appended. Those steps make newEntry navigable's active entry here: they
        //     can jump the queue ahead of the steps creating navigable's nested history, and then find no session
        //     history to hold newEntry.
        if (auto navigable = find(parameters.navigable_id); navigable.has_value()) {
            if (parameters.previous_entry_persisted_state.has_value())
                update_session_history_entry_persisted_state(*navigable, *parameters.previous_entry_persisted_state);
            target_entry = session_history_entry_for(*navigable, parameters.target_entry);
            session_history_changed();
        }
    }

    // A page that aborts an intercepted traverse before its resumption runs revokes the resumption.
    Optional<SessionHistoryTraversalQueue::RevocableOperation> revocable_operation;
    if (request.has<Web::ResumeTraverseHistoryOperationParameters>())
        revocable_operation = SessionHistoryTraversalQueue::RevocableOperation { requesting_page->id(), operation_id };

    auto steps = [this, operation_id, request = move(request), requesting_page = move(requesting_page), sequence_number, target_entry](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_history_operation_at_queue_position(operation_id, move(request), move(requesting_page), sequence_number, move(target_entry), move(promise));
    };

    if (synchronous_navigation_target.has_value())
        m_history_traversal_queue.append_session_history_synchronous_navigation_steps(*synchronous_navigation_target, move(target_entry), move(steps));
    else
        m_history_traversal_queue.append_session_history_traversal_steps(move(steps), revocable_operation);
}

void CanonicalTraversable::revoke_history_operation(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id)
{
    // Steps that have started run to completion.
    if (!m_history_traversal_queue.remove_steps_of_revoked_operation({ source_page.id(), operation_id }))
        return;
    source_page.async_complete_history_operation(operation_id, {}, m_session_history.size());
}

void CanonicalTraversable::enqueue_browser_history_traversal(i32 target_step, bool check_for_cancelation)
{
    auto sequence_number = next_sequence_number();
    auto steps = [this, target_step, check_for_cancelation, sequence_number](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_browser_history_traversal_at_queue_position(target_step, check_for_cancelation, sequence_number, nullptr, move(promise));
    };
    m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::apply_history_step(HistoryOperation& operation, i32 step, bool check_for_cancelation, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement user_involvement, Optional<Web::Bindings::NavigationType> navigation_type, Optional<Web::HTML::NavigationSourceSnapshot> source_snapshot_params)
{
    VERIFY(!operation.algorithm);
    operation.algorithm = make<ApplyHistoryStep>(*this, operation, operation.operation_id, operation.sequence_number, step, check_for_cancelation, initiator_to_check, move(source_snapshot_params), user_involvement, navigation_type);
    operation.algorithm->apply_the_history_step();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-push/replace-history-step
void CanonicalTraversable::apply_the_push_or_replace_history_step(HistoryOperation& operation, i32 step, Web::HTML::HistoryHandlingBehavior history_handling, Web::HTML::UserNavigationInvolvement user_involvement)
{
    auto navigation_type = history_handling == Web::HTML::HistoryHandlingBehavior::Push
        ? Web::Bindings::NavigationType::Push
        : Web::Bindings::NavigationType::Replace;

    // 1. Return the result of applying the history step step to traversable given false, null, null, userInvolvement, and historyHandling.
    apply_history_step(operation, step, false, {}, user_involvement, navigation_type);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-reload-history-step
void CanonicalTraversable::apply_the_reload_history_step(HistoryOperation& operation, Web::HTML::UserNavigationInvolvement user_involvement)
{
    // 1. Let step be traversable's current session history step.
    auto step = m_session_history.current_step();

    // 2. Return the result of applying the history step step to traversable given true, null, null, userInvolvement, and "reload".
    apply_history_step(operation, step, true, {}, user_involvement, Web::Bindings::NavigationType::Reload);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-traverse-history-step
void CanonicalTraversable::apply_the_traverse_history_step(HistoryOperation& operation, i32 step, Optional<Web::HTML::NavigationSourceSnapshot> source_snapshot_params, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement user_involvement)
{
    // 1. Return the result of applying the history step step to traversable given true, sourceSnapshotParams, initiatorToCheck, userInvolvement, and "traverse".
    apply_history_step(operation, step, true, initiator_to_check, user_involvement, Web::Bindings::NavigationType::Traverse, move(source_snapshot_params));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#resume-applying-the-traverse-history-step
void CanonicalTraversable::resume_applying_the_traverse_history_step(HistoryOperation& operation, i32 step, Web::HTML::UserNavigationInvolvement user_involvement)
{
    // Apply step to traversable given false, null, null, userInvolvement, and "traverse".
    apply_history_step(operation, step, false, {}, user_involvement, Web::Bindings::NavigationType::Traverse);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#update-for-navigable-creation/destruction
void CanonicalTraversable::update_for_navigable_creation_or_destruction(HistoryOperation& operation)
{
    // 1. Let step be traversable's current session history step.
    auto step = m_session_history.current_step();

    // 2. Return the result of applying the history step step to traversable given false, null, null, "none", and null.
    apply_history_step(operation, step, false, {}, Web::HTML::UserNavigationInvolvement::None, {});
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta
void CanonicalTraversable::traverse_the_history_by_a_delta_at_queue_position(HistoryOperation& operation, Web::TraverseByDeltaHistoryOperationParameters const& request)
{
    // Steps 1-3 of traverse the history by a delta were performed by the source process before it appended these
    // session history traversal steps.

    // 1. Let allSteps be the result of getting all used history steps for traversable.
    auto all_steps = m_session_history.used_steps();

    // 2. Let currentStepIndex be the index of traversable's current session history step within allSteps.
    // AD-HOC: The nested history of a destroyed navigable is removed when its destruction is admitted, but the current
    //         step is only moved off the steps that removal left unused once the update for the destruction reaches its
    //         position on the queue. A traversal that was queued ahead of the update goes from the used step instead.
    auto current_step_index = m_session_history.current_used_step_index();

    // 3. Let targetStepIndex be currentStepIndex plus delta.
    auto target_step_index = static_cast<i64>(current_step_index) + static_cast<i64>(request.delta);

    // 4. If allSteps[targetStepIndex] does not exist, then abort these steps.
    if (target_step_index < 0 || static_cast<size_t>(target_step_index) >= all_steps.size()) {
        finish_history_operation(operation.operation_id);
        return;
    }

    // 5. Apply the traverse history step allSteps[targetStepIndex] to traversable, given sourceSnapshotParams,
    //    initiatorToCheck, and userInvolvement.
    apply_the_traverse_history_step(operation, all_steps[static_cast<size_t>(target_step_index)], request.source_snapshot_params, request.initiator_to_check, request.user_involvement);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#performing-a-navigation-api-traversal
void CanonicalTraversable::perform_a_navigation_api_traversal_at_queue_position(HistoryOperation& operation, Web::NavigationAPITraverseHistoryOperationParameters const& request)
{
    // The substeps of steps 12.2 and 12.3:
    // 1. Queue a global task on the navigation and traversal task source given navigation's relevant global object to
    //    reject the finished promise for apiMethodTracker with an "InvalidStateError" DOMException.
    // 2. Abort these steps.
    auto reject_and_abort = [&] {
        operation.requesting_page->async_reject_navigation_api_traversal(operation.operation_id);
        finish_history_operation(operation.operation_id);
    };

    // 12.1. Let navigableSHEs be the result of getting session history entries given navigable.
    // 12.2. Let targetSHE be the session history entry in navigableSHEs whose navigation API key is key. If no such
    //       entry exists, then:
    // NB: The requesting process can still have a navigable this session history no longer has.
    auto navigable = find(request.navigable_id);
    RefPtr<CanonicalSessionHistoryEntry> target_entry;
    if (navigable.has_value()) {
        if (auto navigable_session_history_entries = m_session_history.get_session_history_entries(*navigable); navigable_session_history_entries.has_value()) {
            auto entry = navigable_session_history_entries->find_if([&](auto const& entry) {
                return entry->navigation_api_key == request.key;
            });
            if (entry != navigable_session_history_entries->end())
                target_entry = *entry;
        }
    }
    if (!target_entry) {
        reject_and_abort();
        return;
    }

    // 12.3. If targetSHE is navigable's active session history entry, then:
    if (auto const& active_entry = navigable->active_session_history_entry(); active_entry && active_entry->identity() == target_entry->identity()) {
        reject_and_abort();
        return;
    }

    // 12.4. Let result be the result of applying the traverse history step targetSHE's step to traversable, given
    //       sourceSnapshotParams, navigable, and "none".
    apply_the_traverse_history_step(operation, target_entry->step, request.source_snapshot_params, request.navigable_id, Web::HTML::UserNavigationInvolvement::None);

    // Steps 12.5-12.6 are handled in Navigation's relevant realm when the requesting process learns the result.
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-same-document-navigation
void CanonicalTraversable::finalize_a_same_document_navigation(HistoryOperation& operation, Web::FinalizeSameDocumentNavigationHistoryOperationParameters const& request)
{
    VERIFY(request.entry_to_replace.has_value() == (request.history_handling == Web::HTML::HistoryHandlingBehavior::Replace));
    auto target_navigable = find(request.navigable_id);
    if (!target_navigable.has_value()) {
        finish_history_operation(operation.operation_id);
        return;
    }

    // 1. Assert: this is running on traversable's session history traversal queue.
    VERIFY(operation.queue_promise);
    VERIFY(!operation.queue_promise->is_resolved() && !operation.queue_promise->is_rejected());
    VERIFY(&target_navigable->top_level_traversable() == this);

    auto target_entry = operation.target_entry;

    // 2. If targetNavigable's active session history entry is not targetEntry, then return.
    // AD-HOC: WebContent performs this object-identity check synchronously before enqueueing. Repeating
    // it against the later canonical active entry would incorrectly discard back-to-back synchronous pushState()
    // calls; Firefox and Chromium preserve both entries.
    //
    // AD-HOC: The document that made the navigation can still have been replaced by the time this queue position
    //         is reached: a cross-document navigation that committed ahead of it unloads that document, and that
    //         document can push entries until it is destroyed (e.g. from pagehide, or from a timer that fires
    //         during the commit). Such a stale finalization must not be applied. Its target entry belongs to a
    //         document that is no longer active, so applying it would traverse across documents and populate the
    //         unloaded document again, replacing the one that just committed.
    if (!target_entry || target_entry->document_state != target_navigable->active_session_history_entry()->document_state) {
        finish_history_operation(operation.operation_id);
        return;
    }

    // 3. Let targetStep be null.
    Optional<i32> target_step;

    // 4. Let targetEntries be the result of getting session history entries for targetNavigable.
    auto target_entries = m_session_history.get_session_history_entries(*target_navigable);
    if (!target_entries.has_value()) {
        finish_history_operation(operation.operation_id);
        return;
    }

    // AD-HOC: The active entry of a navigable whose history is being reconstructed, which is that of its initial
    //         about:blank, is not among its session history entries.
    if (!any_of(*target_entries, [&](auto const& entry) { return entry->document_state == target_entry->document_state; })) {
        finish_history_operation(operation.operation_id);
        return;
    }

    auto current_step = m_session_history.current_step();

    // 5. If entryToReplace is null:
    if (!request.entry_to_replace.has_value()) {
        // 5.1. Clear the forward session history of traversable.
        // 5.2. Set targetStep to traversable's current session history step + 1.
        // 5.3. Set targetEntry's step to targetStep.
        // 5.4. Append targetEntry to targetEntries.
        target_step = m_session_history.push_session_history_entry(*target_navigable, *target_entry);
    }
    // Otherwise:
    else {
        auto entry_to_replace = target_entries->find_if([&](auto const& entry) {
            return entry->identity() == *request.entry_to_replace;
        });
        if (entry_to_replace == target_entries->end()) {
            finish_history_operation(operation.operation_id);
            return;
        }

        auto replaces_current_entry = target_navigable->current_session_history_entry_is(**entry_to_replace);

        // 5.1. Replace entryToReplace with targetEntry in targetEntries.
        // 5.2. Set targetEntry's step to entryToReplace's step.
        auto replaced = m_session_history.replace_session_history_entry(*target_navigable, **entry_to_replace, *target_entry);
        VERIFY(replaced);

        // AD-HOC: A traversal that runs while this replacement waits can move targetNavigable away from
        //         entryToReplace. Applying the history step would then move targetNavigable to targetEntry, so
        //         the replacement happens in place only.
        if (!replaces_current_entry) {
            m_session_history.update_directive_state_value(target_entry->directive_state_id, target_entry->directive_state_value);
            finish_history_operation(operation.operation_id);
            return;
        }

        // 5.3. Set targetStep to traversable's current session history step.
        target_step = current_step;
    }

    // Canonical entries carry a stable ID and a copied value for a directive state shared across
    // history entries. Keep those copies synchronized when the shared state is mutated.
    m_session_history.update_directive_state_value(target_entry->directive_state_id, target_entry->directive_state_value);

    target_navigable->set_active_session_history_entry(target_entry);
    target_navigable->send_replicated_state();

    // 6. Apply the push/replace history step targetStep to traversable given historyHandling and userInvolvement.
    apply_the_push_or_replace_history_step(operation, *target_step, request.history_handling, request.user_involvement);
}

void CanonicalTraversable::run_history_operation(HistoryOperation& operation)
{
    if (operation.requesting_page)
        add_history_operation_completion_endpoint(operation, *operation.requesting_page);

    operation.parameters.visit(
        [&](Web::ReloadHistoryOperationParameters const& parameters) {
            apply_the_reload_history_step(operation, parameters.user_involvement);
        },
        [&](Web::TraverseByDeltaHistoryOperationParameters const& request) {
            traverse_the_history_by_a_delta_at_queue_position(operation, request);
        },
        [&](HistoryOperation::BrowserTraversal const& traversal) {
            if (!page_hosting(*this)) {
                finish_history_operation(operation.operation_id);
                return;
            }
            apply_history_step(operation, traversal.target_step, traversal.check_for_cancelation, {}, Web::HTML::UserNavigationInvolvement::BrowserUI, Web::Bindings::NavigationType::Traverse);
        },
        [&](Web::NavigationAPITraverseHistoryOperationParameters const& request) {
            perform_a_navigation_api_traversal_at_queue_position(operation, request);
        },
        [&](Web::FinalizeSameDocumentNavigationHistoryOperationParameters const& request) {
            finalize_a_same_document_navigation(operation, request);
        },
        [&](Web::NavigableCreationHistoryOperationParameters const& parameters) {
            auto parent_navigable = find(parameters.parent_navigable_id);
            auto child_navigable = find(parameters.navigable_id);
            if (!parent_navigable.has_value() || !child_navigable.has_value()) {
                finish_history_operation(operation.operation_id);
                return;
            }

            // A navigable created by a document repopulated for its entry already has the nested history that entry
            // kept. Updating for its creation populates the entry the history holds for it at the current step.
            if (m_session_history.get_the_target_history_entry(*child_navigable, m_session_history.current_step())) {
                update_for_navigable_creation_or_destruction(operation);
                return;
            }

            // https://html.spec.whatwg.org/multipage/document-sequences.html#create-a-new-child-navigable
            // 1. Let parentDocState be parentNavigable's active session history entry's document state.
            auto& parent_document_state = *parent_navigable->active_session_history_entry()->document_state;

            // 2. Let parentNavigableEntries be the result of getting session history entries for parentNavigable.
            // 3. Let targetStepSHE be the first session history entry in parentNavigableEntries whose document state equals parentDocState.
            // 4. Set historyEntry's step to targetStepSHE's step.
            // 5. Let nestedHistory be a new nested history whose id is navigable's id and entries list is « historyEntry ».
            // 6. Append nestedHistory to parentDocState's nested histories.
            if (!append_nested_history(*parent_navigable, parent_document_state, parameters.navigable_id).has_value()) {
                finish_history_operation(operation.operation_id);
                return;
            }

            // 7. Update for navigable creation/destruction given traversable.
            update_for_navigable_creation_or_destruction(operation);
        },
        [&](Web::NavigableDestructionHistoryOperationParameters const&) {
            update_for_navigable_creation_or_destruction(operation);
        },
        [&](Web::CloseTopLevelTraversableHistoryOperationParameters const&) {
            // https://html.spec.whatwg.org/multipage/document-sequences.html#definitely-close-a-top-level-traversable
            // Step 3's session history traversal steps run at their queue position:
            // 1. Let afterAllUnloads be an algorithm step which destroys traversable.
            // 2. Unload a document and its descendants given traversable's active document, null, and
            //    afterAllUnloads.
            // NB: The final unload-and-destroy task is dispatched to the page hosting the traversable once every
            //     descendant subtree has unloaded. That need not be the requesting page: a cross-site navigation
            //     ahead of the close in the queue can have moved the traversable to another process.
            unload_a_document_and_its_descendants(*this, page_hosting(*this), Web::HTML::ChildNavigableDestruction::No, [this, operation_id = operation.operation_id] {
                if (!find_history_operation(operation_id))
                    return;
                if (auto host = page_hosting(*this))
                    host->async_run_traversable_close_unload_task();
                finish_history_operation(operation_id);
            });
        },
        [&](Web::FlushSessionHistoryTraversalQueueOperationParameters const&) {
            // Flush is a queue barrier; completing at the queue position is the whole operation.
            finish_history_operation(operation.operation_id);
        },
        [&](Web::ResumeTraverseHistoryOperationParameters const& parameters) {
            resume_applying_the_traverse_history_step(operation, parameters.target_step, parameters.user_involvement);
        },
        [&](Web::FinalizeCrossDocumentNavigationHistoryOperationParameters const&) {
            finalize_a_cross_document_navigation(operation);
        });
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-cross-document-navigation
void CanonicalTraversable::finalize_a_cross_document_navigation(HistoryOperation& operation)
{
    auto const& parameters = operation.parameters.get<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>();

    // NB: A navigation that ended before its completion steps appended this, or that another page hosts, has nothing to
    //     finalize. historyEntry is the navigation's. A javascript: URL navigation whose document was refused has none.
    auto navigation = finalized_navigation(operation);
    if (!navigation || navigation->host() != operation.requesting_page.ptr() || !navigation->history_entry) {
        finish_history_operation(operation.operation_id);
        return;
    }
    auto navigable = find(parameters.navigable_id);
    VERIFY(navigable.has_value());
    NonnullRefPtr history_entry = *navigation->history_entry;

    // 1. Assert: this is running on navigable's traversable navigable's session history traversal queue.
    VERIFY(operation.queue_promise);
    VERIFY(!operation.queue_promise->is_resolved() && !operation.queue_promise->is_rejected());
    VERIFY(&navigable->top_level_traversable() == this);

    // 2. Set navigable's is delaying load events to false.
    // NB: The page that populated historyEntry's document performs this step: when it reports that there is no
    //     document, or as it activates the document.

    // 3. If historyEntry's document is null, then return.
    // NB: The document populated for historyEntry waits in its document state until historyEntry is activated.
    RefPtr<CanonicalDocument> document;
    if (auto const& populated_document = history_entry->document_state->populated_document; populated_document.has_value())
        document = populated_document->document;
    if (!document) {
        navigable->did_cancel_navigation(parameters.navigation_id);
        finish_history_operation(operation.operation_id);
        return;
    }

    // 4. If all of the following are true:
    //    - navigable's parent is null;
    //    - historyEntry's document's browsing context is not an auxiliary browsing context whose opener browsing
    //      context is non-null; and
    //    - historyEntry's document's origin is not navigable's active document's origin,
    //    then set historyEntry's document state's navigable target name to the empty string.
    // AD-HOC: Don't clear the name when leaving an initial about:blank in the same browsing context. A noopener popup's
    //         initial about:blank has an opaque origin, so its first navigation would always lose the name it was
    //         opened with.
    auto& browsing_context = document->browsing_context();
    if (navigable->parent() == nullptr
        && !(browsing_context.is_auxiliary() && browsing_context.opener_browsing_context())
        && document->origin() != navigable->active_document().origin()
        && (!navigable->active_document().is_initial_about_blank() || &browsing_context != &navigable->active_browsing_context())) {
        history_entry->document_state->navigable_target_name = {};
    }

    // 5. Let entryToReplace be navigable's active session history entry if historyHandling is "replace", otherwise null.
    RefPtr<CanonicalSessionHistoryEntry> entry_to_replace;
    if (parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Replace)
        entry_to_replace = navigable->active_session_history_entry();
    if (parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Replace && !entry_to_replace) {
        finish_history_operation(operation.operation_id);
        return;
    }

    // 6. Let traversable be navigable's traversable navigable.
    // NB: This CanonicalTraversable is navigable's traversable navigable.

    // 7. Let targetStep be null.
    Optional<i32> target_step;

    // 8. Let targetEntries be the result of getting session history entries for navigable.
    auto target_entries = m_session_history.get_session_history_entries(*navigable);
    if (!target_entries.has_value()) {
        finish_history_operation(operation.operation_id);
        return;
    }

    // 9. If entryToReplace is null:
    if (!entry_to_replace) {
        // 1. Clear the forward session history of traversable.
        // 2. Set targetStep to traversable's current session history step + 1.
        // 3. Set historyEntry's step to targetStep.
        // 4. Append historyEntry to targetEntries.
        target_step = m_session_history.push_session_history_entry(*navigable, move(history_entry));
    }
    // Otherwise:
    else {
        // 1. Replace entryToReplace with historyEntry in targetEntries.
        // 2. Set historyEntry's step to entryToReplace's step.
        if (!m_session_history.replace_session_history_entry(*navigable, *entry_to_replace, history_entry)) {
            finish_history_operation(operation.operation_id);
            return;
        }

        // 3. If historyEntry's document state's origin is same origin with entryToReplace's document state's origin,
        //    then set historyEntry's navigation API key to entryToReplace's navigation API key.
        if (history_entry->document_state->origin.has_value()
            && entry_to_replace->document_state->origin.has_value()
            && history_entry->document_state->origin->is_same_origin(*entry_to_replace->document_state->origin)) {
            history_entry->navigation_api_key = entry_to_replace->navigation_api_key;
        }

        // 4. Set targetStep to traversable's current session history step.
        target_step = m_session_history.current_step();
    }

    // 10. Apply the push/replace history step targetStep to traversable given historyHandling and userInvolvement.
    apply_the_push_or_replace_history_step(operation, *target_step, parameters.history_handling, parameters.user_involvement);
}

void CanonicalTraversable::did_not_apply_reload(Web::HTML::CrossProcessId navigable_id)
{
    // AD-HOC: The spec leaves reload pending set when the reload does not apply, so a later history step would reload
    //         the document anyway.
    if (auto navigable = find(navigable_id); navigable.has_value()) {
        if (auto const& entry = navigable->active_session_history_entry())
            entry->document_state->reload_pending = false;
    }

    if (navigable_id != id())
        return;
    if (auto view = this->view(); view.has_value())
        view->did_not_apply_reload();
}

// The operation is over: its apply the history step returned result, or its steps ended without applying one.
void CanonicalTraversable::finish_history_operation(Web::HTML::CrossProcessId operation_id, Optional<Web::HTML::HistoryStepResult> result)
{
    auto taken_operation = m_history_operations.take(operation_id);
    if (!taken_operation.has_value())
        return;
    auto operation = taken_operation.release_value();

    if (m_browser_history_traversal == operation.ptr())
        m_browser_history_traversal = nullptr;
    if (operation->beforeunload_check_id.has_value())
        m_pending_beforeunload_checks.remove(*operation->beforeunload_check_id);

    // A changing job still pending when its operation finishes never activates the document it populated.
    if (operation->algorithm)
        operation->algorithm->abandon();
    if (auto const* parameters = operation->parameters.get_pointer<Web::NavigableCreationHistoryOperationParameters>()) {
        if (auto navigable = find(parameters->navigable_id); navigable.has_value())
            navigable->clear_ongoing_navigation_traversal(operation_id);
    }
    if (auto const* parameters = operation->parameters.get_pointer<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>()) {
        if (auto navigable = find(parameters->navigable_id); navigable.has_value())
            navigable->did_finish_finalizing_navigation(parameters->navigation_id);
    }

    // The requesting page learns the result before the operation completes.
    if (result.has_value()) {
        if (operation->requesting_page)
            operation->requesting_page->async_history_step_applied(operation_id, *result);
        if (auto const* reload = operation->parameters.get_pointer<Web::ReloadHistoryOperationParameters>(); reload && result != Web::HTML::HistoryStepResult::Applied)
            did_not_apply_reload(reload->navigable_id);
    }

    auto committed_step = operation->algorithm ? operation->algorithm->committed_step() : Optional<i32> {};
    if (committed_step.has_value()) {
        if (auto view = this->view(); view.has_value())
            view->did_commit_history_step();
    }

    // The step of the entry the requesting page's operation targets: the committed step, but for a created child's
    // initial entry, whose step the steps creating it set.
    auto target_entry_step = committed_step;
    if (auto const* parameters = operation->parameters.get_pointer<Web::NavigableCreationHistoryOperationParameters>(); parameters && committed_step.has_value()) {
        if (auto child_navigable = find(parameters->navigable_id); child_navigable.has_value())
            target_entry_step = child_navigable->active_session_history_entry()->step;
    }

    for (auto& endpoint : operation->completion_endpoints)
        endpoint->async_complete_history_operation(operation_id, target_entry_step, m_session_history.size());

    // All apply-driven mutations have settled at operation completion.
    session_history_changed();

    // What waited for the operation's traversal to be over runs now.
    if (operation->is_browser_traversal()) {
        if (auto callback = move(operation->browser_traversal().on_ready))
            callback();
        if (auto view = this->view(); view.has_value())
            view->did_finish_history_traversal(operation_id);
    }

    // NB: Resolving the queue promise can synchronously start the next queued operation.
    if (operation->queue_promise)
        operation->queue_promise->resolve({});

    if (auto view = this->view(); view.has_value())
        view->run_webdriver_commands_waiting_for_a_document({});

    // NB: The run of apply the history step that finished the operation can be on the stack; destroy the operation
    //     only once the stack has unwound.
    Core::deferred_invoke([operation = move(operation)] { });
}

void CanonicalTraversable::abandon_history_operations()
{
    while (!m_history_operations.is_empty())
        finish_history_operation(m_history_operations.begin()->key);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#checking-if-unloading-is-canceled
// NB: The steps run in the pages hosting the documents, one page at a time. A skipped endpoint has run its part already.
Optional<Web::HTML::CrossProcessId> CanonicalTraversable::check_if_unloading_is_canceled(Vector<Web::HTML::CrossProcessId> navigables_that_need_before_unload, RefPtr<CanonicalSessionHistoryEntry> target_entry, Optional<Web::HTML::UserNavigationInvolvement> user_involvement_for_navigate_event, RefPtr<WebContentPage> skipped_endpoint, Web::HTML::UnloadPromptShown unload_prompt_shown, Function<void(Web::HTML::CheckIfUnloadingIsCanceledResult, Web::HTML::UnloadPromptShown)> on_complete)
{
    PendingBeforeunloadCheck check;
    check.target_entry = target_entry;
    check.user_involvement_for_navigate_event = user_involvement_for_navigate_event;

    // 1. Let documentsToFireBeforeunload be the active document of each item in navigablesThatNeedBeforeUnload.
    for (auto navigable_id : navigables_that_need_before_unload) {
        auto navigable = find(navigable_id);
        if (!navigable.has_value())
            continue;
        auto endpoint = page_hosting(*navigable);
        if (!endpoint || endpoint == skipped_endpoint)
            continue;
        auto group = check.groups.find_if([&](auto const& group) {
            return group.endpoint == endpoint;
        });
        if (group == check.groups.end())
            check.groups.append({ endpoint.release_nonnull(), { navigable_id } });
        else
            group->navigable_ids.append(navigable_id);
    }

    // 2. Let unloadPromptShown be false.
    // NB: A check continuing a navigation's own starts from whether that one showed a prompt.
    check.unload_prompt_shown = unload_prompt_shown;

    // 4. If traversable was given:
    // NB: The traversable's host runs this step, with beforeunload for the documents it hosts, before any other page
    //     fires beforeunload.
    if (auto traversable_host = target_entry ? page_hosting(*this) : nullptr) {
        auto group = check.groups.find_if([&](auto const& group) {
            return group.endpoint == traversable_host;
        });
        BeforeunloadGroup traversable_group { traversable_host.release_nonnull(), {}, true };
        if (group != check.groups.end()) {
            traversable_group.navigable_ids = move(group->navigable_ids);
            check.groups.remove(group.index());
        }
        check.groups.prepend(move(traversable_group));
    }

    if (check.groups.is_empty()) {
        on_complete(Web::HTML::CheckIfUnloadingIsCanceledResult::Continue, unload_prompt_shown);
        return {};
    }

    check.on_complete = move(on_complete);
    auto check_id = Application::the().allocate_ui_process_cross_process_id();
    m_pending_beforeunload_checks.set(check_id, move(check));
    dispatch_next_beforeunload_group(check_id);
    if (!m_pending_beforeunload_checks.contains(check_id))
        return {};
    return check_id;
}

void CanonicalTraversable::dispatch_next_beforeunload_group(Web::HTML::CrossProcessId check_id)
{
    auto check = m_pending_beforeunload_checks.find(check_id);
    if (check == m_pending_beforeunload_checks.end())
        return;

    // 7. For each document of documentsToFireBeforeunload, queue a global task on the navigation and traversal task
    //    source given document's relevant global object to run the steps:
    // NB: Each page runs these for the documents it hosts, and the next page is asked once it has replied.
    while (!check->value.groups.is_empty()) {
        auto group = check->value.groups.take_first();
        // A missing endpoint's documents are already gone. They contribute "proceed".
        if (!group.endpoint->is_open())
            continue;
        group.endpoint->owe_reply({ OwedReply::Kind::BeforeunloadCheck, check_id, {} });
        Optional<Web::HTML::SessionHistoryEntryDescriptor> target_entry;
        if (group.is_traversable_host)
            target_entry = check->value.target_entry->descriptor();
        group.endpoint->async_run_beforeunload_check(check_id, move(group.navigable_ids), move(target_entry), check->value.user_involvement_for_navigate_event, check->value.unload_prompt_shown);
        return;
    }

    // 8. Wait for completedTasks to be totalTasks.
    // 9. Return finalStatus.
    auto completed_check = m_pending_beforeunload_checks.take(check_id);
    completed_check->on_complete(Web::HTML::CheckIfUnloadingIsCanceledResult::Continue, completed_check->unload_prompt_shown);
}

void CanonicalTraversable::did_receive_beforeunload_check_result(WebContentPage& source_page, Web::HTML::CrossProcessId check_id, Web::HTML::CheckIfUnloadingIsCanceledResult result, Web::HTML::UnloadPromptShown unload_prompt_shown)
{
    if (!source_page.take_owed_reply({ OwedReply::Kind::BeforeunloadCheck, check_id, {} }))
        return;
    auto check = m_pending_beforeunload_checks.find(check_id);
    if (check == m_pending_beforeunload_checks.end())
        return;
    if (result != Web::HTML::CheckIfUnloadingIsCanceledResult::Continue) {
        auto completed_check = m_pending_beforeunload_checks.take(check_id);
        completed_check->on_complete(result, unload_prompt_shown);
        return;
    }
    check->value.unload_prompt_shown = unload_prompt_shown;
    dispatch_next_beforeunload_group(check_id);
}

void CanonicalTraversable::did_receive_changing_navigable_history_job_ready(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition, bool update_only, Optional<Web::HTML::CrossProcessId> target_document_id)
{
    if (!source_page.take_owed_reply({ OwedReply::Kind::ChangingJob, operation_id, navigable_id }))
        return;
    auto* operation = find_history_operation(operation_id);
    auto* changing_navigable = operation ? operation->pending_changing_job(navigable_id) : nullptr;
    if (!changing_navigable)
        return;

    if (disposition == Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready) {
        changing_navigable->update_only = update_only;
        changing_navigable->target_document_id = target_document_id;
    }
    operation->algorithm->changing_navigable_job_completed(*changing_navigable, disposition);
}

void CanonicalTraversable::did_receive_changing_navigable_continuation_applied(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::HostedNavigableState> activated_navigable_state, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state)
{
    if (!source_page.take_owed_reply({ OwedReply::Kind::ContinuationApplied, operation_id, navigable_id }))
        return;
    auto* operation = find_history_operation(operation_id);
    if (auto* changing_navigable = operation ? operation->pending_changing_job(navigable_id) : nullptr) {
        auto& navigable = changing_navigable->navigable();
        auto document = move(changing_navigable->populated_document);
        // The job ends first, so that activating its document does not end it as one for the document it replaces.
        operation->algorithm->end_changing_navigable_job(*changing_navigable);
        if (activated_navigable_state.has_value())
            did_activate_history_entry(*operation, *changing_navigable, source_page, activated_navigable_state.release_value());
        else if (document)
            navigable.abandon_populated_document(changing_navigable->target_entry->document_state, *document);
        if (previous_entry_persisted_state.has_value())
            update_session_history_entry_persisted_state(navigable, *previous_entry_persisted_state);
        // A replacement document's creation operation also applies a top-level continuation, but it runs before
        // the document has accepted the UI-owned history state. Only the browser traversal itself reaches the
        // observable top-level completion point here.
        if (navigable_id == id() && operation->is_browser_traversal()) {
            if (auto view = this->view(); view.has_value())
                view->did_apply_top_level_history_traversal_step(operation_id);
        }
        operation->algorithm->process_changing_navigable_continuations();
    }

    if (auto view = this->view(); view.has_value())
        view->run_webdriver_commands_waiting_for_a_document({});
}

void CanonicalTraversable::did_receive_nonchanging_navigable_history_state_updated(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id)
{
    if (!source_page.take_owed_reply({ OwedReply::Kind::NonchangingUpdate, operation_id, navigable_id }))
        return;
    if (auto* operation = find_history_operation(operation_id); operation && operation->algorithm)
        operation->algorithm->nonchanging_navigable_updated();
}

}
