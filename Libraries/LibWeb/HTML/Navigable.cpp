/*
 * Copyright (c) 2026-present, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>

namespace Web::HTML {

Navigable::~Navigable() = default;

void Navigable::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parent);
}

bool Navigable::is_ancestor_of(Navigable const& other) const
{
    for (auto ancestor = other.parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor.ptr() == this)
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

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#allowed-to-navigate
bool Navigable::allowed_by_sandboxing_to_navigate(Navigable const& target, SourceSnapshotParams const& source_snapshot_params) const
{
    auto& source = *this;

    // A navigable source is allowed by sandboxing to navigate a second navigable target,
    // given a source snapshot params sourceSnapshotParams, if the following steps return true:

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
        if (source_snapshot_params.has_transient_activation && has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedTopLevelNavigationWithUserActivation))
            return false;

        // 3. If sourceSnapshotParams's has transient activation is false, and sourceSnapshotParams's sandboxing flags's
        //    sandboxed top-level navigation without user activation browsing context flag is set, then return false.
        if (!source_snapshot_params.has_transient_activation && has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedTopLevelNavigationWithoutUserActivation))
            return false;

        // 4. Return true.
        return true;
    }

    // 4. If target is a top-level traversable:
    if (target.is_top_level_traversable()) {
        // FIXME: 1. If source is the one permitted sandboxed navigator of target, then return true.

        // 2. If sourceSnapshotParams's sandboxing flags's sandboxed navigation browsing context flag is set, then return false.
        if (has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedNavigation))
            return false;

        // 3. Return true.
        return true;
    }

    // 5. If sourceSnapshotParams's sandboxing flags's sandboxed navigation browsing context flag is set, then return false.
    // 6. Return true.
    return !has_flag(source_snapshot_params.sandboxing_flags, SandboxingFlagSet::SandboxedNavigation);
}

}
