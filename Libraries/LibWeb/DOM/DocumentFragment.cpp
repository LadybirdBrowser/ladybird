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
    // NB: libweb_rust reads the host of a fragment in place.
    static_assert(offsetof(DocumentFragment, m_host) == RustFFI::DOCUMENT_FRAGMENT_HOST_OFFSET);
}

void DocumentFragment::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_host);
}

void DocumentFragment::set_host(Web::DOM::Element* element)
{
    // NB: libweb_rust reads the host as a node, so the element must start with its node, as every cell type starts with
    //     its cell.
    VERIFY(static_cast<void const*>(static_cast<Node const*>(element)) == static_cast<void const*>(element));
    m_host = element;
}

GC::Ref<DocumentFragment> DocumentFragment::create(Document& document)
{
    return GC::Heap::the().allocate<DocumentFragment>(document);
}

GC::Ref<DocumentFragment> DocumentFragment::create_for_constructor(JS::Object& relevant_global_object)
{
    return create(HTML::relevant_window(relevant_global_object).associated_document());
}

}
