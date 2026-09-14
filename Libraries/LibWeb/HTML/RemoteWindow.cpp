/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/Location.h>
#include <LibWeb/HTML/RemoteNavigable.h>
#include <LibWeb/HTML/RemoteWindow.h>
#include <LibWeb/HTML/WindowProxy.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(RemoteWindow);

GC::Ref<RemoteWindow> RemoteWindow::create(RemoteNavigable& navigable)
{
    return GC::Heap::the().allocate<RemoteWindow>(navigable);
}

RemoteWindow::RemoteWindow(RemoteNavigable& navigable)
    : m_navigable(navigable)
{
}

RemoteWindow::~RemoteWindow() = default;

void RemoteWindow::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_navigable);
    visitor.visit(m_location);
    visitor.visit(m_cross_origin_property_descriptor_map);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#window-navigable
GC::Ptr<RemoteNavigable> RemoteWindow::navigable() const
{
    if (m_navigable->has_been_destroyed())
        return nullptr;
    return m_navigable;
}

URL::Origin const& RemoteWindow::origin() const
{
    return m_navigable->replicated_state().active_document_origin;
}

// https://html.spec.whatwg.org/multipage/window-object.html#dom-window
GC::Ref<WindowProxy> RemoteWindow::window() const
{
    // The window, frames, and self getter steps are to return this's relevant realm.[[GlobalEnv]].[[GlobalThisValue]].
    return *m_navigable->active_window_proxy();
}

// https://html.spec.whatwg.org/multipage/window-object.html#dom-self
GC::Ref<WindowProxy> RemoteWindow::self() const
{
    // The window, frames, and self getter steps are to return this's relevant realm.[[GlobalEnv]].[[GlobalThisValue]].
    return window();
}

