/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLStyleElement.h>
#include <LibWeb/HTML/HTMLStyleElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLStyleElement);

HTMLStyleElement::HTMLStyleElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLStyleElement::~HTMLStyleElement() = default;

void HTMLStyleElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLStyleElement);
    Base::initialize(realm);
}

void HTMLStyleElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visit_style_element_edges(visitor);
}

void HTMLStyleElement::adopted_from(DOM::Document& old_document)
{
    Base::adopted_from(old_document);

    retarget_style_load_event_delayer(document());
}

void HTMLStyleElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    Base::children_changed(metadata);
    update_a_style_block_for_dynamic_change();
}

void HTMLStyleElement::inserted()
{
    Base::inserted();
    update_a_style_block_for_dynamic_change();
}

void HTMLStyleElement::removed_from(IsSubtreeRoot is_subtree_root, Node* old_ancestor, Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);
    update_a_style_block_for_dynamic_change();
}

void HTMLStyleElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    style_element_attribute_changed(name, value);
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-style-disabled
bool HTMLStyleElement::disabled()
{
    // 1. If this does not have an associated CSS style sheet, return false.
    if (!sheet())
        return false;

    // 2. If this's associated CSS style sheet's disabled flag is set, return true.
    if (sheet()->disabled())
        return true;

    // 3. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/semantics.html#dom-style-disabled
void HTMLStyleElement::set_disabled(bool disabled)
{
    // 1. If this does not have an associated CSS style sheet, return.
    if (!sheet())
        return;

    // 2. If the given value is true, set this's associated CSS style sheet's disabled flag.
    //    Otherwise, unset this's associated CSS style sheet's disabled flag.
    sheet()->set_disabled(disabled);
}

// https://html.spec.whatwg.org/multipage/semantics.html#contributes-a-script-blocking-style-sheet
bool HTMLStyleElement::contributes_a_script_blocking_style_sheet() const
{
    return style_element_contributes_a_script_blocking_style_sheet();
}

}
