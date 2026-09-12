/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class SubtreeInsertionScope {
    AK_MAKE_NONCOPYABLE(SubtreeInsertionScope);
    AK_MAKE_NONMOVABLE(SubtreeInsertionScope);

public:
    explicit SubtreeInsertionScope(Node& parent);
    ~SubtreeInsertionScope();

    Node& parent() const { return m_parent; }
    GC::Ptr<HTML::HTMLFormElement> nearest_inclusive_form_ancestor_of_parent();
    ReadonlySpan<GC::Ref<Element>> inclusive_fieldset_ancestors_of_parent();

private:
    GC::Ref<Node> m_parent;
    GC::Ref<Document> m_document;
    SubtreeInsertionScope* m_enclosing_scope { nullptr };
    Optional<GC::Ptr<HTML::HTMLFormElement>> m_nearest_inclusive_form_ancestor_of_parent;
    Optional<Vector<GC::Ref<Element>>> m_inclusive_fieldset_ancestors_of_parent;
};

}