// https://html.spec.whatwg.org/multipage/window-object.html#dom-frames
GC::Ref<WindowProxy> RemoteWindow::frames() const
{
    // The window, frames, and self getter steps are to return this's relevant realm.[[GlobalEnv]].[[GlobalThisValue]].
    return window();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-location
GC::Ref<Location> RemoteWindow::location()
{
    // The Window object's location getter steps are to return this's Location object.
    if (!m_location)
        m_location = GC::Heap::the().allocate<Location>(*this);
    return *m_location;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-window-closed
bool RemoteWindow::closed() const
{
    // The closed getter steps are to return true if this's navigable is null or its is closing is true; otherwise
    // false.
    if (auto navigable = this->navigable(); !navigable || navigable->is_closing())
        return true;

    return false;
}

// https://html.spec.whatwg.org/multipage/window-object.html#dom-length
u32 RemoteWindow::length()
{
    // The length getter steps are to return this's associated Document's document-tree child navigables's size.
    return document_tree_child_navigables().size();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-top
GC::Ptr<WindowProxy const> RemoteWindow::top() const
{
    // 1. If this's navigable is null, then return null.
    auto navigable = this->navigable();
    if (!navigable)
        return {};

    // 2. Return this's navigable's top-level traversable's active WindowProxy.
    return navigable->top_level_traversable()->active_window_proxy();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-opener
GC::Ptr<WindowProxy const> RemoteWindow::opener() const
{
    // 1. Let current be this's browsing context.
    // 2. If current is null, then return null.
    // 3. If current's opener browsing context is null, then return null.
    // NB: The browsing context lives in the process hosting the Window. Only a top-level one has an opener.
    auto navigable = this->navigable();
    if (!navigable || !navigable->is_top_level_traversable())
        return {};

    // 4. Return current's opener browsing context's WindowProxy object.
    // FIXME: The opener of a traversable hosted by another process is canonical in the UI process.
    TODO();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-parent
GC::Ptr<WindowProxy const> RemoteWindow::parent() const
{
    // 1. Let navigable be this's navigable.
    GC::Ptr<Navigable> navigable = this->navigable();

    // 2. If navigable is null, then return null.
    if (!navigable)
        return {};

    // 3. If navigable's parent is not null, then set navigable to navigable's parent.
    if (auto parent = navigable->parent())
        navigable = parent;

    // 4. Return navigable's active WindowProxy.
    return navigable->active_window_proxy();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-window-close
void RemoteWindow::close()
{
    // 1. Let thisTraversable be this's navigable.
    auto traversable = navigable();

    // 2. If thisTraversable is not a top-level traversable, then return.
    if (!traversable || !traversable->is_top_level_traversable())
        return;

    // 3. If thisTraversable's is closing is true, then return.
    if (traversable->is_closing())
        return;

    // FIXME: Closing a traversable hosted by another process is a request to the UI process, which knows whether it
    //        is script-closable.
    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-window-focus
void RemoteWindow::focus()
{
    // 1. Let current be this's navigable.
    auto current = navigable();

    // 2. If current is null, then return.
    if (!current)
        return;

    // FIXME: Focusing a navigable hosted by another process is a request to the UI process.
    TODO();
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-window-blur
void RemoteWindow::blur()
{
    // The Window blur() method steps are to do nothing.
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-window-postmessage
WebIDL::ExceptionOr<void> RemoteWindow::post_message(JS::Realm& realm, JS::Value message, Utf16String const& target_origin, GC::RootVector<GC::Ref<JS::Object>> const& transfer)
{
    // The Window interface's postMessage(message, targetOrigin, transfer) method steps are to run the window post message
    // steps given this, message, and «[ "targetOrigin" → targetOrigin, "transfer" → transfer ]».
    return post_message(realm, message, Window::PostMessageOptions { { .transfer = transfer }, target_origin });
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-window-postmessage-options
WebIDL::ExceptionOr<void> RemoteWindow::post_message(JS::Realm&, JS::Value, Window::PostMessageOptions const&)
{
    // The Window interface's postMessage(message, options) method steps are to run the window post message steps given
    // this, message, and options.
    // FIXME: A message posted to a Window hosted by another process is a request to the UI process.
    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#document-tree-child-navigables
Vector<GC::Root<Navigable>> RemoteWindow::document_tree_child_navigables()
{
    // 1. If document's node navigable is null, then return the empty list.
    auto navigable = this->navigable();
    if (!navigable)
        return {};

    // 2-5.
    // NB: The document's navigable containers are in the process hosting it, which replicates their content
    //     navigables to the navigable here.
    return navigable->document_tree_child_navigables();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#document-tree-child-navigable-target-name-property-set
OrderedHashMap<Utf16FlyString, GC::Ref<Navigable>> RemoteWindow::document_tree_child_navigable_target_name_property_set()
{
    // The document-tree child navigable target name property set of a Window object window is the return value of running these steps:

    // 1. Let children be the document-tree child navigables of window's associated Document.
    auto children = document_tree_child_navigables();

    // 2. Let firstNamedChildren be an empty ordered set.
    OrderedHashMap<Utf16FlyString, GC::Ref<Navigable>> first_named_children;

    // 3. For each navigable of children:
    for (auto const& navigable : children) {
        // 1. Let name be navigable's target name.
        // 2. If name is the empty string, then continue.
        auto const& target_name = navigable->target_name();
        if (target_name.is_empty())
            continue;

        auto name = Utf16FlyString::from_utf16(target_name.utf16_view());

        // 3. If firstNamedChildren contains a navigable whose target name is name, then continue.
        if (first_named_children.contains(name))
            continue;

        // 4. Append navigable to firstNamedChildren.
        (void)first_named_children.set(name, *navigable);
    }

    // 4. Let names be an empty ordered set.
    OrderedHashMap<Utf16FlyString, GC::Ref<Navigable>> names;

    // 5. For each navigable of firstNamedChildren:
    for (auto const& [name, navigable] : first_named_children) {
        // 1. Let name be navigable's target name.
        // 2. If navigable's active document's origin is same origin with window's relevant settings object's origin, then append name to names.
        auto origin = navigable->active_document_origin();
        if (origin.has_value() && origin->is_same_origin(this->origin()))
            names.set(name, navigable);
    }

    // 6. Return names.
    return names;
}

}
