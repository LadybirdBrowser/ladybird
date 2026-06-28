/*
 * Copyright (c) 2026-present, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Navigable.h>

namespace Web::HTML {

Navigable::~Navigable() = default;

void Navigable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent);
}

bool Navigable::is_ancestor_of(GC::Ref<Navigable> other) const
{
    for (auto ancestor = other->parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor == this)
            return true;
    }
    return false;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-top
GC::Ref<Navigable> Navigable::top_level_traversable()
{
    // 1. Let navigable be inputNavigable.
    GC::Ref<Navigable> navigable = *this;

    // 2. While navigable's parent is not null, set navigable to navigable's parent.
    while (navigable->parent())
        navigable = *navigable->parent();

    // 3. Return navigable.
    return navigable;
}

}
