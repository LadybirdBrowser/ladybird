/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGTitleElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/SVG/SVGTitleElement.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGTitleElement);

SVGTitleElement::SVGTitleElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGTitleElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGTitleElement);
}

JS::GCPtr<Layout::Node> SVGTitleElement::create_layout_node(CSS::StyleProperties)
{
    return nullptr;
}

void SVGTitleElement::children_changed()
{
    Base::children_changed();

    auto& page = document().page();
    if (document().browsing_context() != &page.top_level_browsing_context())
        return;

    auto* document_element = document().document_element();

    if (document_element == parent() && is<SVGElement>(document_element))
        page.client().page_did_change_title(document().title().to_byte_string());
}

}
