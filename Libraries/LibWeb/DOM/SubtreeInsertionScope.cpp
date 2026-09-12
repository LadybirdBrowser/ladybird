/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/SubtreeInsertionScope.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>

namespace Web::DOM {

SubtreeInsertionScope::SubtreeInsertionScope(Node& parent)
    : m_parent(parent)
    , m_document(parent.document())
    , m_enclosing_scope(m_document->subtree_insertion_scope())
{
    m_document->set_subtree_insertion_scope({}, this);
}

SubtreeInsertionScope::~SubtreeInsertionScope()
{
    m_document->set_subtree_insertion_scope({}, m_enclosing_scope);
}

GC::Ptr<HTML::HTMLFormElement> SubtreeInsertionScope::nearest_inclusive_form_ancestor_of_parent()
{
    if (!m_nearest_inclusive_form_ancestor_of_parent.has_value()) {
        GC::Ptr<HTML::HTMLFormElement> form;
        for (auto* node = m_parent.ptr(); node; node = node->parent()) {
            if (auto* form_element = as_if<HTML::HTMLFormElement>(*node)) {
                form = form_element;
                break;
            }
        }
        m_nearest_inclusive_form_ancestor_of_parent = form;
    }
    return *m_nearest_inclusive_form_ancestor_of_parent;
}

ReadonlySpan<GC::Ref<Element>> SubtreeInsertionScope::inclusive_fieldset_ancestors_of_parent()
{
    if (!m_inclusive_fieldset_ancestors_of_parent.has_value()) {
        Vector<GC::Ref<Element>> fieldsets;
        for (GC::Ptr<Element> element = as_if<Element>(*m_parent); element; element = element->parent_element()) {
            if (is<HTML::HTMLFieldSetElement>(*element))
                fieldsets.append(*element);
        }
        m_inclusive_fieldset_ancestors_of_parent = move(fieldsets);
    }
    return *m_inclusive_fieldset_ancestors_of_parent;
}

}
