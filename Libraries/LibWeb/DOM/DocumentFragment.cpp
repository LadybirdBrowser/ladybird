/*
 * Copyright (c) 2020-2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DocumentFragmentPrototype.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/HTML/Window.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(DocumentFragment);

DocumentFragment::DocumentFragment(Document& document)
    : ParentNode(document, NodeType::DOCUMENT_FRAGMENT_NODE)
{
}

void DocumentFragment::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DocumentFragment);
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

// https://dom.spec.whatwg.org/#dom-documentfragment-documentfragment
WebIDL::ExceptionOr<JS::NonnullGCPtr<DocumentFragment>> DocumentFragment::construct_impl(JS::Realm& realm)
{
    auto& window = verify_cast<HTML::Window>(realm.global_object());
    return realm.heap().allocate<DocumentFragment>(realm, window.associated_document());
}

}
