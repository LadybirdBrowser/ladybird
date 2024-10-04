/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLBRElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/Layout/BreakNode.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLBRElement);

HTMLBRElement::HTMLBRElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLBRElement::~HTMLBRElement() = default;

void HTMLBRElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLBRElement);
}

JS::GCPtr<Layout::Node> HTMLBRElement::create_layout_node(NonnullRefPtr<CSS::StyleProperties> style)
{
    return heap().allocate_without_realm<Layout::BreakNode>(document(), *this, move(style));
}

}
