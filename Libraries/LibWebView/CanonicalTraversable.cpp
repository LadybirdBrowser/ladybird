/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <AK/StringBuilder.h>
#include <LibCore/EventLoop.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/SiteIsolation.h>
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

CanonicalTraversable::CanonicalTraversable(Web::HTML::CrossProcessId id)
    : CanonicalNavigable(id)
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

    // When a user agent determines that the system visibility state for
    // traversable navigable traversable has changed to newState, it must run the following steps:

    // 1. Let navigables be the inclusive descendant navigables of traversable's active document.
    // 2. For each navigable of navigables:
    for_each_in_inclusive_subtree([&](CanonicalNavigable& navigable) {
        // 1. Let document be navigable's active document.
        // 2. Queue a global task on the user interaction task source given document's relevant global object
        //    to update the visibility state of document with newState.
        // NB: The page hosting document runs the task. A document activated later hears the state then, with the
        //     continuation that activates it.
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
Compositing::DevicePixelPoint CanonicalTraversable::focused_navigable_host_offset() const
{
    if (!m_focused_navigable_id.has_value())
        return {};
    auto navigable = find(*m_focused_navigable_id);
    if (!navigable.has_value())
        return {};
    return local_root_offset(*navigable);
}

// The origin, in the view's viewport, of the viewport of the local root holding the navigable in its page.
Compositing::DevicePixelPoint CanonicalTraversable::local_root_offset(CanonicalNavigable const& navigable) const
{
    Compositing::DevicePixelPoint offset;
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

void CanonicalTraversable::rehost(CanonicalNavigable& navigable, NonnullRefPtr<WebContentPage> reporting_page, CanonicalDocument& container_document, Web::HTML::HostedNavigableState hosted_state)
{
    // The displayed document's child navigables go with it.
    while (!navigable.children().is_empty())
        remove(*navigable.children().last());
    navigable.clear_ongoing_navigation();
    navigable.discard_pending_host();

    // The page hosting the displayed document elsewhere retires the navigable's node: the page hosting its container
    // displays a document standing in for it from now on.
    RefPtr<WebContentPage> host = navigable.has_remote_host() ? &navigable.remote_host() : nullptr;
    if (host && host->is_open()) {
        VERIFY(navigable.replicated_state().has_value());
        host->async_stop_hosting_navigable(navigable.id(), *navigable.replicated_state());
    }
    navigable.active_document().set_host(reporting_page);
    navigable.set_container_document({}, container_document);
    navigable.update_hosted_state(move(hosted_state));
    if (host)
        release_page_if_unused(host.release_nonnull());
}

// INTEROP: Reloading rebuilds child frames from the new document instead of restoring their previous entries.
void CanonicalTraversable::adopt_nested_history_for_created_child(CanonicalNavigable const& parent, CanonicalDocument const& container_document, Web::HTML::CrossProcessId child_navigable_id)
{
    if (container_document.is_completely_loaded())
        return;
    RefPtr<CanonicalDocumentState> document_state;
    parent.for_each_populated_document([&](PopulatedDocument const& populated_document) {
        if (populated_document.document == &container_document)
            document_state = populated_document.document_state;
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
    for (auto const& page : m_opener_pages)
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

// The other tabs holding the navigables the opener browsing contexts of this tab's browsing contexts are active in.
void CanonicalTraversable::for_each_opener_traversable(Function<void(CanonicalTraversable&)> const& callback) const
{
    for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        auto const& state = navigable.replicated_state();
        if (!state.has_value() || !state->opener_navigable_id.has_value())
            return IterationDecision::Continue;
        if (auto* opener_traversable = traversable_containing(*state->opener_navigable_id); opener_traversable && opener_traversable != this)
            callback(*opener_traversable);
        return IterationDecision::Continue;
    });
}

// A process holding part of this tab holds the tabs of its openers too, in pages hosting none of them, so that a
// document here reaches an opener's WindowProxy.
void CanonicalTraversable::represent_openers_in(WebContentClient& client)
{
    for_each_opener_traversable([&](CanonicalTraversable& opener_traversable) {
        if (client.page_id_for_traversable(opener_traversable).has_value())
            return;
        auto page_id = Application::the().allocate_page_id();
        client.async_create_representing_page(page_id, opener_traversable.remote_navigable_graph());
        client.register_embedded_page(page_id, opener_traversable);
        opener_traversable.m_opener_pages.append(*client.page(page_id));
        opener_traversable.represent_openers_in(client);
    });
}

void CanonicalTraversable::forget_opener_page(WebContentPage& page)
{
    m_opener_pages.remove_all_matching([&](auto const& opener_page) { return opener_page.ptr() == &page; });
}

void CanonicalTraversable::discard_opener_pages()
{
    for (auto& page : exchange(m_opener_pages, {})) {
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
    if (page_hosts_any(page))
        return;
    if (is_opener_page(page)) {
        if (page->client().holds_part_of_a_tab_opened_by(*this))
            return;
        forget_opener_page(page);
    }
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
    VERIFY(!active_document().host());
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

    auto current_step = m_session_history.current_step();
    VERIFY(current_step.has_value());
    auto const* current_entry = m_session_history.get_the_target_history_entry(*this, *current_step);
    VERIFY(current_entry);
    auto current_entry_descriptor = current_entry->descriptor();

    Compositing::PageId page_id;
    if (host && host->page_id_for_traversable(*this).has_value()) {
        page_id = *host->page_id_for_traversable(*this);
        host->async_begin_hosting_navigable(page_id, id(), current_entry_descriptor, Web::HTML::VisibilityState::Hidden);
    } else if (host) {
        page_id = Application::the().allocate_page_id();
        host->async_create_embedded_page(page_id, remote_navigable_graph(), id(), current_entry_descriptor, Web::HTML::VisibilityState::Hidden);
        host->register_embedded_page(page_id, *this);
        represent_openers_in(*host);
    } else {
        auto process = TRY(Application::the().launch_child_frame_web_content_process(view->is_private(), remote_navigable_graph(), id(), current_entry_descriptor));
        host = move(process.client);
        page_id = process.page_id;
        host->register_embedded_page(page_id, *this);
        represent_openers_in(*host);
    }
    auto& page = *host->page(page_id);
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
    forget_opener_page(page);

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

    if (active_document().host() == &page)
        active_document().set_host(nullptr);
}

void CanonicalTraversable::stand_in_for_lost_document(CanonicalNavigable& navigable)
{
    navigable.hand_pending_webdriver_commands_to(*navigable.reporting_page());
    navigable.active_document().set_host(nullptr);
    auto current_step = m_session_history.current_step();
    if (!current_step.has_value())
        return;
    // NB: The canonical session history can still lack the nested history of a newly created navigable.
    auto const* current_entry = m_session_history.get_the_target_history_entry(navigable, *current_step);
    if (!current_entry)
        return;
    navigable.reporting_page()->async_host_navigable(navigable.id(), current_entry->descriptor(), system_visibility_state());
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
void CanonicalTraversable::reload(OnHistoryOperationComplete on_complete)
{
    auto user_involvement = Web::HTML::UserNavigationInvolvement::BrowserUI;

    // 2. Set navigable's active session history entry's document state's reload pending to true.
    NonnullRefPtr reloading_entry = *active_session_history_entry();
    reloading_entry->document_state->reload_pending = true;
    session_history_changed();

    // 3. Let traversable be navigable's traversable navigable.
    // 4. Append the following session history traversal steps to traversable:
    //    1. Apply the reload history step to traversable given userInvolvement.
    auto parameters = Web::ReloadHistoryOperationParameters {
        .navigable_id = id(),
        .user_involvement = user_involvement,
    };
    enqueue_history_operation(Application::the().allocate_ui_process_cross_process_id(), parameters, {}, next_sequence_number(), [this, reloading_entry, on_complete = move(on_complete)](Web::HTML::HistoryStepResult result, Optional<i32> committed_step) {
        // NB: A reload that did not apply leaves no navigation behind to clear the pending flag.
        if (result != Web::HTML::HistoryStepResult::Applied && reloading_entry->document_state->reload_pending) {
            reloading_entry->document_state->reload_pending = false;
            session_history_changed();
        }
        if (on_complete)
            on_complete(result, committed_step);
    });
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-top-level-traversable
CanonicalTraversable& CanonicalTraversable::create_a_new_top_level_traversable(Web::HTML::CrossProcessId id, Optional<CanonicalNavigable&> opener, Web::HTML::SessionHistoryEntryDescriptor initial_history_entry)
{
    // 1. Let document be null.
    RefPtr<CanonicalDocument> document;

    // 2. If opener is null, then set document to the second return value of creating a new top-level browsing context and document.
    if (!opener.has_value()) {
        document = CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document().document;
    }
    // 3. Otherwise, set document to the second return value of creating a new auxiliary browsing context and document given opener.
    else {
        document = CanonicalBrowsingContext::create_a_new_auxiliary_browsing_context_and_document(*opener).document;
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
    auto traversable = make<CanonicalTraversable>(id);

    // 6. Initialize the navigable traversable given documentState.
    traversable->set_current_session_history_entry(history_entry);
    traversable->set_active_session_history_entry(history_entry);
    // NB: The initial document is active from the traversable's creation, before a process hosts it.
    traversable->set_hosted_state({
        .active_document_url = initial_history_entry.url,
        .active_document_is_fully_active = true,
        .opener_policy = {},
        .active_document_is_completely_loaded = false,
        .is_closing = false,
        .container = {},
        .delays_the_load_event_of_its_container = false,
        // The process hosting the traversable reports the compositor context it paints through.
        .compositor_context_id = {},
    });

    // 7. Let initialHistoryEntry be traversable's active session history entry.
    // 8. Set initialHistoryEntry's step to 0.
    history_entry->step = 0;

    // 9. Append initialHistoryEntry to traversable's session history entries.
    traversable->m_session_history.initialize_with_initial_history_entry(move(history_entry));

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
        run_browser_history_traversal_at_queue_position(
            Web::TraverseToStepHistoryOperationParameters {
                .target_step = step,
                .user_involvement = Web::HTML::UserNavigationInvolvement::BrowserUI,
            },
            false, next_sequence_number(), nullptr, nullptr, move(promise));
    });
}

ErrorOr<URL::URL> CanonicalTraversable::restore_session_history_from_ui_snapshot(SessionHistorySnapshot snapshot)
{
    TRY(m_session_history.restore_from_ui_snapshot(move(snapshot.entries), move(snapshot.used_steps), snapshot.current_used_step_index, [] { return Application::the().allocate_ui_process_cross_process_id(); }));
    session_history_changed();

    auto* current_entry = m_session_history.current_entry();
    VERIFY(current_entry);
    return current_entry->url;
}

void CanonicalTraversable::abandon_after_web_content_process_crash()
{
    abandon_history_operations();

    // https://html.spec.whatwg.org/multipage/document-lifecycle.html#destroy-a-document
    // 9. Set document's node navigable's active session history entry's document state's document to null.
    // NB: The crashed process destroyed the document. The traversable displays it until another is activated, on an
    //     entry of no session history, so that the next traversal populates the one it had.
    NonnullRefPtr document = active_document();
    active_session_history_entry()->document_state->document = nullptr;
    set_active_session_history_entry(CanonicalSessionHistoryEntry::create(CanonicalDocumentState::create(Application::the().allocate_ui_process_cross_process_id(), move(document))));
}

void CanonicalTraversable::reset_session_history_for_testing(
    Web::HTML::SessionHistoryEntryDescriptor active_entry)
{
    abandon_history_operations();
    m_session_history.clear();
    auto entry = MUST(CanonicalSessionHistoryEntry::create_from_descriptor(active_entry));
    entry->document_state->document = active_document();
    set_current_session_history_entry(entry);
    set_active_session_history_entry(entry);
    m_session_history.initialize_with_initial_history_entry(move(entry));
    session_history_changed();
}

bool CanonicalTraversable::initialize_session_history_for_testing(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index)
{
    abandon_history_operations();
    if (!m_session_history.initialize_for_testing(move(entries), move(used_steps), current_used_step_index))
        return false;
    auto* active_entry = m_session_history.current_entry();
    VERIFY(active_entry);
    if (auto const& previous_active_entry = active_session_history_entry())
        active_entry->document_state->document = previous_active_entry->document_state->document;
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

struct CanonicalTraversable::HistoryOperation {
    AK_ALLOC_WITH_KMALLOC;

    HistoryOperation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters parameters, RefPtr<WebContentPage> initiating_page, u64 sequence_number, RefPtr<CanonicalSessionHistoryEntry> target_entry, OnHistoryOperationComplete on_complete)
        : operation_id(operation_id)
        , parameters(move(parameters))
        , target_entry(move(target_entry))
        , on_complete(move(on_complete))
        , initiating_page(move(initiating_page))
        , sequence_number(sequence_number)
    {
    }

    Web::HTML::CrossProcessId operation_id;
    Web::HistoryOperationParameters parameters;
    // The entry synchronous navigation steps finalize.
    RefPtr<CanonicalSessionHistoryEntry> target_entry;
    OnHistoryOperationComplete on_complete;
    // Jobs resolve their endpoints at dispatch. This endpoint owns the process-local operation state.
    RefPtr<WebContentPage> initiating_page;
    Vector<NonnullRefPtr<WebContentPage>> completion_endpoints;
    u64 sequence_number;
    bool was_initiated_by_browser { false };
    bool owns_navigation_transaction { false };
    bool check_for_cancelation { false };
    Function<void()> on_browser_traversal_ready;
    Function<void(Web::HTML::HistoryStepResult)> pending_unload_cancelation;
    RefPtr<WebContentPage> unload_cancelation_endpoint;

    struct PendingBeforeunloadGroup {
        NonnullRefPtr<WebContentPage> endpoint;
        Vector<Web::HTML::CrossProcessId> navigable_ids;
    };
    Vector<PendingBeforeunloadGroup> pending_beforeunload_groups;
    RefPtr<WebContentPage> dispatched_beforeunload_endpoint;
    Web::HTML::UnloadPromptShown beforeunload_prompt_shown { Web::HTML::UnloadPromptShown::No };

    struct PendingChangingJob {
        AK_ALLOC_WITH_KMALLOC;

        enum class Phase : u8 {
            Dispatched,
            ReadyReported,
            ContinuationDispatched,
        };

        PendingChangingJob(ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob job, Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)> on_complete)
            : job(move(job))
            , on_complete(move(on_complete))
        {
        }

        ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob job;
        Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)> on_complete;
        Optional<ApplyHistoryStepJobs::ApplyChangingNavigableHistoryStepContinuation> continuation;
        Function<void()> on_continuation_complete;
        Phase phase { Phase::Dispatched };
        // The page the job's latest message went to, which owes the job its reply.
        RefPtr<WebContentPage> dispatched_endpoint;

        Web::HTML::UnloadDisplayedDocument unload_displayed_document { Web::HTML::UnloadDisplayedDocument::No };

        bool unload_preparation_pending { false };
        OwnPtr<NavigationLoader> population_loader;
        CanonicalNavigable::DidPopulateDocument did_populate_document { CanonicalNavigable::DidPopulateDocument::No };
    };
    HashMap<Web::HTML::CrossProcessId, NonnullOwnPtr<PendingChangingJob>> pending_changing_jobs;
    struct PendingNonchangingUpdate {
        Web::HTML::HistoryObjectLengthAndIndex history_object_length_and_index;
        Function<void()> on_complete;
        NonnullRefPtr<WebContentPage> endpoint;
    };
    HashMap<Web::HTML::CrossProcessId, PendingNonchangingUpdate> pending_nonchanging_updates;
    OwnPtr<ApplyHistoryStep> algorithm;
    RefPtr<Core::Promise<Empty>> queue_promise;

    bool is_browser_traversal() const { return was_initiated_by_browser; }
    bool was_initiated_by(WebContentPage const& page) const { return initiating_page.ptr() == &page; }
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

// A page holds the document of a navigable that it hosts, or the document of the navigable's next activation that it
// populates while another page hosts the active document.
CanonicalDocument& CanonicalTraversable::document_active_in(CanonicalNavigable& navigable, WebContentPage const& page) const
{
    if (navigable.active_document().host() == &page)
        return navigable.active_document();
    RefPtr<CanonicalDocument> document;
    navigable.for_each_populated_document([&](PopulatedDocument const& populated_document) {
        if (populated_document.document->host() == &page)
            document = populated_document.document;
    });
    return document ? *document : navigable.active_document();
}

Optional<size_t> CanonicalTraversable::effective_current_session_history_step_index() const
{
    for (auto const& operation : m_history_operations) {
        if (!operation.value->is_browser_traversal())
            continue;
        auto const& parameters = operation.value->parameters.get<Web::TraverseToStepHistoryOperationParameters>();
        auto target = m_session_history.traversal_target_for_step(parameters.target_step);
        if (target.has_value())
            return target->target_step_index;
    }
    return m_session_history.current_used_step_index();
}

CanonicalTraversable::HistoryOperation* CanonicalTraversable::ongoing_browser_history_traversal()
{
    for (auto& operation : m_history_operations) {
        if (operation.value->is_browser_traversal())
            return operation.value.ptr();
    }
    return nullptr;
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
        auto target_step = used_step_at_delta(m_session_history, operation->parameters.get<Web::TraverseToStepHistoryOperationParameters>().target_step, delta);
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
            auto current_step = m_session_history.current_step();
            if (!current_step.has_value())
                return {};
            return used_step_at_delta(m_session_history, *current_step, delta);
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
        view->cancel_uncommitted_top_level_navigation_for_browser_traversal();

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

    run_browser_history_traversal_at_queue_position(
        Web::TraverseToStepHistoryOperationParameters {
            .target_step = *target_step,
            .user_involvement = Web::HTML::UserNavigationInvolvement::BrowserUI,
        },
        check_for_cancelation == CheckForCancelation::Yes, next_sequence_number(), move(on_ready), nullptr, move(promise));
}

void CanonicalTraversable::supersede_browser_history_traversal(HistoryOperation& operation, i32 target_step, Function<void()> on_ready)
{
    VERIFY(operation.queue_promise);
    auto promise = operation.queue_promise.release_nonnull();
    auto operation_id = operation.operation_id;

    auto view = this->view();
    VERIFY(view.has_value());
    if (has_uncommitted_navigation())
        view->cancel_uncommitted_top_level_navigation_for_browser_traversal();
    finish_history_operation(operation_id, Web::HTML::HistoryStepResult::CanceledByNavigate, {});
    run_browser_history_traversal_at_queue_position(
        Web::TraverseToStepHistoryOperationParameters {
            .target_step = target_step,
            .user_involvement = Web::HTML::UserNavigationInvolvement::BrowserUI,
        },
        true, next_sequence_number(), move(on_ready), nullptr, move(promise));
}

Optional<CanonicalTraversable::BrowserHistoryTraversalDiagnostic> CanonicalTraversable::browser_history_traversal_for_testing() const
{
    for (auto const& operation : m_history_operations) {
        if (!operation.value->is_browser_traversal())
            continue;
        VERIFY(operation.value->parameters.has<Web::TraverseToStepHistoryOperationParameters>());
        auto const& parameters = operation.value->parameters.get<Web::TraverseToStepHistoryOperationParameters>();
        auto target = m_session_history.traversal_target_for_step(parameters.target_step);
        if (!target.has_value())
            return {};
        return BrowserHistoryTraversalDiagnostic {
            .target_step = parameters.target_step,
            .target_step_index = target->target_step_index,
            .changes_top_level_entry = target->changes_top_level_entry,
            .stage = operation.value->pending_unload_cancelation
                ? BrowserHistoryTraversalDiagnostic::Stage::CheckingCancelation
                : BrowserHistoryTraversalDiagnostic::Stage::ApplyingInWebContent,
        };
    }
    return {};
}

CanonicalSessionHistoryEntry const* CanonicalTraversable::ongoing_browser_history_traversal_target_entry() const
{
    for (auto const& operation : m_history_operations) {
        if (!operation.value->is_browser_traversal())
            continue;

        auto const& parameters = operation.value->parameters.get<Web::TraverseToStepHistoryOperationParameters>();
        if (auto target = m_session_history.traversal_target_for_step(parameters.target_step); target.has_value())
            return target->target_top_level_entry;
    }
    return nullptr;
}

void CanonicalTraversable::recover_from_web_content_process_crash(OnHistoryOperationComplete on_complete)
{
    // The step a traversal the crash interrupted was applying is applied again, otherwise the current step is.
    Optional<i32> target_step;
    if (auto* traversal = ongoing_browser_history_traversal())
        target_step = traversal->parameters.get<Web::TraverseToStepHistoryOperationParameters>().target_step;
    abandon_history_operations();
    if (!target_step.has_value())
        target_step = m_session_history.current_step();
    if (!target_step.has_value()) {
        if (on_complete)
            on_complete(Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
        return;
    }
    set_current_session_history_entry({});
    enqueue_browser_history_traversal(
        Web::TraverseToStepHistoryOperationParameters {
            .target_step = *target_step,
            .user_involvement = Web::HTML::UserNavigationInvolvement::BrowserUI,
        },
        false,
        move(on_complete));
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

    struct PendingUnloadCompletion {
        Web::HTML::CrossProcessId unload_id;
        Web::HTML::CrossProcessId navigable_id;
    };
    struct PendingJobCompletions {
        Web::HTML::CrossProcessId operation_id;
        Vector<Web::HTML::CrossProcessId> changing_jobs;
        Vector<Web::HTML::CrossProcessId> nonchanging_updates;
    };
    Vector<PendingUnloadCompletion> unload_completions;
    Vector<PendingJobCompletions> job_completions;
    Vector<Web::HTML::CrossProcessId> beforeunload_advances;
    auto advance_beforeunload_once = [&](Web::HTML::CrossProcessId operation_id) {
        if (!beforeunload_advances.contains_slow(operation_id))
            beforeunload_advances.append(operation_id);
    };

    auto lost_endpoint_is_root = page_hosting(*this) == page;

    auto endpoint_matches = [&](WebContentPage const& endpoint) {
        return &endpoint == &page;
    };

    // The page must already read as closed: the completions below can synchronously make a pending unload's
    // parent ready, and its dispatch must not select the endpoint which just disappeared.
    VERIFY(!page.is_open());

    for (auto const& pending_unload : m_pending_unloads) {
        for (auto const& node : pending_unload.value.nodes) {
            // Only leaves and parents whose descendants have completed can have an unload task in flight.
            if (node.value.remaining_children == 0 && node.value.endpoint && endpoint_matches(*node.value.endpoint)) {
                unload_completions.append({
                    pending_unload.key,
                    node.key,
                });
            }
        }
    }

    for (auto& operation_entry : m_history_operations) {
        auto& operation = *operation_entry.value;

        if (!lost_endpoint_is_root
            && operation.unload_cancelation_endpoint
            && endpoint_matches(*operation.unload_cancelation_endpoint)) {
            operation.unload_cancelation_endpoint.clear();
            advance_beforeunload_once(operation.operation_id);
        }

        if (operation.dispatched_beforeunload_endpoint
            && endpoint_matches(*operation.dispatched_beforeunload_endpoint)) {
            operation.dispatched_beforeunload_endpoint.clear();
            advance_beforeunload_once(operation.operation_id);
        }

        // The tab's document is recovered by applying the interrupted step again in a page obtained after the crash.
        // An embedded process's document is discarded, so complete its other queued history work as missing-endpoint
        // work instead of leaving the traversal queue waiting for replies that cannot arrive.
        if (!lost_endpoint_is_root) {
            PendingJobCompletions completions {
                .operation_id = operation.operation_id,
                .changing_jobs = {},
                .nonchanging_updates = {},
            };
            // NB: The page a job's message went to, not the page it would go to now: destroying the navigable
            //     releases its next document's host, and the reply that host owes is dropped with it.
            for (auto const& job : operation.pending_changing_jobs) {
                if (job.value->dispatched_endpoint && endpoint_matches(*job.value->dispatched_endpoint))
                    completions.changing_jobs.append(job.key);
            }
            for (auto const& update : operation.pending_nonchanging_updates) {
                if (endpoint_matches(update.value.endpoint))
                    completions.nonchanging_updates.append(update.key);
            }
            if (!completions.changing_jobs.is_empty() || !completions.nonchanging_updates.is_empty())
                job_completions.append(move(completions));
        }
    }

    // A check waiting on the lost page proceeds past it: its documents are gone.
    Vector<Web::HTML::CrossProcessId> beforeunload_check_advances;
    for (auto& check : m_pending_beforeunload_checks) {
        if (check.value.dispatched_endpoint && endpoint_matches(*check.value.dispatched_endpoint)) {
            check.value.dispatched_endpoint.clear();
            beforeunload_check_advances.append(check.key);
        }
    }
    for (auto check_id : beforeunload_check_advances)
        dispatch_next_beforeunload_group(check_id);

    for (auto const& completion : unload_completions)
        complete_descendant_unload_task(completion.unload_id, completion.navigable_id);
    for (auto& completions : job_completions) {
        if (auto* operation = find_history_operation(completions.operation_id))
            complete_history_jobs_of_lost_page(*operation, move(completions.changing_jobs), move(completions.nonchanging_updates));
    }
    for (auto operation_id : beforeunload_advances) {
        if (auto* operation = find_history_operation(operation_id))
            dispatch_next_beforeunload_group(*operation);
    }
}

CanonicalTraversable::HistoryOperation* CanonicalTraversable::find_history_operation(Web::HTML::CrossProcessId operation_id)
{
    auto operation = m_history_operations.find(operation_id);
    if (operation == m_history_operations.end())
        return nullptr;
    return operation->value.ptr();
}

bool CanonicalTraversable::navigation_transaction_matches(HistoryOperation const& operation, WebContentPage& page, Optional<Web::HTML::CrossProcessId> reply_navigable_id) const
{
    if (!operation.parameters.has<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>())
        return true;

    auto const& parameters = operation.parameters.get<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>();
    if (reply_navigable_id.has_value() && *reply_navigable_id != parameters.navigable_id)
        return true;
    auto navigable = find(parameters.navigable_id);
    if (!navigable.has_value())
        return false;

    // A javascript: navigation clears the spec's ongoing-navigation value before finalization and therefore has no
    // UI-owned population transaction. It must still come from the process hosting the active document.
    if (!parameters.navigation_id.has_value())
        return page_hosting(*navigable) == page;

    return navigable->navigation_transaction_matches(*parameters.navigation_id, page);
}

void CanonicalTraversable::add_history_operation_completion_endpoint(HistoryOperation& operation, NonnullRefPtr<WebContentPage> endpoint)
{
    if (any_of(operation.completion_endpoints, [&](auto const& completion_endpoint) { return completion_endpoint == endpoint; }))
        return;
    operation.completion_endpoints.append(move(endpoint));
}

bool CanonicalTraversable::select_changing_navigable_history_step_job_endpoint(HistoryOperation& operation, ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob& job)
{
    auto navigable = find(job.navigable_id);
    if (!navigable.has_value())
        return false;

    auto endpoint = changing_job_endpoint(*navigable, *job.target_entry->document_state);
    if (!endpoint || !endpoint->is_open())
        return false;

    if (navigable->is_top_level_traversable()) {
        auto callback = move(operation.on_browser_traversal_ready);
        if (callback)
            callback();
    }

    add_history_operation_completion_endpoint(operation, *endpoint);

    // A reload's top-level job repopulates the view's document; begin the recorded load where the job is
    // dispatched instead of having the job echo it back.
    if (navigable->is_top_level_traversable()
        && operation.parameters.has<Web::ReloadHistoryOperationParameters>()) {
        if (endpoint->is_open())
            endpoint->begin_top_level_load({}, job.target_entry->url);
    }
    return true;
}

void CanonicalTraversable::did_finish_history_navigation_params_creation(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryNavigationPopulation population)
{
    auto navigable_id = population.request.navigable_id;
    auto* operation = find_history_operation(operation_id);
    auto discard = [&] { NavigationLoader::discard(source_page.client().is_private(), population.result); };
    if (!operation) {
        discard();
        return;
    }
    auto job = operation->pending_changing_jobs.get(navigable_id);
    if (!job.has_value() || changing_job_endpoint(*operation, navigable_id) != source_page
        || job.value()->population_loader
        || !navigation_transaction_matches(*operation, source_page, navigable_id)) {
        discard();
        return;
    }
    auto& loader = job.value()->population_loader;
    loader = NavigationLoader::create(source_page.client().is_private(), move(population.request));
    loader->did_finish_navigation_params_creation(move(population.result));
    loader->acquire_response_body([weak_this = make_weak_ptr(), operation_id, navigable_id, source_page = NonnullRefPtr<WebContentPage>(source_page)](bool succeeded) {
        if (!weak_this)
            return;
        auto& traversable = static_cast<CanonicalTraversable&>(*weak_this);
        if (!succeeded) {
            traversable.did_receive_changing_navigable_history_job_ready(*source_page, operation_id, navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped, Web::HTML::UnloadDisplayedDocument::No);
            return;
        }
        traversable.continue_history_navigation_population(operation_id, navigable_id);
    });
}

void CanonicalTraversable::continue_history_navigation_population(Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id)
{
    auto* operation = find_history_operation(operation_id);
    auto navigable = find(navigable_id);
    if (!operation || !navigable.has_value())
        return;
    auto pending_job = operation->pending_changing_jobs.get(navigable_id);
    auto endpoint = changing_job_endpoint(*operation, navigable_id);
    if (!pending_job.has_value() || !endpoint || !pending_job.value()->population_loader)
        return;
    auto loader = move(pending_job.value()->population_loader);

    // The task queued by step 5 of attempting to populate the history entry's document runs in the process hosting
    // the browsing context that the document it creates belongs to. A response that creates no document is finished
    // by the process that fetched it.
    auto response_document = loader->response_document();
    if (response_document.has_value()) {
        pending_job.value()->did_populate_document = CanonicalNavigable::DidPopulateDocument::Yes;
        auto document = navigable->create_and_initialize_a_document(*response_document);
        navigable->populate_document(pending_job.value()->job.target_entry->document_state, *document);

        // A document created for inline content stands in for the resource the process that fetched it could not
        // load; that process hosts it.
        if (!response_document->is_inline_content || navigable->is_top_level_traversable()) {
            auto host = navigable->obtain_page_to_host(*document, pending_job.value()->job.target_entry->document_state->initiator_origin);
            if (host.is_error()) {
                did_receive_changing_navigable_history_job_ready(*endpoint, operation_id, navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped, Web::HTML::UnloadDisplayedDocument::No);
                return;
            }
            // Obtaining the page can replace the tab's process, whose change callbacks can finish the operation.
            endpoint = host.release_value();
            operation = find_history_operation(operation_id);
            if (!operation)
                return;
            pending_job = operation->pending_changing_jobs.get(navigable_id);
            if (!pending_job.has_value())
                return;
        }
        // The host takes the navigable over when the document is activated, after the displayed document is unloaded.
        navigable->place_pending_document(*endpoint);
    }
    add_history_operation_completion_endpoint(*operation, *endpoint);
    auto& job = *pending_job.value();
    job.dispatched_endpoint = endpoint;
    endpoint->async_continue_history_navigation_population(operation_id, job.job.target_entry_descriptor(), job.job.navigation_type,
        Web::HTML::HistoryNavigationPopulation { loader->request(), loader->take_result() });
    job.population_loader = move(loader);
}

void CanonicalTraversable::dispatch_changing_navigable_history_step_job(HistoryOperation& operation, Web::HTML::CrossProcessId navigable_id)
{
    auto endpoint = changing_job_endpoint(operation, navigable_id);
    VERIFY(endpoint);
    auto pending_job = operation.pending_changing_jobs.get(navigable_id);
    VERIFY(pending_job.has_value());

    if (pending_job.value()->population_loader)
        pending_job.value()->population_loader->reclaim_response_body_after_failed_handoff();
    pending_job.value()->population_loader = nullptr;
    auto target_entry = pending_job.value()->job.target_entry_descriptor();
    pending_job.value()->dispatched_endpoint = endpoint;
    endpoint->async_run_changing_navigable_history_job(
        operation.operation_id, navigable_id,
        move(target_entry), pending_job.value()->job.user_involvement,
        pending_job.value()->job.navigation_type,
        pending_job.value()->job.traversal_yields_to,
        pending_job.value()->job.canceled_navigation_id);
}

void CanonicalTraversable::dispatch_changing_navigable_history_step_continuation(HistoryOperation& operation, Web::HTML::CrossProcessId navigable_id)
{
    auto pending_job = operation.pending_changing_jobs.get(navigable_id);
    VERIFY(pending_job.has_value());
    VERIFY(pending_job.value()->continuation.has_value());
    VERIFY(pending_job.value()->phase == HistoryOperation::PendingChangingJob::Phase::ReadyReported);
    pending_job.value()->phase = HistoryOperation::PendingChangingJob::Phase::ContinuationDispatched;

    // 10. If changingNavigableContinuation's update-only is true, or targetEntry's document is displayedDocument:
    if (pending_job.value()->unload_displayed_document == Web::HTML::UnloadDisplayedDocument::No) {
        // 1. Set navigable's ongoing navigation to null.
        // NB: The canonical traversal claim is released when the continuation is applied. The queued task mirrors
        //     the null transition into the process-local projection.

        // 2. Queue a global task on the navigation and traversal task source given navigable's active window to
        //    perform afterPotentialUnloads.
        send_changing_navigable_continuation_task(operation, navigable_id, Web::HTML::UnloadDisplayedDocument::No);
    } else {
        // 11. Otherwise:
        // 1. Assert: navigationType is not null.
        VERIFY(pending_job.value()->job.navigation_type.has_value());

        // 2. Deactivate displayedDocument, given userInvolvement, targetEntry, navigationType, and
        //    afterPotentialUnloads.
        deactivate_a_document_for_cross_document_navigation(operation, navigable_id);
    }
}

void CanonicalTraversable::send_changing_navigable_continuation_task(HistoryOperation& operation, Web::HTML::CrossProcessId navigable_id, Web::HTML::UnloadDisplayedDocument unload_displayed_document)
{
    auto endpoint = changing_job_endpoint(operation, navigable_id);
    if (!endpoint)
        return;
    auto pending_job = operation.pending_changing_jobs.get(navigable_id);
    VERIFY(pending_job.has_value());
    VERIFY(pending_job.value()->continuation.has_value());

    auto const& document_state = *pending_job.value()->job.target_entry->document_state;
    if (unload_displayed_document == Web::HTML::UnloadDisplayedDocument::Yes && pending_job.value()->job.target_entry_reload_pending) {
        // INTEROP: Reloading rebuilds the child history tree from the replacement document, as in WebKit.
        //          Keep the old entries until population succeeds so an abandoned reload preserves them.
        auto navigable = find(navigable_id);
        if (navigable.has_value()) {
            Vector<Web::HTML::CrossProcessId> nested_history_ids;
            for (auto const& nested_history : document_state.nested_histories)
                nested_history_ids.append(nested_history.id);
            for (auto nested_history_id : nested_history_ids)
                remove_nested_history(*navigable, document_state.id, nested_history_id);
        }
    }

    auto continuation = *pending_job.value()->continuation;
    pending_job.value()->dispatched_endpoint = endpoint;
    endpoint->async_apply_changing_navigable_continuation(
        operation.operation_id, navigable_id,
        continuation.history_object_length_and_index.script_history_length,
        continuation.history_object_length_and_index.script_history_index,
        move(continuation.entries_for_navigation_api),
        system_visibility_state(),
        unload_displayed_document);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#deactivate-a-document-for-a-cross-document-navigation
void CanonicalTraversable::deactivate_a_document_for_cross_document_navigation(HistoryOperation& operation, Web::HTML::CrossProcessId navigable_id)
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
    // NB: The process running the job applies this step to its projection of the navigable, and reports back before
    //     step 3 runs in unload_displayed_document_for_cross_document_navigation().
    if (auto navigable = find(navigable_id); navigable.has_value())
        navigable->clear_ongoing_navigation_traversal(operation.operation_id);

    auto pending_job = operation.pending_changing_jobs.get(navigable_id);
    VERIFY(pending_job.has_value());
    VERIFY(!pending_job.value()->unload_preparation_pending);
    pending_job.value()->unload_preparation_pending = true;

    auto endpoint = changing_job_endpoint(operation, navigable_id);
    if (!endpoint)
        return;
    pending_job.value()->dispatched_endpoint = endpoint;
    endpoint->async_prepare_changing_navigable_for_unload(operation.operation_id, navigable_id);

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

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#deactivate-a-document-for-a-cross-document-navigation
void CanonicalTraversable::unload_displayed_document_for_cross_document_navigation(HistoryOperation& operation, Web::HTML::CrossProcessId navigable_id)
{
    // 5. If potentiallyTriggerViewTransition is false, then:
    // 3. Unload a document and its descendants given displayedDocument, targetEntry's document,
    //    afterPotentialUnloads, and firePageSwapBeforeUnload.
    auto endpoint = changing_job_endpoint(operation, navigable_id);
    if (!endpoint)
        return;
    auto operation_id = operation.operation_id;
    unload_a_document_and_its_descendants(navigable_id, *endpoint, Web::HTML::ChildNavigableDestruction::No, [this, operation_id, navigable_id](UnloadedInItsHost) {
        // afterPotentialUnloads runs in the continuation task, in the process running the job. That process first
        // unloads the document it displays: displayedDocument, or the stand-in of a host chosen for the new document.
        auto* operation = find_history_operation(operation_id);
        if (!operation || !operation->pending_changing_jobs.contains(navigable_id))
            return;
        send_changing_navigable_continuation_task(*operation, navigable_id, Web::HTML::UnloadDisplayedDocument::Yes);
    });
}

// The process running a changing navigable's job activated targetEntry's document and applied the continuation's
// remaining steps.
void CanonicalTraversable::did_activate_history_entry(HistoryOperation& operation, Web::HTML::CrossProcessId navigable_id, NonnullRefPtr<WebContentPage> source_page, CanonicalSessionHistoryEntry& target_entry, CanonicalNavigable::DidPopulateDocument did_populate_document, Web::HTML::HostedNavigableState activated_navigable_state)
{
    auto navigable = find(navigable_id);
    if (!navigable.has_value())
        return;

    RefPtr<WebContentPage> host = page_hosting(*navigable);
    if (auto document = navigable->document_populated_for(*target_entry.document_state); document && document->host() == source_page)
        host = source_page;

    auto navigation_id = operation.parameters.visit(
        [](Web::FinalizeCrossDocumentNavigationHistoryOperationParameters const& parameters) { return parameters.navigation_id; },
        [](auto const&) { return Optional<Utf16String> {}; });
    navigable->did_commit_navigation(target_entry, move(activated_navigable_state), navigation_id, did_populate_document, move(host));

    if (navigable_id == id()) {
        if (auto view = this->view(); view.has_value()) {
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
void CanonicalTraversable::unload_a_document_and_its_descendants(Web::HTML::CrossProcessId navigable_id, RefPtr<WebContentPage> continuing_endpoint, Web::HTML::ChildNavigableDestruction child_navigable_destruction, Function<void(UnloadedInItsHost)> queue_document_unload_task)
{
    // 1. Assert: this is running within document's node navigable's traversable navigable's session history
    //    traversal queue. The UI process owns that queue. The recursion's bookkeeping runs here because the
    //    descendant subtrees can be hosted by other WebContent processes.

    // 2. Let childNavigables be document's child navigables.
    // 3. Let numberUnloaded be 0.
    // Snapshot the descendant tree, and the page hosting the document, before unload handlers can mutate the tree.
    // A navigable that disappears before its dispatch counts as unloaded.
    PendingUnload pending_unload;
    pending_unload.navigable_id = navigable_id;
    RefPtr<WebContentPage> document_host;
    if (auto navigable = find(navigable_id); navigable.has_value()) {
        // A document is unloaded in the page hosting it, whichever that is, before the document replacing it activates.
        // That is the page continuing the invoking algorithm for the traversable's document, unless another page
        // displays it while the view's process populates the document replacing it.
        document_host = navigable->active_document().host();
        if (!document_host)
            document_host = page_hosting(*navigable);
        Function<void(CanonicalNavigable const&, Optional<Web::HTML::CrossProcessId>)> append_subtree =
            [&](CanonicalNavigable const& descendant, Optional<Web::HTML::CrossProcessId> parent_id) {
                pending_unload.nodes.set(descendant.id(),
                    PendingUnload::Node {
                        .parent_id = parent_id,
                        .remaining_children = descendant.children().size(),
                        .endpoint = page_hosting(descendant),
                    });
                for (auto const& grandchild : descendant.children())
                    append_subtree(*grandchild, descendant.id());
            };
        for (auto const& child : navigable->children())
            append_subtree(*child, {});
        pending_unload.remaining_root_children = navigable->children().size();
    }

    // 6. Queue a global task on the navigation and traversal task source given document's relevant global object
    //    to perform the following steps:
    //    1. If firePageSwapSteps is given, then run firePageSwapSteps.
    //    2. Unload document, passing along newDocument if it is not null.
    //    3. If afterAllUnloads was given, then run it.
    // NB: queue_document_unload_task dispatches this task to the page continuing the invoking algorithm, once every
    //     child subtree has completed. When another page hosts the document, that page unloads it first, and the
    //     task is told so. This happens immediately when the document has no child navigables.
    pending_unload.queue_document_unload_task = [this, navigable_id, continuing_endpoint, child_navigable_destruction, document_host = move(document_host), queue_document_unload_task = move(queue_document_unload_task)] mutable {
        if (!document_host || document_host == continuing_endpoint) {
            queue_document_unload_task(UnloadedInItsHost::No);
            return;
        }
        unload_document_in_its_host(document_host.release_nonnull(), navigable_id, child_navigable_destruction, [queue_document_unload_task = move(queue_document_unload_task)] {
            queue_document_unload_task(UnloadedInItsHost::Yes);
        });
    };
    if (pending_unload.nodes.is_empty()) {
        pending_unload.queue_document_unload_task();
        return;
    }

    // 4. For each childNavigable of childNavigables [[ in what order? ]], queue a global task on the navigation
    //    and traversal task source given childNavigable's active window to perform the following steps:
    //    1. Let incrementUnloaded be an algorithm step which increments numberUnloaded.
    //    2. Unload a document and its descendants given childNavigable's active document, null, and
    //       incrementUnloaded.
    // NB: The nested recursion is this walk itself: a node's task is dispatched only after its own subtree has
    //     completed, so the leaves go first and every completion dispatches its parent once the parent's other
    //     children are done. Collect the leaves before dispatching: a missing endpoint completes synchronously
    //     and mutates the node set.
    Vector<Web::HTML::CrossProcessId> leaves;
    for (auto const& node : pending_unload.nodes) {
        if (node.value.remaining_children == 0)
            leaves.append(node.key);
    }
    auto unload_id = Application::the().allocate_ui_process_cross_process_id();
    m_pending_unloads.set(unload_id, move(pending_unload));
    for (auto leaf : leaves)
        dispatch_descendant_unload_task(unload_id, leaf);
}

void CanonicalTraversable::dispatch_descendant_unload_task(Web::HTML::CrossProcessId unload_id, Web::HTML::CrossProcessId navigable_id)
{
    auto pending_unload = m_pending_unloads.find(unload_id);
    if (pending_unload == m_pending_unloads.end())
        return;
    auto node = pending_unload->value.nodes.find(navigable_id);
    if (node == pending_unload->value.nodes.end())
        return;

    // A page unloads the documents it hosts even when an operation's jobs left it for the process replacing it.
    auto endpoint = node->value.endpoint;
    if (!endpoint || !endpoint->is_open()) {
        complete_descendant_unload_task(unload_id, navigable_id);
        return;
    }
    endpoint->async_run_descendant_unload_task(unload_id, navigable_id, node->value.child_navigable_destruction, node->value.stop_hosting_after_unload);
}

void CanonicalTraversable::complete_descendant_unload_task(Web::HTML::CrossProcessId unload_id, Web::HTML::CrossProcessId navigable_id)
{
    auto pending_unload = m_pending_unloads.find(unload_id);
    if (pending_unload == m_pending_unloads.end())
        return;
    auto node = pending_unload->value.nodes.take(navigable_id);
    if (!node.has_value())
        return;
    VERIFY(node->remaining_children == 0);

    // 5. Wait until numberUnloaded equals childNavigable's size.
    if (node->parent_id.has_value()) {
        auto parent = pending_unload->value.nodes.find(*node->parent_id);
        VERIFY(parent != pending_unload->value.nodes.end());
        VERIFY(parent->value.remaining_children > 0);
        --parent->value.remaining_children;
        if (parent->value.remaining_children == 0)
            dispatch_descendant_unload_task(unload_id, parent->key);
        return;
    }

    VERIFY(pending_unload->value.remaining_root_children > 0);
    --pending_unload->value.remaining_root_children;
    if (pending_unload->value.remaining_root_children == 0) {
        // 6. Queue a global task ... to fire pageswap, unload document, and run afterAllUnloads.
        auto completed_unload = m_pending_unloads.take(unload_id);
        completed_unload->queue_document_unload_task();
    }
}

void CanonicalTraversable::did_receive_changing_navigable_unload_preparation_complete(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id)
{
    auto* operation = find_history_operation(operation_id);
    if (!operation)
        return;
    auto pending_job = operation->pending_changing_jobs.get(navigable_id);
    if (!pending_job.has_value() || !pending_job.value()->unload_preparation_pending)
        return;
    // NB: A job whose navigable is gone completes with what its page reports.
    if (auto endpoint = changing_job_endpoint(*operation, navigable_id); endpoint && endpoint != source_page)
        return;

    pending_job.value()->unload_preparation_pending = false;
    unload_displayed_document_for_cross_document_navigation(*operation, navigable_id);
}

// Step 6 of unload a document and its descendants for a document hosted by a page other than the one continuing the
// invoking algorithm: that page unloads it the way it unloads a descendant's, and afterAllUnloads runs once it has.
void CanonicalTraversable::unload_document_in_its_host(NonnullRefPtr<WebContentPage> endpoint, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChildNavigableDestruction child_navigable_destruction, Function<void()> after_unload)
{
    PendingUnload pending_unload;
    pending_unload.navigable_id = navigable_id;
    pending_unload.queue_document_unload_task = move(after_unload);
    pending_unload.nodes.set(navigable_id, PendingUnload::Node {
                                               .parent_id = {},
                                               .remaining_children = 0,
                                               .endpoint = move(endpoint),
                                               .child_navigable_destruction = child_navigable_destruction,
                                               // Another page replaces or destroys the document. This one represents the
                                               // navigable remotely from the moment the document is unloaded.
                                               .stop_hosting_after_unload = Web::HTML::StopHostingAfterUnload::Yes,
                                           });
    pending_unload.remaining_root_children = 1;
    auto unload_id = Application::the().allocate_ui_process_cross_process_id();
    m_pending_unloads.set(unload_id, move(pending_unload));
    dispatch_descendant_unload_task(unload_id, navigable_id);
}

bool CanonicalTraversable::is_unloading_document_of(Web::HTML::CrossProcessId navigable_id) const
{
    for (auto const& pending_unload : m_pending_unloads) {
        if (pending_unload.value.navigable_id == navigable_id)
            return true;
    }
    return false;
}

// Whether the page hosting the navigable's active document is unloading it, or has, for a document another page hosts.
// That page stops hosting the navigable once the document is unloaded, and the other page takes it over only when the
// document replacing it is activated.
bool CanonicalTraversable::is_handing_navigable_to_another_page(CanonicalNavigable const& navigable) const
{
    for (auto const& operation : m_history_operations) {
        auto pending_job = operation.value->pending_changing_jobs.get(navigable.id());
        if (!pending_job.has_value())
            continue;
        auto const& job = *pending_job.value();
        if (job.phase == HistoryOperation::PendingChangingJob::Phase::ContinuationDispatched
            && job.unload_displayed_document == Web::HTML::UnloadDisplayedDocument::Yes
            && job.dispatched_endpoint != navigable.active_document().host())
            return true;
    }
    return false;
}

RefPtr<WebContentPage> CanonicalTraversable::changing_job_endpoint(CanonicalNavigable const& navigable, CanonicalDocumentState const& target_document_state) const
{
    if (auto document = navigable.document_populated_for(target_document_state); document && document->host())
        return document->host();
    return page_hosting(navigable);
}

RefPtr<WebContentPage> CanonicalTraversable::changing_job_endpoint(HistoryOperation const& operation, Web::HTML::CrossProcessId navigable_id) const
{
    auto navigable = find(navigable_id);
    if (!navigable.has_value())
        return nullptr;
    auto job = operation.pending_changing_jobs.get(navigable_id);
    if (!job.has_value())
        return page_hosting(*navigable);
    return changing_job_endpoint(*navigable, *job.value()->job.target_entry->document_state);
}

void CanonicalTraversable::did_receive_descendant_unload_task_complete(WebContentPage& source_page, Web::HTML::CrossProcessId unload_id, Web::HTML::CrossProcessId navigable_id)
{
    auto pending_unload = m_pending_unloads.find(unload_id);
    if (pending_unload == m_pending_unloads.end())
        return;
    auto node = pending_unload->value.nodes.find(navigable_id);
    if (node == pending_unload->value.nodes.end())
        return;
    if (node->value.endpoint != source_page)
        return;
    if (node->value.remaining_children != 0)
        return;
    complete_descendant_unload_task(unload_id, navigable_id);
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
    unload_a_document_and_its_descendants(navigable_id, source_page, Web::HTML::ChildNavigableDestruction::Yes, [source_page = NonnullRefPtr<WebContentPage>(source_page), navigable_id](UnloadedInItsHost) {
        source_page->async_continue_child_navigable_destruction(navigable_id);
    });
}

void CanonicalTraversable::complete_history_jobs_of_lost_page(HistoryOperation& operation, Vector<Web::HTML::CrossProcessId> changing_jobs, Vector<Web::HTML::CrossProcessId> nonchanging_updates)
{
    Vector<Function<void()>> completions;
    for (auto navigable_id : changing_jobs) {
        auto pending_job = operation.pending_changing_jobs.take(navigable_id);
        if (!pending_job.has_value())
            continue;
        if (auto navigable = find(navigable_id); navigable.has_value())
            navigable->abandon_document_populated_for(*pending_job.value()->job.target_entry->document_state);

        if (pending_job.value()->population_loader)
            pending_job.value()->population_loader->reclaim_response_body_after_failed_handoff();

        switch (pending_job.value()->phase) {
        case HistoryOperation::PendingChangingJob::Phase::Dispatched:
            completions.append([on_complete = move(pending_job.value()->on_complete)]() mutable {
                on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped);
            });
            break;
        case HistoryOperation::PendingChangingJob::Phase::ReadyReported:
            // ApplyHistoryStep already retained this navigable in its continuation queue. If it has not supplied the
            // continuation yet, removing the retained job makes that future application complete locally.
            if (pending_job.value()->continuation.has_value())
                completions.append(move(pending_job.value()->on_continuation_complete));
            break;
        case HistoryOperation::PendingChangingJob::Phase::ContinuationDispatched:
            completions.append(move(pending_job.value()->on_continuation_complete));
            break;
        }
    }
    for (auto navigable_id : nonchanging_updates) {
        if (auto pending_update = operation.pending_nonchanging_updates.take(navigable_id); pending_update.has_value())
            completions.append(move(pending_update->on_complete));
    }
    for (auto& completion : completions) {
        if (completion)
            completion();
    }
}

ApplyHistoryStepJobs CanonicalTraversable::create_apply_history_step_jobs(Web::HTML::CrossProcessId operation_id)
{
    return {
        .run_unload_cancelation_job = [this, operation_id](ApplyHistoryStepJobs::UnloadCancelationJob job, Function<void(Web::HTML::HistoryStepResult)> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            operation->pending_unload_cancelation = move(on_complete);
            operation->unload_cancelation_endpoint.clear();
            operation->pending_beforeunload_groups.clear();
            operation->dispatched_beforeunload_endpoint.clear();
            operation->beforeunload_prompt_shown = Web::HTML::UnloadPromptShown::No;

            // The traversable's active-document process runs the navigate-event portion of the check. Other
            // processes only run beforeunload for the crossing documents they host.
            auto traversable_endpoint = page_hosting(*this);
            Vector<Web::HTML::CrossProcessId> traversable_subset;
            for (auto navigable_id : job.navigables_crossing_documents) {
                auto navigable = find(navigable_id);
                if (!navigable.has_value())
                    continue;
                auto endpoint = page_hosting(*navigable);
                if (!endpoint)
                    continue;
                if (endpoint == traversable_endpoint) {
                    traversable_subset.append(navigable_id);
                    continue;
                }
                auto group = operation->pending_beforeunload_groups.find_if([&](auto const& group) {
                    return group.endpoint == endpoint;
                });
                if (group == operation->pending_beforeunload_groups.end())
                    operation->pending_beforeunload_groups.append({ endpoint.release_nonnull(), { navigable_id } });
                else
                    group->navigable_ids.append(navigable_id);
            }

            if (!traversable_endpoint || !traversable_endpoint->is_open()) {
                dispatch_next_beforeunload_group(*operation);
                return;
            }

            operation->unload_cancelation_endpoint = traversable_endpoint;
            traversable_endpoint->async_run_history_step_unload_cancelation_job(operation_id, job.target_entry->descriptor(), move(traversable_subset), job.user_involvement); },
        .queue_navigation_api_state_clear_task = [this, operation_id](Web::HTML::CrossProcessId navigable_id) {
            auto* operation = find_history_operation(operation_id);
            auto navigable = find(navigable_id);
            if (!operation || !navigable.has_value())
                return;
            auto endpoint = page_hosting(*navigable);
            if (!endpoint)
                return;
            add_history_operation_completion_endpoint(*operation, *endpoint);
            endpoint->async_queue_navigation_api_state_clear_task(operation_id, navigable_id); },
        .select_changing_navigable_history_step_job_endpoint = [this, operation_id](ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob& job) {
            auto* operation = find_history_operation(operation_id);
            return operation && select_changing_navigable_history_step_job_endpoint(*operation, job); },
        .run_changing_navigable_history_step_job = [this, operation_id](ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob job, Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            auto navigable_id = job.navigable_id;
            operation->pending_changing_jobs.set(navigable_id, make<HistoryOperation::PendingChangingJob>(move(job), move(on_complete)));
            dispatch_changing_navigable_history_step_job(*operation, navigable_id); },
        .apply_changing_navigable_history_step_continuation = [this, operation_id](ApplyHistoryStepJobs::ApplyChangingNavigableHistoryStepContinuation continuation, Function<void()> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            auto navigable_id = continuation.navigable_id;
            auto pending_job = operation->pending_changing_jobs.get(navigable_id);
            if (!pending_job.has_value()) {
                on_complete();
                return;
            }
            // If a sync navigation that jumped the queue changed the job's target entry while the job was paused, then
            // the job follows — so that the entry recorded as the navigable's active one, and any re-dispatch of the
            // job, name the entry as the session history has it.
            if (continuation.updated_target_entry)
                pending_job.value()->job.target_entry = *continuation.updated_target_entry;
            pending_job.value()->continuation = move(continuation);
            pending_job.value()->on_continuation_complete = move(on_complete);
            if (pending_job.value()->phase == HistoryOperation::PendingChangingJob::Phase::ReadyReported)
                dispatch_changing_navigable_history_step_continuation(*operation, navigable_id); },
        .update_nonchanging_navigable_history_step_state = [this, operation_id](Web::HTML::CrossProcessId navigable_id, Web::HTML::HistoryObjectLengthAndIndex history_object_length_and_index, Function<void()> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            auto navigable = find(navigable_id);
            RefPtr<WebContentPage> endpoint = navigable.has_value() ? page_hosting(*navigable) : nullptr;
            if (!endpoint || !endpoint->is_open()) {
                on_complete();
                return;
            }
            add_history_operation_completion_endpoint(*operation, *endpoint);
            operation->pending_nonchanging_updates.set(navigable_id,
                HistoryOperation::PendingNonchangingUpdate {
                    history_object_length_and_index,
                    move(on_complete),
                    *endpoint,
                });
            endpoint->async_update_nonchanging_navigable_history_state(operation_id, navigable_id,
                history_object_length_and_index.script_history_length, history_object_length_and_index.script_history_index); },
    };
}

void CanonicalTraversable::run_history_operation_at_queue_position(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters request, RefPtr<WebContentPage> requesting_page, u64 sequence_number, RefPtr<CanonicalSessionHistoryEntry> target_entry, OnHistoryOperationComplete on_complete, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    // The traversal queue can outlive an embedded page which appended work to it. Such a page cannot run the
    // preparation step or receive completion, so discard its queued operation instead of waiting forever.
    if (requesting_page && !requesting_page->is_open()) {
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
    m_history_operations.set(operation_id, make<HistoryOperation>(operation_id, move(request), move(requesting_page), sequence_number, move(target_entry), move(on_complete)));
    auto* operation = find_history_operation(operation_id);
    VERIFY(operation);
    operation->queue_promise = promise;
    start_history_operation(*operation, promise);
}

void CanonicalTraversable::run_browser_history_traversal_at_queue_position(Web::TraverseToStepHistoryOperationParameters parameters, bool check_for_cancelation, u64 sequence_number, Function<void()> on_ready, OnHistoryOperationComplete on_complete, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    auto operation_id = Application::the().allocate_ui_process_cross_process_id();
    auto owned_operation = make<HistoryOperation>(operation_id, Web::HistoryOperationParameters { move(parameters) }, RefPtr<WebContentPage> {}, sequence_number, RefPtr<CanonicalSessionHistoryEntry> {}, move(on_complete));
    owned_operation->was_initiated_by_browser = true;
    owned_operation->check_for_cancelation = check_for_cancelation;
    owned_operation->on_browser_traversal_ready = move(on_ready);
    m_history_operations.set(operation_id, move(owned_operation));
    auto* operation = find_history_operation(operation_id);
    VERIFY(operation);
    operation->queue_promise = promise;
    auto view = this->view();
    VERIFY(view.has_value());
    view->will_apply_history_traversal_step(operation_id);
    start_history_operation(*operation, promise);
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
    entry->classic_history_api_state = same_document_entry.classic_history_api_state;
    entry->navigation_api_state = same_document_entry.navigation_api_state;
    entry->navigation_api_key = same_document_entry.navigation_api_key;
    entry->navigation_api_id = same_document_entry.navigation_api_id;
    entry->scroll_restoration_mode = same_document_entry.scroll_restoration_mode;
    entry->scroll_position_data = same_document_entry.scroll_position_data;
    return entry;
}

void CanonicalTraversable::enqueue_history_operation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters request, RefPtr<WebContentPage> requesting_page, u64 sequence_number, OnHistoryOperationComplete on_complete)
{
    // https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-child-navigable
    // Steps 6-7 remove the nested history before step 9 appends traversal steps. Apply the canonical counterpart while
    // admitting the destruction request, rather than deferring it to the operation's eventual queue position.
    if (request.has<Web::NavigableDestructionHistoryOperationParameters>()) {
        auto const& parameters = request.get<Web::NavigableDestructionHistoryOperationParameters>();
        if (auto parent_navigable = find(parameters.parent_navigable_id); parent_navigable.has_value())
            remove_nested_history(*parent_navigable, parameters.parent_document_state_id, parameters.navigable_id);
    }

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

    auto steps = [this, operation_id, request = move(request), requesting_page = move(requesting_page), sequence_number, target_entry, on_complete = move(on_complete)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_history_operation_at_queue_position(operation_id, move(request), move(requesting_page), sequence_number, move(target_entry), move(on_complete), move(promise));
    };

    if (synchronous_navigation_target.has_value())
        m_history_traversal_queue.append_session_history_synchronous_navigation_steps(*synchronous_navigation_target, move(target_entry), move(steps));
    else
        m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::enqueue_browser_history_traversal(Web::TraverseToStepHistoryOperationParameters parameters, bool check_for_cancelation, OnHistoryOperationComplete on_complete)
{
    auto sequence_number = next_sequence_number();
    auto steps = [this, parameters = move(parameters), check_for_cancelation, sequence_number, on_complete = move(on_complete)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_browser_history_traversal_at_queue_position(move(parameters), check_for_cancelation, sequence_number, nullptr, move(on_complete), move(promise));
    };
    m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::apply_history_step(HistoryOperation& operation, i32 step, bool check_for_cancelation, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement user_involvement, Optional<Web::Bindings::NavigationType> navigation_type, Optional<Web::InitiatorSourceSnapshot> initiator_source_snapshot)
{
    VERIFY(!operation.algorithm);
    auto operation_id = operation.operation_id;
    operation.algorithm = make<ApplyHistoryStep>(
        m_session_history, *this, m_history_traversal_queue, m_apply_history_step_traversable_state, create_apply_history_step_jobs(operation_id),
        operation_id, operation.sequence_number,
        step, check_for_cancelation, initiator_to_check, initiator_source_snapshot, user_involvement, navigation_type,
        [this, operation_id](Web::HTML::HistoryStepResult result) {
            auto* operation = find_history_operation(operation_id);
            auto committed_step = operation && operation->algorithm ? operation->algorithm->committed_step() : Optional<i32> {};
            finish_history_operation(operation_id, result, committed_step);
        });
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
    if (!step.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 2. Return the result of applying the history step step to traversable given true, null, null, userInvolvement, and "reload".
    apply_history_step(operation, *step, true, {}, user_involvement, Web::Bindings::NavigationType::Reload);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#apply-the-traverse-history-step
void CanonicalTraversable::apply_the_traverse_history_step(HistoryOperation& operation, i32 step, Optional<Web::InitiatorSourceSnapshot> initiator_source_snapshot, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement user_involvement)
{
    // 1. Return the result of applying the history step step to traversable given true, sourceSnapshotParams, initiatorToCheck, userInvolvement, and "traverse".
    apply_history_step(operation, step, true, initiator_to_check, user_involvement, Web::Bindings::NavigationType::Traverse, move(initiator_source_snapshot));
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
    if (!step.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 2. Return the result of applying the history step step to traversable given false, null, null, "none", and null.
    apply_history_step(operation, *step, false, {}, Web::HTML::UserNavigationInvolvement::None, {});
}

// Direct operations have their complete input in canonical state, so they enter apply-the-history-step at their
// queue position without a WebContent preparation round trip.
static bool history_operation_is_direct(Web::HistoryOperationParameters const& parameters)
{
    return parameters.has<Web::ReloadHistoryOperationParameters>()
        || parameters.has<Web::TraverseByDeltaHistoryOperationParameters>()
        || parameters.has<Web::TraverseToStepHistoryOperationParameters>()
        || parameters.has<Web::NavigationAPITraverseHistoryOperationParameters>()
        || parameters.has<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>()
        || parameters.has<Web::NavigableCreationHistoryOperationParameters>()
        || parameters.has<Web::NavigableDestructionHistoryOperationParameters>()
        || parameters.has<Web::CloseTopLevelTraversableHistoryOperationParameters>()
        || parameters.has<Web::FlushSessionHistoryTraversalQueueOperationParameters>();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#traverse-the-history-by-a-delta
void CanonicalTraversable::traverse_the_history_by_a_delta_at_queue_position(HistoryOperation& operation, Web::TraverseByDeltaHistoryOperationParameters const& request)
{
    // Steps 1-3 of traverse the history by a delta were performed by the source process before it appended these
    // session history traversal steps.

    // 1. Let allSteps be the result of getting all used history steps for traversable.
    auto all_steps = m_session_history.used_steps();

    // 2. Let currentStepIndex be the index of traversable's current session history step within allSteps.
    auto current_step = m_session_history.current_step();
    if (!current_step.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::Applied, {});
        return;
    }
    auto current_step_index = all_steps.find_first_index(*current_step);
    VERIFY(current_step_index.has_value());

    // 3. Let targetStepIndex be currentStepIndex plus delta.
    auto target_step_index = static_cast<i64>(*current_step_index) + static_cast<i64>(request.delta);

    // 4. If allSteps[targetStepIndex] does not exist, then abort these steps.
    if (target_step_index < 0 || static_cast<size_t>(target_step_index) >= all_steps.size()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::Applied, {});
        return;
    }

    // 5. Apply the traverse history step allSteps[targetStepIndex] to traversable, given sourceSnapshotParams,
    //    initiatorToCheck, and userInvolvement.
    apply_the_traverse_history_step(operation, all_steps[static_cast<size_t>(target_step_index)], request.initiator_source_snapshot, request.initiator_to_check, request.user_involvement);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#performing-a-navigation-api-traversal
void CanonicalTraversable::perform_a_navigation_api_traversal_at_queue_position(HistoryOperation& operation, Web::NavigationAPITraverseHistoryOperationParameters const& request)
{
    auto navigable = find(request.navigable_id);
    if (!navigable.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 12.1. Let navigableSHEs be the result of getting session history entries given navigable.
    auto navigable_session_history_entries = m_session_history.get_session_history_entries(*navigable);
    if (!navigable_session_history_entries.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 12.2. Let targetSHE be the session history entry in navigableSHEs whose navigation API key is key. If no
    //       such entry exists, queue rejection of the finished promise and abort these steps.
    auto target_entry = navigable_session_history_entries->find_if([&](auto const& entry) {
        return entry->navigation_api_key == request.key;
    });
    if (target_entry == navigable_session_history_entries->end()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 12.3. If targetSHE is navigable's active session history entry, queue rejection of the finished promise and
    //       abort these steps.
    if (auto const& active_entry = navigable->active_session_history_entry(); active_entry && active_entry->identity() == (*target_entry)->identity()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 12.4. Let result be the result of applying the traverse history step targetSHE's step to traversable, given
    //       sourceSnapshotParams, navigable, and "none".
    apply_the_traverse_history_step(operation, (*target_entry)->step, request.initiator_source_snapshot, request.navigable_id, Web::HTML::UserNavigationInvolvement::None);

    // Steps 12.5-12.6 are handled in Navigation's relevant realm when the operation completes.
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-same-document-navigation
void CanonicalTraversable::finalize_a_same_document_navigation(HistoryOperation& operation, Web::FinalizeSameDocumentNavigationHistoryOperationParameters const& request)
{
    VERIFY(request.entry_to_replace.has_value() == (request.history_handling == Web::HTML::HistoryHandlingBehavior::Replace));
    auto target_navigable = find(request.navigable_id);
    if (!target_navigable.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
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
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 3. Let targetStep be null.
    Optional<i32> target_step;

    // 4. Let targetEntries be the result of getting session history entries for targetNavigable.
    auto target_entries = m_session_history.get_session_history_entries(*target_navigable);
    if (!target_entries.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // AD-HOC: The active entry of a navigable whose history is being reconstructed, which is that of its initial
    //         about:blank, is not among its session history entries.
    if (!any_of(*target_entries, [&](auto const& entry) { return entry->document_state == target_entry->document_state; })) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    auto current_step = m_session_history.current_step();
    if (!current_step.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 5. If entryToReplace is null:
    if (!request.entry_to_replace.has_value()) {
        // 5.1. Clear the forward session history of traversable.
        // 5.2. Set targetStep to traversable's current session history step + 1.
        // 5.3. Set targetEntry's step to targetStep.
        // 5.4. Append targetEntry to targetEntries.
        target_step = m_session_history.push_session_history_entry(*target_navigable, *target_entry);
        if (!target_step.has_value()) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }
    }
    // Otherwise:
    else {
        auto entry_to_replace = target_entries->find_if([&](auto const& entry) {
            return entry->identity() == *request.entry_to_replace;
        });
        if (entry_to_replace == target_entries->end()) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }

        // 5.1. Replace entryToReplace with targetEntry in targetEntries.
        // 5.2. Set targetEntry's step to entryToReplace's step.
        if (!m_session_history.replace_session_history_entry(*target_navigable, **entry_to_replace, *target_entry)) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }

        // 5.3. Set targetStep to traversable's current session history step.
        target_step = *current_step;
    }

    target_navigable->set_active_session_history_entry(target_entry);

    // 6. Apply the push/replace history step targetStep to traversable given historyHandling and userInvolvement.
    apply_the_push_or_replace_history_step(operation, *target_step, request.history_handling, request.user_involvement);
}

void CanonicalTraversable::run_direct_history_operation(HistoryOperation& operation)
{
    operation.parameters.visit(
        [&](Web::ReloadHistoryOperationParameters const& parameters) {
            auto current_step = m_session_history.current_step();
            auto navigable = find(parameters.navigable_id);
            auto* target_entry = current_step.has_value() && navigable.has_value()
                ? m_session_history.get_the_target_history_entry(*navigable, *current_step)
                : nullptr;
            if (!target_entry) {
                finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
                return;
            }
            target_entry->document_state->reload_pending = true;
            session_history_changed();
            apply_the_reload_history_step(operation, parameters.user_involvement);
        },
        [&](Web::TraverseByDeltaHistoryOperationParameters const& request) {
            traverse_the_history_by_a_delta_at_queue_position(operation, request);
        },
        [&](Web::TraverseToStepHistoryOperationParameters const& parameters) {
            apply_the_traverse_history_step(operation, parameters.target_step, {}, {}, parameters.user_involvement);
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
            auto current_step = m_session_history.current_step();
            if (!parent_navigable.has_value() || !child_navigable.has_value() || !current_step.has_value() || !operation.initiating_page) {
                finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
                return;
            }

            // A navigable created by a document repopulated for its entry navigates to the entry its nested history
            // kept, rather than starting from about:blank.
            if (auto* target_entry = m_session_history.get_the_target_history_entry(*child_navigable, *current_step)) {
                auto uuid = Web::Crypto::generate_random_uuid();
                auto navigation_id = Utf16String::from_ascii_without_validation(uuid.bytes());
                auto ongoing_navigation = CanonicalNavigation {
                    .url = target_entry->url,
                    .navigation_id = navigation_id,
                    .sequence_number = next_sequence_number(),
                    .has_started = true,
                    .phase = CanonicalNavigation::Phase::Populating,
                    .reconstructed_entry = target_entry,
                };
                child_navigable->set_ongoing_navigation(move(ongoing_navigation));
                child_navigable->set_navigation_host(*operation.initiating_page);
                auto reconstructed_child_navigation = Web::ReconstructedChildNavigation {
                    .target_entry = target_entry->descriptor(),
                    .navigation_id = move(navigation_id),
                };
                operation.initiating_page->async_reconstruct_child_navigable_history(parameters.navigable_id, move(reconstructed_child_navigation));
                finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::Applied, {});
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
                finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
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
            // NB: The final unload-and-destroy task is dispatched to the requesting process once every descendant
            //     subtree has unloaded. Completing the operation afterwards is ordered behind that task's message.
            unload_a_document_and_its_descendants(id(), page_hosting(*this), Web::HTML::ChildNavigableDestruction::No, [this, operation_id = operation.operation_id](UnloadedInItsHost) {
                auto* operation = find_history_operation(operation_id);
                if (!operation)
                    return;
                if (operation->initiating_page)
                    operation->initiating_page->async_run_traversable_close_unload_task(operation_id);
                finish_history_operation(operation_id, Web::HTML::HistoryStepResult::Applied, {});
            });
        },
        [&](Web::FlushSessionHistoryTraversalQueueOperationParameters const&) {
            // Flush is a queue barrier; completing at the queue position is the whole operation.
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::Applied, {});
        },
        [&](auto const&) {
            VERIFY_NOT_REACHED();
        });
}

void CanonicalTraversable::start_history_operation(HistoryOperation& operation, NonnullRefPtr<Core::Promise<Empty>>)
{
    if (!operation.initiating_page)
        operation.initiating_page = page_hosting(*this);

    if (operation.is_browser_traversal()) {
        if (!operation.initiating_page) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
            return;
        }
        VERIFY(operation.parameters.has<Web::TraverseToStepHistoryOperationParameters>());
        auto const& parameters = operation.parameters.get<Web::TraverseToStepHistoryOperationParameters>();
        apply_history_step(operation, parameters.target_step, operation.check_for_cancelation, {}, parameters.user_involvement, Web::Bindings::NavigationType::Traverse);
        return;
    }

    if (operation.initiating_page)
        add_history_operation_completion_endpoint(operation, *operation.initiating_page);

    if (history_operation_is_direct(operation.parameters)) {
        run_direct_history_operation(operation);
        return;
    }

    if (!operation.initiating_page) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
        return;
    }

    if (operation.parameters.has<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>()) {
        operation.owns_navigation_transaction = navigation_transaction_matches(operation, *operation.initiating_page);
        if (!operation.owns_navigation_transaction) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::Applied, {});
            return;
        }
    }

    operation.initiating_page->async_history_operation_started(operation.operation_id);
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#finalize-a-cross-document-navigation
void CanonicalTraversable::finalize_a_cross_document_navigation(HistoryOperation& operation)
{
    auto const& parameters = operation.parameters.get<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>();
    auto navigable = find(parameters.navigable_id);
    if (!navigable.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // NB: historyEntry's document state is the navigation's documentState, which holds the document it populated.
    CanonicalSessionHistoryEntry::DocumentStates document_states;
    if (auto document_state = navigable->populating_document_state(); document_state && document_state->id == parameters.history_entry.document_state.id)
        document_states.set(document_state->id, *document_state);
    auto history_entry_or_error = CanonicalSessionHistoryEntry::create_from_descriptor(Web::HTML::create_session_history_entry_descriptor(parameters.history_entry, 0), document_states, CanonicalSessionHistoryEntry::UpdateDocumentState::Yes);
    if (history_entry_or_error.is_error()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }
    auto history_entry = history_entry_or_error.release_value();

    // NB: A javascript: URL navigation creates its document in the process running it, in navigable's active browsing
    //     context, and reports it with these steps. It is populated for historyEntry's document state here.
    if (!parameters.navigation_id.has_value()) {
        if (!history_entry->document_state->origin.has_value()) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }
        auto const& origin = *history_entry->document_state->origin;
        NavigationLoader::ResponseDocument response_document {
            .is_inline_content = false,
            .coop_enforcement_result = { .url = history_entry->url, .origin = origin, .opener_policy = {} },
            .response_url = history_entry->url,
            .request_current_url = {},
            .origin = origin,
        };
        navigable->populate_document(history_entry->document_state, navigable->create_and_initialize_a_document(response_document));
    }

    // 1. Assert: this is running on navigable's traversable navigable's session history traversal queue.
    VERIFY(operation.queue_promise);
    VERIFY(!operation.queue_promise->is_resolved() && !operation.queue_promise->is_rejected());
    VERIFY(&navigable->top_level_traversable() == this);

    // 2. Set navigable's is delaying load events to false.
    // NB: The process hosting navigable performed this step when the operation reached its queue position, before
    //     answering that it started.

    // 3. If historyEntry's document is null, then return.
    // NB: The document populated for historyEntry's document state waits on navigable until historyEntry is activated.
    RefPtr<CanonicalDocument> document = navigable->populating_document_state().ptr() == history_entry->document_state.ptr() ? navigable->pending_document() : nullptr;
    if (!document) {
        if (navigable->is_top_level_traversable()) {
            if (auto view = this->view(); view.has_value())
                view->did_cancel_loading(parameters.navigation_id);
        }
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::Applied, {});
        return;
    }

    // 4. If all of the following are true:
    //    - navigable's parent is null;
    //    - historyEntry's document's browsing context is not an auxiliary browsing context whose opener browsing
    //      context is non-null; and
    //    - historyEntry's document's origin is not navigable's active document's origin,
    //    then set historyEntry's document state's navigable target name to the empty string.
    auto& browsing_context = document->browsing_context();
    if (navigable->parent() == nullptr
        && !(browsing_context.is_auxiliary() && browsing_context.opener_browsing_context())
        && document->origin() != navigable->active_document().origin()) {
        history_entry->document_state->navigable_target_name = {};
    }

    auto current_step = m_session_history.current_step();
    if (!current_step.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 5. Let entryToReplace be navigable's active session history entry if historyHandling is "replace", otherwise null.
    RefPtr<CanonicalSessionHistoryEntry> entry_to_replace;
    if (parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Replace) {
        entry_to_replace = navigable->active_session_history_entry();

        // AD-HOC: A navigation reconstructing the navigable's history replaces the entry whose document it populates.
        if (auto const& ongoing_navigation = navigable->ongoing_navigation(); ongoing_navigation.has_value() && ongoing_navigation->reconstructed_entry && ongoing_navigation->navigation_id == parameters.navigation_id)
            entry_to_replace = ongoing_navigation->reconstructed_entry;

        // AD-HOC: An active entry that is not among the navigable's session history entries, as that of a document a
        //         crashed process destroyed, is not one to replace. The current entry, whose document that was, is.
        auto entries = m_session_history.get_session_history_entries(*navigable);
        auto is_among_entries = [&](RefPtr<CanonicalSessionHistoryEntry> const& candidate) {
            return candidate && entries.has_value() && any_of(*entries, [&](auto const& entry) { return entry == candidate; });
        };
        if (entry_to_replace && !is_among_entries(entry_to_replace))
            entry_to_replace = is_among_entries(navigable->current_session_history_entry()) ? navigable->current_session_history_entry() : nullptr;
    }
    if (parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Replace && !entry_to_replace) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 6. Let traversable be navigable's traversable navigable.
    // NB: This CanonicalTraversable is navigable's traversable navigable.

    // 7. Let targetStep be null.
    Optional<i32> target_step;

    // 8. Let targetEntries be the result of getting session history entries for navigable.
    auto target_entries = m_session_history.get_session_history_entries(*navigable);
    if (!target_entries.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 9. If entryToReplace is null:
    if (!entry_to_replace) {
        // 1. Clear the forward session history of traversable.
        // 2. Set targetStep to traversable's current session history step + 1.
        // 3. Set historyEntry's step to targetStep.
        // 4. Append historyEntry to targetEntries.
        target_step = m_session_history.push_session_history_entry(*navigable, move(history_entry));
        if (!target_step.has_value()) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }
    }
    // Otherwise:
    else {
        // 1. Replace entryToReplace with historyEntry in targetEntries.
        // 2. Set historyEntry's step to entryToReplace's step.
        if (!m_session_history.replace_session_history_entry(*navigable, *entry_to_replace, history_entry)) {
            finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
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
        target_step = *current_step;
    }

    // 10. Apply the push/replace history step targetStep to traversable given historyHandling and userInvolvement.
    apply_the_push_or_replace_history_step(operation, *target_step, parameters.history_handling, parameters.user_involvement);
}

void CanonicalTraversable::did_receive_history_operation_ready(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HistoryOperationReadyResult result)
{
    auto* operation = find_history_operation(operation_id);
    if (!operation || operation->algorithm)
        return;
    if (history_operation_is_direct(operation->parameters))
        return;
    if (!operation->was_initiated_by(source_page))
        return;
    if (!navigation_transaction_matches(*operation, source_page)) {
        finish_history_operation(operation_id, Web::HTML::HistoryStepResult::Applied, {});
        return;
    }

    if (result.has<Web::HTML::HistoryStepResult>()) {
        finish_history_operation(operation_id, result.get<Web::HTML::HistoryStepResult>(), {});
        return;
    }

    VERIFY(!operation->is_browser_traversal());
    auto const& request = operation->parameters;
    auto result_matches_request = request.visit(
        [&](Web::FinalizeSameDocumentNavigationHistoryOperationParameters const&) { return false; },
        [&](Web::CloseTopLevelTraversableHistoryOperationParameters const&) { return false; },
        [&](Web::FlushSessionHistoryTraversalQueueOperationParameters const&) { return false; },
        [&](auto const&) { return result.has<Empty>(); });
    if (!result_matches_request) {
        finish_history_operation(operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    request.visit(
        [&](Web::FinalizeCrossDocumentNavigationHistoryOperationParameters const&) {
            finalize_a_cross_document_navigation(*operation);
        },
        [&](Web::ReloadHistoryOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::TraverseByDeltaHistoryOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::TraverseToStepHistoryOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::NavigationAPITraverseHistoryOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::ResumeTraverseHistoryOperationParameters const& parameters) {
            resume_applying_the_traverse_history_step(*operation, parameters.target_step, parameters.user_involvement);
        },
        [&](Web::NavigableCreationHistoryOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::NavigableDestructionHistoryOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::FinalizeSameDocumentNavigationHistoryOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::CloseTopLevelTraversableHistoryOperationParameters const&) {
            // Close runs entirely in the requesting process at this queue position and must complete with proceed=false.
            VERIFY_NOT_REACHED();
        },
        [&](Web::FlushSessionHistoryTraversalQueueOperationParameters const&) {
            VERIFY_NOT_REACHED();
        });
}

void CanonicalTraversable::finish_history_operation(Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult result, Optional<i32> committed_step)
{
    auto operation = m_history_operations.take(operation_id);
    if (!operation.has_value())
        return;
    auto& taken_operation = **operation;

    // A changing job still pending when its operation finishes never activates the document it populated.
    for (auto const& [navigable_id, pending_job] : taken_operation.pending_changing_jobs) {
        if (auto navigable = find(navigable_id); navigable.has_value())
            navigable->abandon_document_populated_for(*pending_job->job.target_entry->document_state);
    }
    if (taken_operation.owns_navigation_transaction) {
        auto const& parameters = taken_operation.parameters.get<Web::FinalizeCrossDocumentNavigationHistoryOperationParameters>();
        if (auto navigable = find(parameters.navigable_id); navigable.has_value())
            navigable->did_finish_navigation_transaction(parameters.navigation_id, result);
    }

    if (committed_step.has_value()) {
        if (auto view = this->view(); view.has_value()) {
            if (auto* current_entry = m_session_history.current_entry())
                view->set_url(current_entry->url);
        }
    }

    for (auto& endpoint : taken_operation.completion_endpoints)
        endpoint->async_complete_history_operation(
            operation_id, result, committed_step,
            m_session_history.size());

    // All apply-driven mutations have settled at operation completion.
    session_history_changed();

    if (taken_operation.on_complete)
        taken_operation.on_complete(result, committed_step);

    if (taken_operation.is_browser_traversal()) {
        auto callback = move(taken_operation.on_browser_traversal_ready);
        if (callback)
            callback();
        if (auto view = this->view(); view.has_value())
            view->did_finish_history_traversal(operation_id, result);
    }

    // NB: Resolving the queue promise can synchronously start the next queued operation.
    if (taken_operation.queue_promise)
        taken_operation.queue_promise->resolve({});

    // The navigations that waited for the operation's traversal to be over begin, unless another traversal began.
    Vector<Web::HTML::CrossProcessId> navigables_with_waiting_navigation;
    for_each_in_inclusive_subtree([&](CanonicalNavigable const& navigable) {
        if (navigable.has_navigation_waiting_for_traversal())
            navigables_with_waiting_navigation.append(navigable.id());
        return IterationDecision::Continue;
    });
    for (auto navigable_id : navigables_with_waiting_navigation) {
        if (auto navigable = find(navigable_id); navigable.has_value())
            navigable->begin_navigation_waiting_for_traversal();
    }

    if (auto view = this->view(); view.has_value())
        view->run_webdriver_commands_waiting_for_a_document({});

    // The completion callback that brought us here can be running inside the algorithm object; destroy the
    // operation only once the stack has unwound.
    Core::deferred_invoke([operation = operation.release_value()] { });
}

void CanonicalTraversable::abandon_history_operations()
{
    while (!m_history_operations.is_empty()) {
        auto operation_id = m_history_operations.begin()->key;
        finish_history_operation(operation_id, Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
    }
}

void CanonicalTraversable::did_receive_history_step_unload_cancelation_result(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryStepResult result, Web::HTML::UnloadPromptShown unload_prompt_shown)
{
    auto* operation = find_history_operation(operation_id);
    if (!operation)
        return;
    if (operation->unload_cancelation_endpoint != source_page) {
        return;
    }
    operation->unload_cancelation_endpoint.clear();
    if (!operation->pending_unload_cancelation)
        return;

    // This result means browser UI Back stopped an uncommitted navigation before the normal unload
    // cancellation checks began. Complete the phase without checking the other document hosts.
    if (result == Web::HTML::HistoryStepResult::CanceledPendingNavigation) {
        if (operation->is_browser_traversal())
            result = Web::HTML::HistoryStepResult::Applied;
        complete_unload_cancelation(*operation, result);
        return;
    }

    if (result != Web::HTML::HistoryStepResult::Applied) {
        complete_unload_cancelation(*operation, result);
        return;
    }

    operation->beforeunload_prompt_shown = unload_prompt_shown;
    dispatch_next_beforeunload_group(*operation);
}

void CanonicalTraversable::dispatch_next_beforeunload_group(HistoryOperation& operation)
{
    while (!operation.pending_beforeunload_groups.is_empty()) {
        auto group = operation.pending_beforeunload_groups.take_first();
        // A missing endpoint's documents are already gone. They contribute "proceed".
        if (!group.endpoint->is_open())
            continue;

        operation.dispatched_beforeunload_endpoint = group.endpoint;
        group.endpoint->async_run_beforeunload_check(operation.operation_id, move(group.navigable_ids), operation.beforeunload_prompt_shown);
        return;
    }

    complete_unload_cancelation(operation, Web::HTML::HistoryStepResult::Applied);
}

void CanonicalTraversable::complete_unload_cancelation(HistoryOperation& operation, Web::HTML::HistoryStepResult result)
{
    operation.unload_cancelation_endpoint.clear();
    operation.pending_beforeunload_groups.clear();
    operation.dispatched_beforeunload_endpoint.clear();
    if (auto pending = move(operation.pending_unload_cancelation))
        pending(result);
}

void CanonicalTraversable::check_if_unloading_is_canceled(Vector<Web::HTML::CrossProcessId> navigable_ids, RefPtr<WebContentPage> skipped_endpoint, Web::HTML::UnloadPromptShown unload_prompt_shown, Function<void(Web::HTML::HistoryStepResult, Web::HTML::UnloadPromptShown)> on_complete)
{
    PendingBeforeunloadCheck check;
    check.unload_prompt_shown = unload_prompt_shown;
    for (auto navigable_id : navigable_ids) {
        auto navigable = find(navigable_id);
        if (!navigable.has_value())
            continue;
        auto endpoint = page_hosting(*navigable);
        if (!endpoint)
            continue;
        if (skipped_endpoint == endpoint)
            continue;
        auto group = check.groups.find_if([&](auto const& group) {
            return group.endpoint == endpoint;
        });
        if (group == check.groups.end())
            check.groups.append({ endpoint.release_nonnull(), { navigable_id } });
        else
            group->navigable_ids.append(navigable_id);
    }

    if (check.groups.is_empty()) {
        on_complete(Web::HTML::HistoryStepResult::Applied, unload_prompt_shown);
        return;
    }

    check.on_complete = move(on_complete);
    auto check_id = Application::the().allocate_ui_process_cross_process_id();
    m_pending_beforeunload_checks.set(check_id, move(check));
    dispatch_next_beforeunload_group(check_id);
}

void CanonicalTraversable::dispatch_next_beforeunload_group(Web::HTML::CrossProcessId check_id)
{
    auto check = m_pending_beforeunload_checks.find(check_id);
    if (check == m_pending_beforeunload_checks.end())
        return;
    while (!check->value.groups.is_empty()) {
        auto group = check->value.groups.take_first();
        // A missing endpoint's documents are already gone. They contribute "proceed".
        if (!group.endpoint->is_open())
            continue;
        check->value.dispatched_endpoint = group.endpoint;
        group.endpoint->async_run_beforeunload_check(check_id, move(group.navigable_ids), check->value.unload_prompt_shown);
        return;
    }
    auto completed_check = m_pending_beforeunload_checks.take(check_id);
    completed_check->on_complete(Web::HTML::HistoryStepResult::Applied, completed_check->unload_prompt_shown);
}

void CanonicalTraversable::did_receive_beforeunload_check_result(WebContentPage& source_page, Web::HTML::CrossProcessId check_id, Web::HTML::HistoryStepResult result, Web::HTML::UnloadPromptShown unload_prompt_shown)
{
    // A check of its own, or one of a history step's unload cancelation job.
    if (auto check = m_pending_beforeunload_checks.find(check_id); check != m_pending_beforeunload_checks.end()) {
        if (check->value.dispatched_endpoint != source_page) {
            return;
        }
        check->value.dispatched_endpoint.clear();
        if (result != Web::HTML::HistoryStepResult::Applied) {
            auto completed_check = m_pending_beforeunload_checks.take(check_id);
            completed_check->on_complete(result, unload_prompt_shown);
            return;
        }
        check->value.unload_prompt_shown = unload_prompt_shown;
        dispatch_next_beforeunload_group(check_id);
        return;
    }

    auto operation_id = check_id;
    auto* operation = find_history_operation(operation_id);
    if (!operation)
        return;
    if (operation->dispatched_beforeunload_endpoint != source_page) {
        return;
    }
    operation->dispatched_beforeunload_endpoint.clear();

    if (result != Web::HTML::HistoryStepResult::Applied) {
        complete_unload_cancelation(*operation, result);
        return;
    }

    operation->beforeunload_prompt_shown = unload_prompt_shown;
    dispatch_next_beforeunload_group(*operation);
}

void CanonicalTraversable::did_receive_changing_navigable_history_job_ready(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition, Web::HTML::UnloadDisplayedDocument unload_displayed_document)
{
    if (auto* operation = find_history_operation(operation_id)) {
        auto pending_job = operation->pending_changing_jobs.get(navigable_id);
        if (!pending_job.has_value())
            return;
        // NB: A job whose navigable is gone completes with what its page reports.
        if (auto endpoint = changing_job_endpoint(*operation, navigable_id); endpoint && endpoint != source_page)
            return;

        if (!navigation_transaction_matches(*operation, source_page, navigable_id))
            disposition = Web::HTML::ChangingNavigableHistoryStepJobDisposition::Stale;

        if (disposition != Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready && pending_job.value()->population_loader)
            pending_job.value()->population_loader->reclaim_response_body_after_failed_handoff();

        if (disposition == Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready)
            pending_job.value()->unload_displayed_document = unload_displayed_document;

        switch (pending_job.value()->phase) {
        case HistoryOperation::PendingChangingJob::Phase::Dispatched:
            break;
        case HistoryOperation::PendingChangingJob::Phase::ReadyReported:
        case HistoryOperation::PendingChangingJob::Phase::ContinuationDispatched:
            return;
        }

        auto on_complete = move(pending_job.value()->on_complete);
        if (disposition == Web::HTML::ChangingNavigableHistoryStepJobDisposition::Ready) {
            pending_job.value()->phase = HistoryOperation::PendingChangingJob::Phase::ReadyReported;
            if (auto navigable = find(navigable_id); navigable.has_value())
                navigable->claim_document_populated_for_ongoing_navigation(*pending_job.value()->job.target_entry->document_state);
            on_complete(disposition);
            return;
        }

        if (auto navigable = find(navigable_id); navigable.has_value())
            navigable->abandon_document_populated_for(*pending_job.value()->job.target_entry->document_state);
        operation->pending_changing_jobs.remove(navigable_id);
        on_complete(disposition);
    }
}

void CanonicalTraversable::did_receive_changing_navigable_continuation_applied(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::HostedNavigableState> activated_navigable_state, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state)
{
    if (auto* operation = find_history_operation(operation_id)) {
        // NB: A job whose navigable is gone completes with what its page reports.
        if (auto endpoint = changing_job_endpoint(*operation, navigable_id); endpoint && endpoint != source_page)
            return;
        auto pending_job = operation->pending_changing_jobs.take(navigable_id);
        if (!pending_job.has_value())
            return;
        if (activated_navigable_state.has_value())
            did_activate_history_entry(*operation, navigable_id, source_page, *pending_job.value()->job.target_entry, pending_job.value()->did_populate_document, activated_navigable_state.release_value());
        else if (auto navigable = find(navigable_id); navigable.has_value())
            navigable->abandon_document_populated_for(*pending_job.value()->job.target_entry->document_state);
        if (previous_entry_persisted_state.has_value()) {
            auto navigable = find(navigable_id);
            if (navigable.has_value())
                update_session_history_entry_persisted_state(*navigable, *previous_entry_persisted_state);
        }
        // A replacement document's creation operation also applies a top-level continuation, but it runs before
        // the document has accepted the UI-owned history state. Only the browser traversal itself reaches the
        // observable top-level completion point here.
        if (navigable_id == id() && operation->is_browser_traversal()) {
            if (auto view = this->view(); view.has_value())
                view->did_apply_top_level_history_traversal_step(operation_id);
        }
        if (pending_job.value()->on_continuation_complete)
            pending_job.value()->on_continuation_complete();
    }

    if (auto view = this->view(); view.has_value())
        view->run_webdriver_commands_waiting_for_a_document({});
}

void CanonicalTraversable::did_receive_nonchanging_navigable_history_state_updated(WebContentPage& source_page, Web::HTML::CrossProcessId operation_id, Web::HTML::CrossProcessId navigable_id)
{
    if (auto* operation = find_history_operation(operation_id)) {
        auto pending = operation->pending_nonchanging_updates.get(navigable_id);
        if (!pending.has_value() || pending->endpoint != source_page)
            return;
        auto taken_pending = operation->pending_nonchanging_updates.take(navigable_id);
        taken_pending->on_complete();
    }
}

}
