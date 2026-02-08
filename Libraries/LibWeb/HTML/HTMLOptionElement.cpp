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
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLDataListElement.h>
#include <LibWeb/HTML/HTMLHRElement.h>
#include <LibWeb/HTML/HTMLOptGroupElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLScriptElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLSelectedContentElement.h>
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
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLOptionElement);
    Base::initialize(realm);
}

void HTMLOptionElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_cached_nearest_select_element);
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

    // this is here to invalidate the cache on the HTMLCollection in HTMLSelectElement::selected_options
    document().bump_dom_tree_version();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-option-value
Utf16String HTMLOptionElement::value() const
{
    // The value of an option element is the value of the value content attribute, if there is one.
    // ...or, if there is not, the value of the element's text IDL attribute.
    if (auto value = attribute(HTML::AttributeNames::value); value.has_value())
        return Utf16String::from_utf8(*value);
    return text();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-option-value
void HTMLOptionElement::set_value(Utf16String const& value)
{
    set_attribute_value(HTML::AttributeNames::value, value.to_utf8_but_should_be_ported_to_utf16());
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
    return text().to_utf8_but_should_be_ported_to_utf16();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-option-label
void HTMLOptionElement::set_label(String const& label)
{
    set_attribute_value(HTML::AttributeNames::label, label);
    // Note: this causes attribute_changed() to be called, which will update the <select>'s label
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-option-text
Utf16String HTMLOptionElement::text() const
{
    StringBuilder builder(StringBuilder::Mode::UTF16);

    // Concatenation of data of all the Text node descendants of the option element, in tree order,
    // excluding any that are descendants of descendants of the option element that are themselves
    // script or SVG script elements.
    for_each_child([&](auto const& node) {
        concatenate_descendants_text_content(&node, builder);
        return IterationDecision::Continue;
    });

    // Return the result of stripping and collapsing ASCII whitespace from the above concatenation.
    return Infra::strip_and_collapse_whitespace(builder.to_utf16_string());
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-option-text
void HTMLOptionElement::set_text(Utf16String const& text)
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
GC::Ptr<HTMLFormElement const> HTMLOptionElement::form() const
{
    // The form getter steps are:

    // 1. Let select be this's option element nearest ancestor select.
    auto select = nearest_select_element();

    // 2. If select is null, then return null.
    if (!select)
        return {};

    // 3. Return select's form owner.
    return select->form();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#update-an-option's-nearest-ancestor-select
void HTMLOptionElement::update_nearest_select_element()
{
    // 1. Let oldSelect be option's cached nearest ancestor select element.
    auto old_select = m_cached_nearest_select_element;

    // 2. Let newSelect be option's option element nearest ancestor select.
    auto new_select = compute_nearest_select_element();

    // 3. If oldSelect is not newSelect:
    if (old_select != new_select) {
        // 1. If oldSelect is not null, then run the selectedness setting algorithm given oldSelect.
        if (old_select)
            old_select->update_selectedness();

        // 2. If newSelect is not null, then run the selectedness setting algorithm given newSelect.
        if (new_select)
            new_select->update_selectedness();
    }

    // 4. Set option's cached nearest ancestor select element to newSelect.
    m_cached_nearest_select_element = new_select;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#option-element-nearest-ancestor-select
GC::Ptr<HTMLSelectElement> HTMLOptionElement::compute_nearest_select_element()
{
    // 1. Let ancestorOptgroup be null.
    GC::Ptr<HTMLOptGroupElement> ancestor_optgroup;

    // 2. For each ancestor of option's ancestors, in reverse tree order:
    for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
        // 1. If ancestor is a datalist, hr, or option element, then return null.
        if (is<HTMLDataListElement>(*ancestor)
            || is<HTMLHRElement>(*ancestor)
            || is<HTMLOptionElement>(*ancestor))
            return nullptr;

        // 2. If ancestor is an optgroup element:
        if (auto* optgroup_element = as_if<HTMLOptGroupElement>(*ancestor)) {
            // 1. If ancestorOptgroup is not null, then return null.
            if (ancestor_optgroup)
                return nullptr;

            // 2. Set ancestorOptgroup to ancestor.
            ancestor_optgroup = optgroup_element;
        }

        // 3. If ancestor is a select, then return ancestor.
        if (auto* select_element = as_if<HTMLSelectElement>(*ancestor))
            return select_element;
    }

    // 3. Return null.
    return nullptr;
}

Optional<ARIA::Role> HTMLOptionElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-option
    // TODO: Only an option element that is in a list of options or that represents a suggestion in a datalist should return option
    return ARIA::Role::option;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-option-element:clone-an-option-into-a-selectedcontent
WebIDL::ExceptionOr<void> HTMLOptionElement::maybe_clone_into_selectedcontent()
{
    // To maybe clone an option into selectedcontent, given an option option:

    // 1. Let select be option's option element nearest ancestor select.
    auto select = m_cached_nearest_select_element;

    // 2. If all of the following conditions are true:
    //      - select is not null;
    //      - option's selectedness is true; and
    //      - select's enabled selectedcontent is not null,
    //    then run clone an option into a selectedcontent given option and select's enabled selectedcontent.
    if (select && selected()) {
        if (auto selectedcontent = select->enabled_selectedcontent())
            TRY(clone_into_selectedcontent(*selectedcontent));
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#clone-an-option-into-a-selectedcontent
WebIDL::ExceptionOr<void> HTMLOptionElement::clone_into_selectedcontent(GC::Ref<HTMLSelectedContentElement> selectedcontent)
{
    // To clone an option into a selectedcontent, given an option element option and a selectedcontent element selectedcontent:

    // 1. Let documentFragment be a new DocumentFragment whose node document is option's node document.
    auto fragment = realm().create<DOM::DocumentFragment>(document());

    // 2. For each child of option's children:
    for (auto* child = first_child(); child; child = child->next_sibling()) {
        // 1. Let childClone be the result of running clone given child with subtree set to true.
        auto child_clone = TRY(child->clone_node(&document(), true));

        // 2. Append childClone to documentFragment.
        TRY(fragment->append_child(child_clone));
    }

    // 3. Replace all with documentFragment within selectedcontent.
    selectedcontent->replace_all(fragment);

    return {};
}

void HTMLOptionElement::inserted()
{
    Base::inserted();

    set_selected_internal(selected());

    // The option HTML element insertion steps, given insertedOption,
    // are to run update an option's nearest ancestor select given insertedOption.
    update_nearest_select_element();
}

void HTMLOptionElement::removed_from(Node* old_parent, Node& old_root)
{
    Base::removed_from(old_parent, old_root);

    // The option HTML element removing steps, given removedOption and oldParent,
    // are to run update an option's nearest ancestor select given removedOption.
    update_nearest_select_element();
}

void HTMLOptionElement::children_changed(ChildrenChangedMetadata const* metadata)
{
    Base::children_changed(metadata);

    update_selection_label();
}

}
