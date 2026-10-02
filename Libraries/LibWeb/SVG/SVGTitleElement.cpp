/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/SVG/SVGTitleElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTitleElement);

SVGTitleElement::SVGTitleElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

CSS::ElementBoxKind SVGTitleElement::box_kind() const
{
    return CSS::ElementBoxKind::NoBox;
}

void SVGTitleElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    Base::children_changed(metadata);
    if (!m_suppresses_title_change_reports)
        report_title_change_to_page();
}

void SVGTitleElement::report_title_change_to_page()
{
    auto navigable = document().navigable();
    if (!navigable || !navigable->is_top_level_traversable())
        return;

    auto* document_element = document().document_element();

    if (document_element == parent() && is<SVGElement>(document_element))
        document().page().client().page_did_change_title(document().title());
}

void SVGTitleElement::set_text(Utf16View value)
{
    // NB: Replacing the children removes the old text before it inserts the new one. The page hears the title once,
    //     after both steps, rather than an empty title in between.
    {
        TemporaryChange suppress_title_change_reports { m_suppresses_title_change_reports, true };
        string_replace_all(value);
    }
    report_title_change_to_page();
}

}
