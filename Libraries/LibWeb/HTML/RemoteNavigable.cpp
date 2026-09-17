/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/PreparedNavigationDescriptor.h>
#include <LibWeb/HTML/RemoteNavigable.h>
#include <LibWeb/HTML/RemoteWindow.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(RemoteNavigable);

HashTable<GC::RawRef<RemoteNavigable>>& all_remote_navigables()
{
    static NeverDestroyed<HashTable<GC::RawRef<RemoteNavigable>>> set;
    return *set;
}

GC::Ptr<RemoteNavigable> remote_navigable_with_id(Page const& page, CrossProcessId id)
{
    for (auto& navigable : all_remote_navigables()) {
        if (navigable->id() == id && &navigable->page() == &page)
            return navigable;
    }
    return nullptr;
}

GC::Ref<RemoteNavigable> RemoteNavigable::create(GC::Ref<Page> page, CrossProcessId id, GC::Ptr<Navigable> parent, ReplicatedNavigableState replicated_state)
{
    return GC::Heap::the().allocate<RemoteNavigable>(page, id, parent, move(replicated_state));
}

RemoteNavigable::RemoteNavigable(GC::Ref<Page> page, CrossProcessId id, GC::Ptr<Navigable> parent, ReplicatedNavigableState replicated_state)
    : Navigable(page)
    , m_replicated_state(move(replicated_state))
{
    set_id(id);
    set_parent(parent);
    all_remote_navigables().set(*this);
}

RemoteNavigable::~RemoteNavigable() = default;

void RemoteNavigable::finalize()
{
    all_remote_navigables().remove(*this);
    Base::finalize();
}

void RemoteNavigable::remove_from_all_remote_navigables()
{
    all_remote_navigables().remove(*this);
}

void RemoteNavigable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_children);
    visitor.visit(m_window_proxy);
    visitor.visit(m_active_window);
    visitor.visit(m_provisional_navigable);
}

void RemoteNavigable::append_child(GC::Ref<Navigable> child)
{
    VERIFY(child->parent().ptr() == this);
    m_children.append(child);
}

void RemoteNavigable::remove_child(Navigable& child)
{
    auto removed = m_children.remove_first_matching([&](auto const& existing_child) { return existing_child.ptr() == &child; });
    VERIFY(removed);
}

void RemoteNavigable::replace_child(Navigable& child, GC::Ref<Navigable> replacement)
{
    VERIFY(replacement->id() == child.id());
    VERIFY(replacement->parent().ptr() == this);
    auto index = m_children.find_first_index_if([&](auto const& existing_child) { return existing_child.ptr() == &child; });
    VERIFY(index.has_value());
    m_children[*index] = replacement;
}

GC::Ptr<WindowProxy> RemoteNavigable::active_window_proxy()
{
    // The WindowProxy of a navigable hosted by another process lives in the realm of a document this page hosts and
    // answers every access on the cross-origin path. Only a script of such a document can ask for it.
    if (!m_window_proxy) {
        auto local_roots = page().local_roots();
        VERIFY(!local_roots.is_empty());
        auto window = local_roots.first()->active_window();
        VERIFY(window);
        m_window_proxy = WindowProxy::create(relevant_realm(*window));
        m_window_proxy->set_window(active_window());
    }
    return m_window_proxy;
}

GC::Ptr<WindowProxy> RemoteNavigable::active_browsing_context_opener_window_proxy() const
{
    // NB: The active browsing context is in the process hosting this navigable, which replicates its opener browsing
    //     context as the navigable that browsing context is active in. The opener can be in another tab, which this
    //     process holds in a page of its own.
    auto const& opener_navigable_id = m_replicated_state.opener_navigable_id;
    if (!opener_navigable_id.has_value())
        return nullptr;
    for (auto& local_navigable : all_local_navigables()) {
        if (local_navigable->id() == *opener_navigable_id && !local_navigable->has_been_destroyed())
            return local_navigable->active_window_proxy();
    }
    for (auto& remote_navigable : all_remote_navigables()) {
        if (remote_navigable->id() == *opener_navigable_id && !remote_navigable->has_been_destroyed())
            return remote_navigable->active_window_proxy();
    }
    return nullptr;
}

