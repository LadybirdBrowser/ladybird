/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLTitleElement.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLTitleElement);

HTMLTitleElement::HTMLTitleElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLTitleElement::~HTMLTitleElement() = default;

void HTMLTitleElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    HTMLElement::children_changed(metadata);
    if (!m_suppresses_title_change_reports)
        report_title_change_to_page();
}

void HTMLTitleElement::report_title_change_to_page()
{
    auto navigable = this->navigable();
    if (navigable && navigable->is_traversable())
        navigable->page().client().page_did_change_title(document().title());
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-title-text
Utf16String HTMLTitleElement::text() const
{
    // The text attribute's getter must return this title element's child text content.
    return child_text_content();
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-title-text
void HTMLTitleElement::set_text(Utf16View value)
{
    // The text attribute's setter must string replace all with the given value within this title element.
    // NB: Replacing the children removes the old text before it inserts the new one. The page hears the title once,
    //     after both steps, rather than an empty title in between.
    {
        TemporaryChange suppress_title_change_reports { m_suppresses_title_change_reports, true };
        string_replace_all(value);
    }
    report_title_change_to_page();
}

}
