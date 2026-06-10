/*
 * Copyright (c) 2020-2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(DocumentFragment);

DocumentFragment::DocumentFragment(Document& document)
    : ParentNode(document, NodeType::DOCUMENT_FRAGMENT_NODE)
{
}

void DocumentFragment::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_host);
}

void DocumentFragment::set_host(Web::DOM::Element* element)
{
    m_host = element;
}

GC::Ref<DocumentFragment> DocumentFragment::create(Document& document)
{
    return GC::Heap::the().allocate<DocumentFragment>(document);
}

GC::Ref<DocumentFragment> DocumentFragment::construct_impl(JS::Realm& realm)
{
    return create(HTML::relevant_window(realm.global_object()).associated_document());
}

}