GC::Ref<RemoteWindow> RemoteNavigable::active_window()
{
    if (!m_active_window)
        m_active_window = RemoteWindow::create(*this);
    return *m_active_window;
}

Vector<GC::Root<Navigable>> RemoteNavigable::document_tree_child_navigables()
{
    // NB: The children are in the order the UI process learned of their creation, not in tree order.
    Vector<GC::Root<Navigable>> navigables;
    for (auto& child : m_children) {
        if (child->container_is_in_document_tree())
            navigables.append(*child);
    }
    return navigables;
}

Vector<GC::Root<Navigable>> RemoteNavigable::active_document_inclusive_descendant_navigables()
{
    // The navigable's subtree as the UI process replicates it: itself, then each child's inclusive descendants.
    Vector<GC::Root<Navigable>> navigables;
    navigables.append(*this);
    for (auto& child : m_children)
        navigables.extend(child->active_document_inclusive_descendant_navigables());
    return navigables;
}

void RemoteNavigable::set_replicated_state(ReplicatedNavigableState state)
{
    auto was_delaying_the_load_event_of_its_container = m_replicated_state.delays_the_load_event_of_its_container;
    auto previous_compositor_context_id = m_replicated_state.compositor_context_id;
    m_replicated_state = move(state);

    // The documents of the navigable's children hosted here composite into the context its document is painted
    // through, wherever that document is hosted.
    if (previous_compositor_context_id != m_replicated_state.compositor_context_id) {
        for (auto& child : m_children) {
            if (auto* local_child = as_if<LocalNavigable>(*child))
                local_child->set_parent_compositor_context(m_replicated_state.compositor_context_id);
        }
    }

    // A container here hears from the document through the replicated state what a document here tells it directly.
    auto container = this->container();
    if (!container)
        return;
    if (previous_compositor_context_id != m_replicated_state.compositor_context_id) {
        if (auto* layout_node = container->unsafe_layout_node())
            layout_node->refresh_dom_paint_facts();
        container->set_needs_repaint();
    }
    if (was_delaying_the_load_event_of_its_container && !m_replicated_state.delays_the_load_event_of_its_container)
        container->document().schedule_html_parser_end_check();
}

ReplicatedContainerState RemoteNavigable::container_state() const
{
    if (auto container = this->container())
        return container->replicated_container_state();
    return m_replicated_state.container;
}

bool RemoteNavigable::has_session_history_entry_and_ready_for_navigation() const
{
    return m_replicated_state.has_session_history_entry_and_ready_for_navigation;
}

bool RemoteNavigable::delays_the_load_event_of_its_container() const
{
    // A destroyed navigable's document is on its way out, as a local one's is once its delaying flag is cleared.
    return !has_been_destroyed() && m_replicated_state.delays_the_load_event_of_its_container;
}

// https://html.spec.whatwg.org/multipage/interaction.html#currently-focused-area-of-a-top-level-traversable
GC::Ptr<DOM::Node> RemoteNavigable::currently_focused_area()
{
    // 1. If traversable does not have system focus, then return null.
    // 2. Let candidate be traversable's active document.
    // 3. While candidate's focused area is a navigable container with a non-null content navigable: set candidate to
    //    the active document of that navigable container's content navigable.
    // 4. If candidate's focused area is non-null, set candidate to candidate's focused area.
    // 5. Return candidate.
    // NB: The active document is in the process hosting this navigable. The tab's focused navigable shows where the
    //     walk goes below it.
    return currently_focused_area_shown_by_focused_navigable();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
WebIDL::ExceptionOr<void> RemoteNavigable::continue_navigation_in_active_document_agent(PreparedNavigation navigation)
{
    // 8. If the surrounding agent is equal to navigable's active document's relevant agent, then continue these
    //    steps. Otherwise, queue a global task on the navigation and traversal task source given navigable's active
    //    window to continue these steps.
    // NB: The active window lives in the process hosting the active document, so the task is a request to the UI
    //     process, which forwards it to that process.
    page().client().request_navigation_of_remote_navigable(*this, create_prepared_navigation_descriptor(navigation));
    return {};
}

}
