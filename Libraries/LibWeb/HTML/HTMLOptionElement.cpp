/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/Bindings/HTMLOptionElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLOptGroupElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/SVG/SVGScriptElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLOptionElement);

static u64 m_next_selectedness_update_index = 1;

HTMLOptionElement::HTMLOptionElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLOptionElement::~HTMLOptionElement() = default;

void HTMLOptionElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLOptionElement);
}

// FIXME: This needs to be called any time a descendant's text is modified.
void HTMLOptionElement::update_selection_label()
{
    if (selected()) {
        if (auto* select_element = first_ancestor_of_type<HTMLSelectElement>()) {
            select_element->update_inner_text_element({});
        }
    }
}

void HTMLOptionElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == HTML::AttributeNames::selected) {
        if (!value.has_value()) {
            // Whenever an option element's selected attribute is removed, if its dirtiness is false, its selectedness must be set to false.
            if (!m_dirty)
                set_selected_internal(false);
        } else {
            // Except where otherwise specified, when the element is created, its selectedness must be set to true
            // if the element has a selected attribute. Whenever an option element's selected attribute is added,
            // if its dirtiness is false, its selectedness must be set to true.
            if (!m_dirty)
                set_selected_internal(true);
        }
    } else if (name == HTML::AttributeNames::label) {
        update_selection_label();
    }
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-option-selected
void HTMLOptionElement::set_selected(bool selected)
{
    // On setting, it must set the element's selectedness to the new value, set its dirtiness to true, and then cause the element to ask for a reset.
    set_selected_internal(selected);
    m_dirty = true;
    ask_for_a_reset();
}

void HTMLOptionElement::set_selected_internal(bool selected)
{
    if (m_selected != selected)
        invalidate_style(DOM::StyleInvalidationReason::HTMLOptionElementSelectedChange);

    m_selected = selected;
    if (selected)
        m_selectedness_update_index = m_next_selectedness_update_index++;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-option-value
String HTMLOptionElement::value() const
{
    // The value of an option element is the value of the value content attribute, if there is one.
    // ...or, if there is not, the value of the element's text IDL attribute.
    return attribute(HTML::AttributeNames::value).value_or(text());
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-option-value
WebIDL::ExceptionOr<void> HTMLOptionElement::set_value(String const& value)
{
    return set_attribute(HTML::AttributeNames::value, value);
}

static void concatenate_descendants_text_content(DOM::Node const* node, StringBuilder& builder)
{
    if (is<HTMLScriptElement>(node) || is<SVG::SVGScriptElement>(node))
        return;
    if (is<DOM::Text>(node))
        builder.append(as<DOM::Text>(node)->data());
    node->for_each_child([&](auto const& node) {
        concatenate_descendants_text_content(&node, builder);
        return IterationDecision::Continue;
    });
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-option-label
String HTMLOptionElement::label() const
{
    // The label IDL attribute, on getting, if there is a label content attribute,
    // must return that attribute's value; otherwise, it must return the element's label.
    if (auto label = attribute(HTML::AttributeNames::label); label.has_value())
        return label.release_value();

    return text();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-option-label
void HTMLOptionElement::set_label(String const& label)
{
    MUST(set_attribute(HTML::AttributeNames::label, label));
    // Note: this causes attribute_changed() to be called, which will update the <select>'s label
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-option-text
String HTMLOptionElement::text() const
{
    StringBuilder builder;

    // Concatenation of data of all the Text node descendants of the option element, in tree order,
    // excluding any that are descendants of descendants of the option element that are themselves
    // script or SVG script elements.
    for_each_child([&](auto const& node) {
        concatenate_descendants_text_content(&node, builder);
        return IterationDecision::Continue;
    });

    // Return the result of stripping and collapsing ASCII whitespace from the above concatenation.
    return MUST(Infra::strip_and_collapse_whitespace(builder.string_view()));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-option-text
void HTMLOptionElement::set_text(String const& text)
{
    string_replace_all(text);
    // Note: this causes children_changed() to be called, which will update the <select>'s label
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-option-index
int HTMLOptionElement::index() const
{
    // An option element's index is the number of option elements that are in the same list of options but that come before it in tree order.
    if (auto select_element = first_ancestor_of_type<HTMLSelectElement>()) {
        int index = 0;
        for (auto const& option_element : select_element->list_of_options()) {
            if (option_element.ptr() == this)
                return index;
            ++index;
        }
    }

    // If the option element is not in a list of options, then the option element's index is zero.
    return 0;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#ask-for-a-reset
void HTMLOptionElement::ask_for_a_reset()
{
    // If an option element in the list of options asks for a reset, then run that select element's selectedness setting algorithm.
    if (auto* select = first_ancestor_of_type<HTMLSelectElement>()) {
        select->update_selectedness();
    }
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-option-disabled
bool HTMLOptionElement::disabled() const
{
    // An option element is disabled if its disabled attribute is present or if it is a child of an optgroup element whose disabled attribute is present.
    return has_attribute(AttributeNames::disabled)
        || (parent() && is<HTMLOptGroupElement>(parent()) && static_cast<HTMLOptGroupElement const&>(*parent()).has_attribute(AttributeNames::disabled));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-option-form
GC::Ptr<HTMLFormElement> HTMLOptionElement::form() const
{
    // The form IDL attribute's behavior depends on whether the option element is in a select element or not.
    // If the option has a select element as its parent, or has an optgroup element as its parent and that optgroup element has a select element as its parent,
    // then the form IDL attribute must return the same value as the form IDL attribute on that select element.
    // Otherwise, it must return null.
    auto const* parent = parent_element();
    if (is<HTMLOptGroupElement>(parent))
        parent = parent->parent_element();

    if (is<HTML::HTMLSelectElement>(parent)) {
        auto const* select_element = as<HTMLSelectElement>(parent);
        return const_cast<HTMLFormElement*>(select_element->form());
    }

    return {};
}

Optional<ARIA::Role> HTMLOptionElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-option
    // TODO: Only an option element that is in a list of options or that represents a suggestion in a datalist should return option
    return ARIA::Role::option;
}

void HTMLOptionElement::inserted()
{
    Base::inserted();

    set_selected_internal(selected());

    // 1. The option HTML element insertion steps, given insertedNode, are:
    //    If insertedNode's parent is a select element,
    //    or insertedNode's parent is an optgroup element whose parent is a select element,
    //    then run that select element's selectedness setting algorithm.
    if (is<HTMLSelectElement>(*parent()))
        static_cast<HTMLSelectElement&>(*parent()).update_selectedness();
    else if (is<HTMLOptGroupElement>(parent()) && parent()->parent() && is<HTMLSelectElement>(*parent()->parent()))
        static_cast<HTMLSelectElement&>(*parent()->parent()).update_selectedness();
}

void HTMLOptionElement::removed_from(Node* old_parent)
{
    Base::removed_from(old_parent);

    // The option HTML element removing steps, given removedNode and oldParent, are:
    // 1. If oldParent is a select element, or oldParent is an optgroup element whose parent is a select element,
    //    then run that select element's selectedness setting algorithm.
    if (old_parent) {
        if (is<HTMLSelectElement>(*old_parent))
            static_cast<HTMLSelectElement&>(*old_parent).update_selectedness();
        else if (is<HTMLOptGroupElement>(*old_parent) && old_parent->parent_element() && is<HTMLSelectElement>(old_parent->parent_element()))
            static_cast<HTMLSelectElement&>(*old_parent->parent_element()).update_selectedness();
    }
}

void HTMLOptionElement::children_changed()
{
    Base::children_changed();

    update_selection_label();
}

}
