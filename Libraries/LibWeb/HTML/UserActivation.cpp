/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/UserActivation.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(UserActivation);

GC::Ref<UserActivation> UserActivation::create(Window& window)
{
    return GC::Heap::the().allocate<UserActivation>(window);
}

UserActivation::UserActivation(Window& window)
    : m_window(window)
{
}

void UserActivation::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-useractivation-hasbeenactive
bool UserActivation::has_been_active() const
{
    // The hasBeenActive getter steps are to return true if this's relevant global object has sticky activation, and false otherwise.
    return m_window->has_sticky_activation();
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-useractivation-isactive
bool UserActivation::is_active() const
{
    // The isActive getter steps are to return true if this's relevant global object has transient activation, and false otherwise.
    return m_window->has_transient_activation();
}

}
