/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/NativeFunction.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/Invalidation/ElementStateInvalidator.h>
#include <LibWeb/CSS/Invalidation/FormControlInvalidator.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLDataListElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLHRElement.h>
#include <LibWeb/HTML/HTMLOptGroupElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLSelectedContentElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWebCommon/Infra/Strings.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLSelectElement);

HTMLSelectElement::HTMLSelectElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLSelectElement::~HTMLSelectElement() = default;

// `:user-valid` and `:user-invalid` turn on the first time the user has interacted with the
// control, which no attribute and no value says. The style engine is told what the element now
// holds rather than asking.
void HTMLSelectElement::set_user_validity(bool flag)
{
    if (m_user_validity == flag)
        return;
    m_user_validity = flag;
    CSS::Invalidation::invalidate_style_after_validity_change(*this);
}

void HTMLSelectElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_options);
    visitor.visit(m_selected_options);
    visitor.visit(m_inner_text_element);
    visitor.visit(m_selected_label_element);
    visitor.visit(m_label_sizer_element);
    visitor.visit(m_chevron_icon_element);
    visitor.visit(m_cached_list_of_options);
    visitor.visit(m_select_item_option_elements);
    visitor.visit(m_list_box_element);
    for (auto const& row : m_list_box_rows) {
        visitor.visit(row.element);
        visitor.visit(row.option);
    }
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-select-size
u32 HTMLSelectElement::display_size() const
{
    // The size IDL attribute must reflect the respective content attributes of the same name. The size IDL attribute has a default value of 0.
    if (auto size_string = get_attribute(HTML::AttributeNames::size); size_string.has_value()) {
        // The display size of a select element is the result of applying the rules for parsing non-negative integers
        // to the value of element's size attribute, if it has one and parsing it is successful.
        if (auto size = parse_non_negative_integer(*size_string); size.has_value())
            return *size;
    }

    // If applying those rules to the attribute's value is not successful or if the size attribute is absent,
    // then the element's display size is 4 if the element's multiple content attribute is present, and 1 otherwise.
    if (has_attribute(AttributeNames::multiple))
        return 4;
    return 1;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-size
WebIDL::UnsignedLong HTMLSelectElement::size() const
{
    // The multiple, required, and size IDL attributes must reflect the respective content attributes of the same name. The size IDL attribute has a default value of 0.
    if (auto size_string = get_attribute(HTML::AttributeNames::size); size_string.has_value()) {
        if (auto size = parse_non_negative_integer(*size_string); size.has_value() && *size <= 2147483647)
            return *size;
    }

    return 0;
}

void HTMLSelectElement::set_size(WebIDL::UnsignedLong size)
{
    if (size > 2147483647)
        size = 0;
    set_attribute_value(HTML::AttributeNames::size, Utf16String::number(size));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-options
GC::Ptr<HTMLOptionsCollection> const& HTMLSelectElement::options() const
{
    // The options IDL attribute must return an HTMLOptionsCollection rooted at the select node,
    // whose filter matches the elements in the list of options.
    if (!m_options) {
        m_options = HTMLOptionsCollection::create(const_cast<HTMLSelectElement&>(*this), [this](DOM::Element const& element) {
            auto const* maybe_option = as_if<HTML::HTMLOptionElement>(element);
            return maybe_option && maybe_option->nearest_select_element().ptr() == this;
        });
    }
    return m_options;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-length
WebIDL::UnsignedLong HTMLSelectElement::length()
{
    // The length IDL attribute must return the number of nodes represented by the options collection. On setting, it must act like the attribute of the same name on the options collection.
    return const_cast<HTMLOptionsCollection&>(*options()).length();
}

WebIDL::ExceptionOr<void> HTMLSelectElement::set_length(WebIDL::UnsignedLong length)
{
    // On setting, it must act like the attribute of the same name on the options collection.
    return const_cast<HTMLOptionsCollection&>(*options()).set_length(length);
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-item
HTMLOptionElement* HTMLSelectElement::item(WebIDL::UnsignedLong index)
{
    // The item(index) method must return the value returned by the method of the same name on the options collection, when invoked with the same argument.
    return as<HTMLOptionElement>(const_cast<HTMLOptionsCollection&>(*options()).item(index));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-nameditem
HTMLOptionElement* HTMLSelectElement::named_item(Utf16String const& name)
{
    // The namedItem(name) method must return the value returned by the method of the same name on the options collection, when invoked with the same argument.
    return as<HTMLOptionElement>(const_cast<HTMLOptionsCollection&>(*options()).named_item(name.utf16_view()));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-add
WebIDL::ExceptionOr<void> HTMLSelectElement::add(HTMLOptionOrOptGroupElement element, NullableHTMLElementOrElementIndex before)
{
    // Similarly, the add(element, before) method must act like its namesake method on that same options collection.
    TRY(const_cast<HTMLOptionsCollection&>(*options()).add(move(element), move(before)));

    update_selectedness(); // Not in spec

    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-select-element:set-the-value-of-a-new-indexed-property
WebIDL::ExceptionOr<void> HTMLSelectElement::set_value_of_indexed_property(u32 n, Optional<GC::Ref<DOM::Element>> new_value)
{
    // When the user agent is to set the value of a new indexed property or set the value of an existing indexed property
    // for a select element, it must instead run the corresponding algorithm on the select element's options collection.
    TRY(const_cast<HTMLOptionsCollection&>(*options()).set_value_of_indexed_property(n, move(new_value)));

    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-remove
void HTMLSelectElement::remove()
{
    // The remove() method must act like its namesake method on that same options collection when it has arguments,
    // and like its namesake method on the ChildNode interface implemented by the HTMLSelectElement ancestor interface Element when it has no arguments.
    ChildNode::remove_from_parent();
}

void HTMLSelectElement::remove(WebIDL::Long index)
{
    const_cast<HTMLOptionsCollection&>(*options()).remove(index);
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-selectedoptions
GC::Ref<DOM::HTMLCollection> HTMLSelectElement::selected_options()
{
    // The selectedOptions IDL attribute must return an HTMLCollection rooted at the select node,
    // whose filter matches the elements in the list of options that have their selectedness set to true.
    if (!m_selected_options) {
        auto filter = [this](Element const& element) {
            auto const* maybe_option = as_if<HTML::HTMLOptionElement>(element);
            if (maybe_option && maybe_option->nearest_select_element().ptr() == this) {
                return maybe_option->selected();
            }
            return false;
        };
        m_selected_options = DOM::HTMLCollection::create(*this, DOM::HTMLCollection::Scope::Descendants, move(filter), DOM::HTMLCollection::AttributeInvalidationType::None, nullptr, DOM::HTMLCollection::Kind::SelectedOptions);
    }
    return *m_selected_options;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-select-option-list
void HTMLSelectElement::update_cached_list_of_options() const
{
    // 1. Let options be « ».
    m_cached_list_of_options.clear();

    // Check if node is an optgroup element and node has an ancestor optgroup in between itself and this select
    auto is_nested_optgroup = [this](DOM::Node const& node) {
        if (!is<HTMLOptGroupElement>(node))
            return false;

        for (auto const* ancestor = node.parent(); ancestor; ancestor = ancestor->parent()) {
            if (ancestor == this)
                return false; // reached the select without another optgroup
            if (is<HTMLOptGroupElement>(*ancestor))
                return true; // found an optgroup above us
        }
        return false;
    };

    // 2. Let node be the first child of select in tree order.
    // 3. While node is not null:
    for_each_in_subtree([&](auto& node) {
        // 1. If node is an option element, then append node to options.
        if (auto maybe_option = as_if<HTMLOptionElement>(node))
            m_cached_list_of_options.append(const_cast<HTMLOptionElement&>(*maybe_option));

        // 2. If any of the following conditions are true:
        //    - node is a select element;
        //    - node is an hr element;
        //    - node is an option element;
        //    - node is a datalist element;
        //    - node is an optgroup element and node has an ancestor optgroup in between itself and select,
        if (is<HTMLSelectElement>(node)
            || is<HTMLHRElement>(node)
            || is<HTMLOptionElement>(node)
            || is<HTMLDataListElement>(node)
            || is_nested_optgroup(node)) {
            // then set node to the next descendant of select in tree order, excluding node's descendants, if any such
            // node exists; otherwise null.
            return TraversalDecision::SkipChildrenAndContinue;
        }
        // Otherwise, set node to the next descendant of select in tree order, if any such node exists; otherwise null.
        return TraversalDecision::Continue;
    });

    // 4. Return options.
    // (Implicit by updating m_cached_list_of_options)
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-select-option-list
Vector<GC::Root<HTMLOptionElement>> HTMLSelectElement::list_of_options() const
{
    update_cached_list_of_options();
    Vector<GC::Root<HTMLOptionElement>> list;
    list.ensure_capacity(m_cached_list_of_options.size());
    for (auto& item : m_cached_list_of_options)
        list.unchecked_append(GC::make_root(item));

    return list;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-select-element:concept-form-reset-control
void HTMLSelectElement::reset_algorithm()
{
    update_cached_list_of_options();
    // The reset algorithm for a select element selectElement is:

    // 1. Set selectElement's user validity to false.
    m_user_validity = false;

    // 2. For each optionElement of selectElement's list of options:
    for (auto const& option_element : m_cached_list_of_options) {
        // 1. If optionElement has a selected attribute, then set optionElement's selectedness to true; otherwise set it to false.
        option_element->set_selected_internal(option_element->has_attribute(AttributeNames::selected));
        // 2. Set optionElement's dirtiness to false.
        option_element->m_dirty = false;
    }

    // 3. Run the selectedness setting algorithm given selectElement.
    update_selectedness();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-selectedindex
WebIDL::Long HTMLSelectElement::selected_index() const
{
    // The selectedIndex getter steps are to return the index of the first option element in this's list of options
    // in tree order that has its selectedness set to true, if any. If there isn't one, then return −1.
    update_cached_list_of_options();

    WebIDL::Long index = 0;
    for (auto const& option_element : m_cached_list_of_options) {
        if (option_element->selected())
            return index;
        ++index;
    }
    return -1;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-selectedindex
WebIDL::ExceptionOr<void> HTMLSelectElement::set_selected_index(WebIDL::Long index)
{
    // The selectedIndex setter steps are:
    ScopeGuard guard { [&]() {
        clone_selected_option_into_select_button();
        // AD-HOC: Changing the selected option can change whether a required select satisfies its constraints.
        CSS::Invalidation::invalidate_style_after_validity_change(*this);
    } };

    // 1. Let firstMatchingOption be null.
    GC::Ptr<HTMLOptionElement> first_matching_option;

    // 2. For each option of this's list of options:
    update_cached_list_of_options();
    WebIDL::Long current_index = 0;
    for (auto const& option : m_cached_list_of_options) {
        // 1. Set option's selectedness to false.
        option->set_selected_internal(false);

        // 2. If firstMatchingOption is null and option's index is equal to the given value, then
        //    set firstMatchingOption to option.
        if (!first_matching_option && current_index == index)
            first_matching_option = option;

        current_index++;
    }

    // 3. If firstMatchingOption is not null, then set firstMatchingOption's selectedness to true
    //    and set firstMatchingOption's dirtiness to true.
    if (first_matching_option) {
        first_matching_option->set_selected_internal(true);
        first_matching_option->m_dirty = true;
    }

    // 4. Run update a select's selectedcontent given this.
    TRY(update_selectedcontent());

    return {};
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLSelectElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

bool HTMLSelectElement::can_skip_selectedness_update_for_inserted_option(HTMLOptionElement const& option) const
{
    if (option.selected())
        return false;

    if (m_number_of_selected_options >= 2)
        return false;

    if (display_size() == 1 && m_number_of_selected_options == 0)
        return false;

    return true;
}

bool HTMLSelectElement::can_skip_children_changed_selectedness_update(ChildrenChangedMetadata const& metadata) const
{
    // If the following criteria are met, there is no need to re-run the selectedness algorithm.
    // FIXME: We can tighten up these conditions and skip even more work!
    if (metadata.type != ChildrenChangedMetadata::Type::Inserted)
        return false;

    if (auto* option = as_if<HTMLOptionElement>(*metadata.node))
        return can_skip_selectedness_update_for_inserted_option(*option);

    return false;
}

void HTMLSelectElement::children_changed(ChildrenChangedMetadata const& metadata)
{
    Base::children_changed(metadata);

    if (can_skip_children_changed_selectedness_update(metadata))
        return;

    update_cached_list_of_options();
    update_selectedness();
    list_box_options_did_change();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-type
Utf16FlyString HTMLSelectElement::type() const
{
    // The type IDL attribute, on getting, must return the string "select-one" if the multiple attribute is absent, and the string "select-multiple" if the multiple attribute is present.
    if (!has_attribute(AttributeNames::multiple))
        return "select-one"_utf16_fly_string;

    return "select-multiple"_utf16_fly_string;
}

Optional<ARIA::Role> HTMLSelectElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-select-multiple-or-size-greater-1
    if (has_attribute(AttributeNames::multiple))
        return ARIA::Role::listbox;
    if (auto size_string = get_attribute(HTML::AttributeNames::size); size_string.has_value()) {
        if (auto size = size_string->to_number<int>(); size.has_value() && *size > 1)
            return ARIA::Role::listbox;
    }
    // https://www.w3.org/TR/html-aria/#el-select
    return ARIA::Role::combobox;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-value
Utf16String HTMLSelectElement::value() const
{
    // The value getter steps are to return the value of the first option element in this's
    // list of options in tree order that has its selectedness set to true, if any. If there
    // isn't one, then return the empty string.
    update_cached_list_of_options();
    for (auto const& option_element : m_cached_list_of_options)
        if (option_element->selected())
            return option_element->value();
    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-value
WebIDL::ExceptionOr<void> HTMLSelectElement::set_value(Utf16View value)
{
    // The value setter steps are:
    ScopeGuard guard { [&]() {
        clone_selected_option_into_select_button();
        // AD-HOC: Changing the selected option can change whether a required select satisfies its constraints.
        CSS::Invalidation::invalidate_style_after_validity_change(*this);
    } };
    update_cached_list_of_options();

    // 1. Let firstMatchingOption be null.
    GC::Ptr<HTMLOptionElement> first_matching_option;

    // 2. For each option of this's list of options:
    for (auto const& option_element : m_cached_list_of_options) {
        // 1. Set option's selectedness to false.
        option_element->set_selected_internal(false);

        // 2. If firstMatchingOption is null and option's value is equal to the given value, then set
        //    firstMatchingOption to option.
        if (!first_matching_option && option_element->value() == value)
            first_matching_option = option_element;
    }

    // 3. If firstMatchingOption is not null, then set firstMatchingOption's selectedness to true and set
    //    firstMatchingOption's dirtiness to true.
    if (first_matching_option) {
        first_matching_option->set_selected_internal(true);
        first_matching_option->m_dirty = true;
    }

    // 4. Run update a select's selectedcontent given this.
    TRY(update_selectedcontent());

    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#send-select-update-notifications
void HTMLSelectElement::send_select_update_notifications()
{
    // To send select update notifications for a select element element, queue an element task on
    // the user interaction task source given element to run these steps:
    // https://github.com/whatwg/html/issues/1080
    // INTEROP: Other engines perform these steps synchronously after changing the selection.

    // 1. Set the select element's user validity to true.
    m_user_validity = true;

    // AD-HOC: Setting the user validity changes which of the :user-valid and :user-invalid pseudo-classes match.
    CSS::Invalidation::invalidate_style_after_validity_change(*this);

    // 2. Run update a select's selectedcontent given element.
    MUST(update_selectedcontent());

    // 4. Fire an event named input at element, with the bubbles and composed attributes initialized to true.
    auto input_event = DOM::Event::create(HTML::relevant_global_object(*this), HTML::EventNames::input);
    input_event->set_bubbles(true);
    input_event->set_composed(true);
    dispatch_event(input_event);

    // 5. Fire an event named change at element, with the bubbles attribute initialized to true.
    auto change_event = DOM::Event::create(HTML::relevant_global_object(*this), HTML::EventNames::change);
    change_event->set_bubbles(true);
    dispatch_event(*change_event);
}

void HTMLSelectElement::set_is_open(bool open)
{
    if (open == m_is_open)
        return;

    m_is_open = open;
    CSS::Invalidation::invalidate_style_after_select_open_state_change(*this, open);
}

bool HTMLSelectElement::has_activation_behavior() const
{
    return true;
}

// https://html.spec.whatwg.org/multipage/input.html#show-the-picker,-if-applicable
void HTMLSelectElement::show_the_picker_if_applicable()
{
    // FIXME: Deduplicate with HTMLInputElement
    // To show the picker, if applicable for a select element element:

    // 1. If element's relevant global object does not have transient activation, then return.
    auto& relevant_global = relevant_window(*this);
    if (!relevant_global.has_transient_activation())
        return;

    // 2. If element is not mutable, then return.
    if (!is_mutable())
        return;

    // 3. Consume user activation given element's relevant global object.
    relevant_global.consume_user_activation();

    // 4. If element does not support a picker, then return.
    // NB: Select elements always support a picker.

    // 5. If element is an input element and element's type attribute is in the File Upload state, then run these steps
    //    in parallel:
    // NB: Not applicable to select elements.

    // 6. Otherwise, the user agent should show the relevant user interface for selecting a value for element, in the
    //    way it normally would when the user interacts with the control.
    //    When showing such a user interface, it must respect the requirements stated in the relevant parts of the
    //    specification for how element behaves given its type attribute state. (For example, various sections describe
    //    restrictions on the resulting value string.)
    //    This step can have side effects, such as closing other pickers that were previously shown by this algorithm.
    //    (If this closes a file selection picker, then per the above that will lead to firing either input and change
    //    events, or a cancel event.)

    // Populate select items
    // NB: Each option item's ID is one past its index in m_select_item_option_elements.
    Vector<SelectItem> select_items;
    m_select_item_option_elements.clear();
    auto create_select_item_option = [&](HTMLOptionElement& option_element) {
        m_select_item_option_elements.append(option_element);
        return SelectItemOption { static_cast<u32>(m_select_item_option_elements.size()), option_element.selected(), option_element.disabled(), Infra::strip_and_collapse_whitespace(option_element.label()), option_element.value() };
    };
    for (auto const& child : children_as_vector()) {
        if (auto const* opt_group_element = as_if<HTMLOptGroupElement>(*child)) {
            if (!opt_group_element->has_attribute(Web::HTML::AttributeNames::hidden)) {
                Vector<SelectItemOption> option_group_items;
                for (auto const& child : opt_group_element->children_as_vector()) {
                    if (auto const& option_element = as_if<HTMLOptionElement>(*child)) {
                        if (!option_element->has_attribute(Web::HTML::AttributeNames::hidden))
                            option_group_items.append(create_select_item_option(*option_element));
                    }
                }
                auto label = opt_group_element->get_attribute(AttributeNames::label);
                select_items.append(SelectItemOptionGroup {
                    label.has_value() ? label.release_value() : Utf16String {},
                    move(option_group_items) });
            }
        }

        if (auto const& option_element = as_if<HTMLOptionElement>(*child)) {
            if (!option_element->has_attribute(Web::HTML::AttributeNames::hidden))
                select_items.append(create_select_item_option(*option_element));
        }

        if (auto const* hr_element = as_if<HTMLHRElement>(*child)) {
            if (!hr_element->has_attribute(Web::HTML::AttributeNames::hidden))
                select_items.append(SelectItemSeparator {});
        }
    }

    // Request select dropdown
    auto weak_element = GC::Weak<HTMLSelectElement> { *this };
    auto rect = get_bounding_client_rect();
    auto navigable = document().navigable();
    auto position = navigable->to_page_position(Web::CSSPixelPoint { rect.x(), rect.bottom() });
    document().page().did_request_select_dropdown(weak_element, navigable->local_root()->id(), position, rect.width(), move(select_items));
    set_is_open(true);
}

// https://html.spec.whatwg.org/multipage/input.html#dom-select-showpicker
WebIDL::ExceptionOr<void> HTMLSelectElement::show_picker()
{
    // FIXME: Deduplicate with HTMLInputElement
    // The showPicker() method steps are:

    // 1. If this is not mutable, then throw an "InvalidStateError" DOMException.
    if (!is_mutable())
        return WebIDL::InvalidStateError::create("Element is not mutable"_utf16);

    // 2. If this's relevant settings object's origin is not same origin with this's relevant settings object's top-level origin,
    //    and this is a select element, then throw a "SecurityError" DOMException.
    if (!relevant_settings_object(*this).origin().is_same_origin(relevant_settings_object(*this).top_level_origin.value())) {
        return WebIDL::SecurityError::create("Cross origin pickers are not allowed"_utf16);
    }

    // 3. If this's relevant global object does not have transient activation, then throw a "NotAllowedError" DOMException.
    auto* relevant_window = window_from_global_object(relevant_global_object(*this));
    if (!relevant_window || !relevant_window->has_transient_activation()) {
        return WebIDL::NotAllowedError::create("Too long since user activation to show picker"_utf16);
    }

    // 4. If this is a select element, and this is not being rendered, then throw a "NotSupportedError" DOMException.
    // NB: This treats an element outside a display: none subtree as being rendered, without forcing layout.
    if (!meets_focusable_area_rendering_requirements())
        return WebIDL::NotSupportedError::create("Element is not being rendered"_utf16);

    // 5. Show the picker, if applicable, for this.
    show_the_picker_if_applicable();
    return {};
}

void HTMLSelectElement::activation_behavior(DOM::Event const& event)
{
    // NB: A list box shows its options in place, and is interacted with there.
    if (renders_as_list_box())
        return;
    if (event.is_trusted())
        show_the_picker_if_applicable();
}

void HTMLSelectElement::did_select_item(Optional<u32> const& id)
{
    set_is_open(false);

    if (!id.has_value())
        return;

    update_cached_list_of_options();
    for (auto const& option_element : m_cached_list_of_options)
        option_element->set_selected(false);

    if (*id >= 1 && *id <= m_select_item_option_elements.size())
        m_select_item_option_elements[*id - 1]->set_selected(true);

    clone_selected_option_into_select_button();
    send_select_update_notifications();
}

void HTMLSelectElement::form_associated_element_was_inserted()
{
    create_shadow_tree_if_needed();
}

void HTMLSelectElement::form_associated_element_attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    if (namespace_.has_value())
        return;

    // https://html.spec.whatwg.org/multipage/form-elements.html#the-select-element
    if (name == HTML::AttributeNames::size) {
        // Whenever a select element's size attribute is set, changed, or removed, reset a select's selectedness given the select element and false.
        reset_selectedness(HasChanged::No);
    } else if (name == HTML::AttributeNames::multiple && old_value.has_value() != value.has_value()) {
        // Whenever a select element select's multiple attribute is added or removed, run these steps:
        // 1. If the multiple attribute was removed, then set the selectedness of all but the first option element
        //    with its selectedness set to true in select's list of options to false.
        if (!value.has_value()) {
            update_cached_list_of_options();
            bool found_selected_option = false;
            for (auto const& option : m_cached_list_of_options) {
                if (!option->selected())
                    continue;
                if (found_selected_option)
                    option->set_selected_internal(false);
                found_selected_option = true;
            }
        }

        // 2. Reset a select's selectedness given select and true.
        reset_selectedness(HasChanged::Yes);
    }

    // NB: The size and multiple attributes decide whether the select renders as a drop-down box or a list box.
    if ((name == HTML::AttributeNames::size || name == HTML::AttributeNames::multiple) && shadow_root()
        && renders_as_list_box() != !!m_list_box_element) {
        set_shadow_root(nullptr);
        m_inner_text_element = nullptr;
        m_selected_label_element = nullptr;
        m_label_sizer_element = nullptr;
        m_chevron_icon_element = nullptr;
        m_chevron_icon_hidden = false;
        m_list_box_element = nullptr;
        m_list_box_rows.clear();
        m_list_box_anchor_index = {};
        m_list_box_focus_index = {};
        create_shadow_tree_if_needed();
    } else if (name == HTML::AttributeNames::size && m_list_box_element) {
        update_list_box();
    }

    // AD-HOC: Changing the required or multiple attribute can change whether the select satisfies its constraints.
    if (name == HTML::AttributeNames::required || name == HTML::AttributeNames::multiple)
        CSS::Invalidation::invalidate_style_after_validity_change(*this);
}

void HTMLSelectElement::computed_properties_changed()
{
    if (!m_chevron_icon_element)
        return;

    // Hide chevron icon when appearance is none
    auto style = computed_style();
    VERIFY(style);
    // NB: This runs while the style engine's records are applied, and rewriting the shadow tree's declarations then
    //     makes the records of its elements stale. So they are only written when appearance flips.
    bool hide_chevron_icon = style->appearance() == CSS::Appearance::None;
    if (hide_chevron_icon == m_chevron_icon_hidden)
        return;
    m_chevron_icon_hidden = hide_chevron_icon;
    if (hide_chevron_icon) {
        MUST(m_chevron_icon_element->style()->set_property(CSS::PropertyID::Display, "none"_utf16));
        MUST(m_inner_text_element->style()->set_property(CSS::PropertyID::MarginInlineEnd, "0"_utf16));
    } else {
        MUST(m_chevron_icon_element->style()->set_property(CSS::PropertyID::Display, "block"_utf16));
        MUST(m_inner_text_element->style()->set_property(CSS::PropertyID::MarginInlineEnd, "20px"_utf16));
    }
}

void HTMLSelectElement::create_shadow_tree_if_needed()
{
    if (shadow_root())
        return;

    auto shadow_root = DOM::ShadowRoot::create(document(), *this, Web::DOM::ShadowRootMode::Closed);
    shadow_root->set_user_agent_internal(true);
    set_shadow_root(shadow_root);

    if (renders_as_list_box()) {
        create_list_box_shadow_tree(shadow_root);
        return;
    }

    auto border = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    border->set_attribute_value(HTML::AttributeNames::style, R"~~~(
        display: flex;
        align-items: center;
        height: 100%;
        position: relative;
    )~~~"_utf16);
    MUST(shadow_root->append_child(border));

    m_inner_text_element = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    m_inner_text_element->set_attribute_value(HTML::AttributeNames::style, R"~~~(
        flex: 1;
        min-width: 0;
        overflow: clip;
        text-overflow: inherit;
        margin-inline-end: 20px;
    )~~~"_utf16);
    MUST(border->append_child(*m_inner_text_element));

    m_selected_label_element = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    m_selected_label_element->set_attribute_value(HTML::AttributeNames::style, R"~~~(
        overflow: clip;
        text-overflow: inherit;
    )~~~"_utf16);
    MUST(m_inner_text_element->append_child(*m_selected_label_element));

    // NB: The label sizer holds the label of every option in its own block row, without taking up any block size or
    //     being painted. It gives the select enough inline size for its widest option, so choosing another option
    //     does not resize it. It floats so that its rows never give the select a baseline, even when no option is selected.
    m_label_sizer_element = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    m_label_sizer_element->set_attribute_value(HTML::AttributeNames::style, R"~~~(
        float: inline-start;
        block-size: 0;
        overflow: clip;
        visibility: hidden;
    )~~~"_utf16);
    MUST(m_inner_text_element->append_child(*m_label_sizer_element));
    update_cached_list_of_options();
    for (auto const& option : m_cached_list_of_options)
        option->update_label_sizer_text();

    // NB: The chevron icon is positioned out of flow so that the label is the wrapper's only flex item, keeping the
    //     baseline of the select derived from the label text. The label's margin-inline-end reserves its space.
    m_chevron_icon_element = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    m_chevron_icon_element->set_attribute_value(HTML::AttributeNames::style, R"~~~(
        display: block;
        position: absolute;
        inset-inline-end: 0;
        top: calc(50% - 8px);
        width: 16px;
        height: 16px;
    )~~~"_utf16);

    auto chevron_svg_element = DOM::create_element(document(), SVG::TagNames::svg, Namespace::SVG).release_value_but_fixme_should_propagate_errors();
    chevron_svg_element->set_attribute_value(SVG::AttributeNames::xmlns, Namespace::SVG.to_utf16_string());
    chevron_svg_element->set_attribute_value(SVG::AttributeNames::viewBox, "0 0 24 24"_utf16);
    MUST(m_chevron_icon_element->append_child(chevron_svg_element));

    auto chevron_path_element = DOM::create_element(document(), SVG::TagNames::path, Namespace::SVG).release_value_but_fixme_should_propagate_errors();
    chevron_path_element->set_attribute_value(SVG::AttributeNames::fill, "currentcolor"_utf16);
    chevron_path_element->set_attribute_value(SVG::AttributeNames::d, "M7.41,8.58L12,13.17L16.59,8.58L18,10L12,16L6,10L7.41,8.58Z"_utf16);
    MUST(chevron_svg_element->append_child(chevron_path_element));

    MUST(border->append_child(*m_chevron_icon_element));

    clone_selected_option_into_select_button();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#clone-selected-option-into-select-button
void HTMLSelectElement::clone_selected_option_into_select_button()
{
    // To clone selected option into select button, given a select element select:

    if (!m_selected_label_element)
        return;

    update_cached_list_of_options();

    // 1. Let option be the first element of select's option list whose selectedness is set to true,
    //    if such an element exists; otherwise null.
    auto option = find_value(m_cached_list_of_options, [](auto option) { return option->selected(); });

    // 2. Let text be the empty string.
    Utf16String text;

    // 3. If option is not null, then set text to option's label.
    if (option.has_value())
        text = (*option)->label();

    // 4. Set select's select fallback button text to text.
    m_selected_label_element->string_replace_all(move(text));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#selectedness-setting-algorithm
// https://whatpr.org/html/11890/form-elements.html#selectedness-setting-algorithm
bool HTMLSelectElement::update_selectedness()
{
    // The selectedness setting algorithm, given a select element element, is to run the following steps:
    update_cached_list_of_options();

    // 1. Let updateSelectedcontent be false.
    auto should_update_selectedcontent = false;

    // 2. If element 's multiple attribute is absent, and element's display size is 1,
    //    and no option elements in the element's list of options have their selectedness set to true, then
    if (!has_attribute(AttributeNames::multiple) && display_size() == 1 && m_number_of_selected_options == 0) {
        // 1. Set the selectedness of the first option element in the list of options in tree order
        //    that is not disabled, if any, to true.
        for (auto const& option_element : m_cached_list_of_options) {
            if (!option_element->disabled()) {
                option_element->set_selected_internal(true);
                break;
            }
        }

        // 2. Set updateSelectedcontent to true.
        should_update_selectedcontent = true;
    }
    // Otherwise, if element's multiple attribute is absent,
    // and two or more option elements in element's list of options have their selectedness set to true, then:
    else if (!has_attribute(AttributeNames::multiple) && m_number_of_selected_options >= 2) {
        // 1. Set the selectedness of all but the last option element with its selectedness set to true
        //    in the list of options in tree order to false.
        GC::Ptr<HTML::HTMLOptionElement> last_selected_option;
        u64 last_selected_option_update_index = 0;

        for (auto const& option_element : m_cached_list_of_options) {
            if (!option_element->selected())
                continue;
            if (!last_selected_option
                || option_element->selectedness_update_index() > last_selected_option_update_index) {
                last_selected_option = option_element;
                last_selected_option_update_index = option_element->selectedness_update_index();
            }
        }

        for (auto const& option_element : m_cached_list_of_options) {
            if (option_element != last_selected_option)
                option_element->set_selected_internal(false);
        }

        // 2. Set updateSelectedcontent to true.
        should_update_selectedcontent = true;
    }

    // 4. If updateSelectedcontent is true, then run update a select's selectedcontent given element.
    if (should_update_selectedcontent)
        MUST(update_selectedcontent());

    // AD-HOC: The selectedness setting algorithm does not itself refresh the select's fallback button text, but the
    //         set of selected options may have changed. Run the spec's "clone selected option into select button"
    //         algorithm so the button stays in sync.
    clone_selected_option_into_select_button();

    // AD-HOC: A change to the selected option can change whether a required select satisfies its constraints.
    CSS::Invalidation::invalidate_style_after_validity_change(*this);

    return should_update_selectedcontent;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#reset-a-select%27s-selectedness
void HTMLSelectElement::reset_selectedness(HasChanged has_changed)
{
    // 1. If the result of running the selectedness setting algorithm given select is true, then set hasChanged to true.
    // NB: Our selectedness setting algorithm already updated the selectedcontent elements if it changed anything.
    if (update_selectedness())
        return;

    // 2. If hasChanged is false, then return.
    if (has_changed == HasChanged::No)
        return;

    // 3. Update a select's descendant selectedcontent elements given select.
    MUST(update_selectedcontent());
}

bool HTMLSelectElement::is_focusable() const
{
    return enabled() && meets_focusable_area_rendering_requirements();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#placeholder-label-option
HTMLOptionElement* HTMLSelectElement::placeholder_label_option() const
{
    // If a select element has a required attribute specified, and has a display size of 1;
    if (has_attribute(HTML::AttributeNames::required) && display_size() == 1) {
        // and if the value of the first option element in the select element's list of options (if any) is the empty
        // string, and that option element's parent node is the select element (and not an optgroup element), then that
        // option is the select element's placeholder label option.
        auto options = list_of_options();
        if (!options.is_empty()) {
            auto first_option_element = options[0];
            if (first_option_element->value().is_empty() && first_option_element->parent() == this)
                return first_option_element;
        }
    }
    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#select-enabled-selectedcontent
GC::Ptr<HTMLSelectedContentElement> HTMLSelectElement::enabled_selectedcontent() const
{
    // To get a select's enabled selectedcontent given a select element select:

    // 1. If select has the multiple attribute, then return null.
    if (has_attribute(AttributeNames::multiple))
        return nullptr;

    // 2. Let selectedcontent be the first selectedcontent element descendant of select in tree order if any such
    //    element exists; otherwise return null.
    GC::Ptr<HTMLSelectedContentElement> selectedcontent;
    for_each_in_subtree_of_type<HTMLSelectedContentElement>([&](auto& element) {
        selectedcontent = const_cast<HTMLSelectedContentElement*>(&element);
        return TraversalDecision::Break;
    });
    if (!selectedcontent)
        return nullptr;

    // 3. If selectedcontent is disabled, then return null.
    if (selectedcontent->disabled())
        return nullptr;

    // 4. Return selectedcontent.
    return selectedcontent;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#clear-a-select%27s-non-primary-selectedcontent-elements
void HTMLSelectElement::clear_non_primary_selectedcontent()
{
    // To clear a select's non-primary selectedcontent elements, given a select element select:

    // 1. Let passedFirstSelectedcontent be false.
    bool passed_first_selectedcontent = false;

    // 2. For each descendant of select's descendants in tree order that is a selectedcontent element:
    for_each_in_subtree_of_type<HTMLSelectedContentElement>([&](auto& element) {
        // 1. If passedFirstSelectedcontent is false, then set passedFirstSelectedcontent to true.
        if (!passed_first_selectedcontent)
            passed_first_selectedcontent = true;
        // 2. Otherwise, run clear a selectedcontent given descendant.
        else
            element.clear_selectedcontent();

        return TraversalDecision::Continue;
    });
}

// https://html.spec.whatwg.org/multipage/form-elements.html#update-a-select%27s-selectedcontent
WebIDL::ExceptionOr<void> HTMLSelectElement::update_selectedcontent()
{
    // To update a select's selectedcontent given a select element select:

    // 1. Let selectedcontent be the result of get a select's enabled selectedcontent given select.
    auto selectedcontent = enabled_selectedcontent();

    // 2. If selectedcontent is null, then return.
    if (!selectedcontent)
        return {};

    // 3. Let option be the first option in select's list of options whose selectedness is true,
    //    if any such option exists, otherwise null.
    update_cached_list_of_options();
    GC::Ptr<HTML::HTMLOptionElement> option;
    for (auto const& candidate : m_cached_list_of_options) {
        if (candidate->selected()) {
            option = candidate;
            break;
        }
    }

    // 4. If option is null, then run clear a selectedcontent given selectedcontent.
    if (!option) {
        selectedcontent->clear_selectedcontent();
        return {};
    }

    // 5. Otherwise, run clone an option into a selectedcontent given option and selectedcontent.
    TRY(option->clone_into_selectedcontent(*selectedcontent));
    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-select-element%3Asuffering-from-being-missing
bool HTMLSelectElement::suffering_from_being_missing() const
{
    // If the element has its required attribute specified, and either none of the option elements in the select element's list of options have their selectedness
    // set to true, or the only option element in the select element's list of options with its selectedness set to true is the placeholder label option, then the element is suffering from being
    // missing.
    auto selected_options = this->selected_options();
    return has_attribute(HTML::AttributeNames::required) && (selected_options->length() == 0 || (selected_options->length() == 1 && selected_options->item(0) == placeholder_label_option()));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-select-element:concept-fe-mutable
bool HTMLSelectElement::is_mutable() const
{
    // A select element that is not disabled is mutable.
    return enabled();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#option-element-nearest-ancestor-select
GC::Ptr<HTMLSelectElement> get_nearest_ancestor_select(DOM::Element& element)
{
    // 1. Let ancestorOptgroup be null.
    GC::Ptr<HTMLOptGroupElement> ancestor_optgroup;

    // 2. For each ancestor of element's ancestors, in reverse tree order:
    for (auto* ancestor = element.parent(); ancestor; ancestor = ancestor->parent()) {
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

GC::Ptr<HTMLSelectElement const> get_nearest_ancestor_select(DOM::Element const& element)
{
    return get_nearest_ancestor_select(const_cast<DOM::Element&>(element));
}

static constexpr double list_box_row_height_in_em = 1.2;

// https://html.spec.whatwg.org/multipage/rendering.html#the-select-element-2
bool HTMLSelectElement::renders_as_list_box() const
{
    // A select element whose multiple attribute is present, or whose display size is greater than 1, is expected to
    // render as a multi-select list box.
    return has_attribute(AttributeNames::multiple) || display_size() > 1;
}

bool HTMLSelectElement::is_presentational_hint(Utf16FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;
    return first_is_one_of(name, HTML::AttributeNames::size, HTML::AttributeNames::multiple);
}

void HTMLSelectElement::apply_presentational_hints(Vector<CSS::StyleProperty>& properties) const
{
    Base::apply_presentational_hints(properties);

    // AD-HOC: A list box is as tall as the number of rows its display size asks for, whatever options it holds. Its
    //         rows are its contents, so size containment keeps that height through its contain-intrinsic-block-size.
    if (!renders_as_list_box())
        return;
    auto rows = max(display_size(), 1u);
    properties.append({ .property_id = CSS::PropertyID::ContainIntrinsicBlockSize, .value = CSS::LengthStyleValue::create(CSS::Length(rows * list_box_row_height_in_em, CSS::LengthUnit::Em)) });
}

void HTMLSelectElement::create_list_box_shadow_tree(DOM::ShadowRoot& shadow_root)
{
    m_list_box_element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    MUST(shadow_root.append_child(*m_list_box_element));

    auto mousedown_function = JS::NativeFunction::create(
        HTML::relevant_realm(*this), [this](JS::VM& vm) {
            auto event = vm.argument(0).is_object()
                ? Bindings::impl_from<UIEvents::MouseEvent>(&vm.argument(0).as_object())
                : nullptr;
            if (event)
                handle_list_box_mousedown(*event);
            return JS::js_undefined();
        },
        1, Utf16FlyString {}, &HTML::relevant_realm(*this));
    auto mousedown_callback = GC::Heap::the().allocate<WebIDL::CallbackType>(*mousedown_function, HTML::relevant_realm(*this));
    m_list_box_element->add_event_listener_without_options(UIEvents::EventNames::mousedown, DOM::IDLEventListener::create(mousedown_callback));

    update_list_box();
}

void HTMLSelectElement::list_box_options_did_change()
{
    // NB: The rows are built again once for any number of changes made to the options before the next task, so that
    //     inserting the options of a long list one by one does not build its rows over and over.
    if (!m_list_box_element || m_list_box_update_queued)
        return;
    m_list_box_update_queued = true;
    queue_an_element_task(HTML::Task::Source::DOMManipulation, [this] {
        m_list_box_update_queued = false;
        if (m_list_box_element)
            update_list_box();
    });
}

void HTMLSelectElement::update_list_box_row_styles()
{
    for (auto const& row : m_list_box_rows)
        update_list_box_row_style(row.element, row.option);
}

void HTMLSelectElement::did_receive_focus()
{
    m_list_box_is_focused = true;
    update_list_box_row_styles();
}

void HTMLSelectElement::did_lose_focus()
{
    m_list_box_is_focused = false;
    update_list_box_row_styles();
}

void HTMLSelectElement::list_box_option_selectedness_did_change(HTMLOptionElement const& option)
{
    if (!m_list_box_element)
        return;
    for (auto const& row : m_list_box_rows) {
        if (row.option.ptr() == &option) {
            update_list_box_row_style(row.element, option);
            return;
        }
    }
}

void HTMLSelectElement::update_list_box_row_style(DOM::Element& row, HTMLOptionElement const& option) const
{
    StringBuilder style;
    style.append("display: block; min-block-size: 1.2em; line-height: 1.2em; padding-inline: 4px; user-select: none;"sv);
    if (as_if<HTMLOptGroupElement>(option.parent()))
        style.append(" padding-inline-start: 1.25em;"sv);
    // NB: The selection of a list box without focus is gray, so that it stands out less than one the keys move.
    if (option.selected() && m_list_box_is_focused)
        style.append(" background-color: AccentColor; color: AccentColorText;"sv);
    else if (option.selected())
        style.append(" background-color: color-mix(in srgb, currentcolor 22%, transparent);"sv);
    else if (option.disabled())
        style.append(" color: GrayText;"sv);
    row.set_attribute_value(HTML::AttributeNames::style, Utf16String::from_utf8(style.string_view()));
}

void HTMLSelectElement::update_list_box()
{
    VERIFY(m_list_box_element);

    auto rows = max(display_size(), 1u);
    m_list_box_element->set_attribute_value(HTML::AttributeNames::style, Utf16String::formatted("block-size: {}em; overflow-x: hidden; overflow-y: auto;", rows * list_box_row_height_in_em));

    m_list_box_rows.clear();
    m_list_box_anchor_index = {};
    m_list_box_focus_index = {};
    m_list_box_element->replace_all(nullptr);

    auto append_option_row = [&](HTMLOptionElement& option) {
        if (option.has_attribute(AttributeNames::hidden))
            return;
        auto row = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
        MUST(row->append_child(document().create_text_node(Infra::strip_and_collapse_whitespace(option.label()))));
        update_list_box_row_style(row, option);
        MUST(m_list_box_element->append_child(row));
        m_list_box_rows.append({ row, option });
    };

    for (auto const& child : children_as_vector()) {
        if (auto* option = as_if<HTMLOptionElement>(*child)) {
            append_option_row(*option);
        } else if (auto* group = as_if<HTMLOptGroupElement>(*child)) {
            if (group->has_attribute(AttributeNames::hidden))
                continue;
            auto label_row = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
            label_row->set_attribute_value(HTML::AttributeNames::style, "display: block; min-block-size: 1.2em; line-height: 1.2em; padding-inline: 4px; font-weight: bold; user-select: none;"_utf16);
            MUST(label_row->append_child(document().create_text_node(group->get_attribute(AttributeNames::label).value_or({}))));
            MUST(m_list_box_element->append_child(label_row));
            for (auto const& group_child : group->children_as_vector()) {
                if (auto* group_option = as_if<HTMLOptionElement>(*group_child))
                    append_option_row(*group_option);
            }
        }
    }
}

void HTMLSelectElement::handle_list_box_mousedown(UIEvents::MouseEvent& event)
{
    if (event.button() != 0 || !is_mutable())
        return;

    // Find the row that was pressed, as the child of the list box that holds the event's target.
    auto* node = as_if<DOM::Node>(event.target().ptr());
    while (node && node->parent() != m_list_box_element.ptr())
        node = node->parent();
    if (!node)
        return;

    Optional<size_t> index;
    for (size_t i = 0; i < m_list_box_rows.size(); ++i) {
        if (m_list_box_rows[i].element.ptr() == node) {
            index = i;
            break;
        }
    }
    if (!index.has_value() || m_list_box_rows[*index].option->disabled())
        return;

    // NB: Pressing an option chooses it. In a multiple select, a press with Ctrl or Cmd held toggles the option, and a
    //     press with Shift held chooses the options from the last one pressed up to this one.
    bool const multiple = has_attribute(AttributeNames::multiple);
    bool const toggle = multiple && (event.ctrl_key() || event.meta_key());
    if (!toggle) {
        choose_list_box_row(*index, multiple && event.shift_key());
        return;
    }

    auto& option = m_list_box_rows[*index].option;
    option->set_selected(!option->selected());
    m_list_box_anchor_index = *index;
    m_list_box_focus_index = *index;
    send_select_update_notifications();
}

void HTMLSelectElement::choose_list_box_row(size_t index, bool extend)
{
    update_cached_list_of_options();
    Vector<bool> was_selected;
    for (auto const& option : m_cached_list_of_options)
        was_selected.append(option->selected());

    // NB: Set the selectedness and dirtiness of each option directly, then run the selectedness setting algorithm
    //     once, instead of letting every option ask for a reset.
    auto set_selectedness = [](HTMLOptionElement& option, bool selected) {
        option.set_selected_internal(selected);
        option.m_dirty = true;
    };
    for (auto const& option : m_cached_list_of_options)
        set_selectedness(*option, false);
    if (extend && m_list_box_anchor_index.has_value()) {
        auto first = min(*m_list_box_anchor_index, index);
        auto last = max(*m_list_box_anchor_index, index);
        for (size_t i = first; i <= last; ++i) {
            if (!m_list_box_rows[i].option->disabled())
                set_selectedness(*m_list_box_rows[i].option, true);
        }
    } else {
        set_selectedness(*m_list_box_rows[index].option, true);
        m_list_box_anchor_index = index;
    }
    update_selectedness();
    m_list_box_focus_index = index;
    Bindings::ScrollIntoViewOptions scroll_options;
    scroll_options.block = Bindings::ScrollLogicalPosition::Nearest;
    scroll_options.inline_ = Bindings::ScrollLogicalPosition::Nearest;
    m_list_box_rows[index].element->scroll_into_view(scroll_options, nullptr);

    bool changed = false;
    for (size_t i = 0; i < m_cached_list_of_options.size(); ++i) {
        if (m_cached_list_of_options[i]->selected() != was_selected[i])
            changed = true;
    }
    if (changed)
        send_select_update_notifications();
}

bool HTMLSelectElement::handle_list_box_key(UIEvents::KeyCode key, u32 modifiers)
{
    if (m_list_box_rows.is_empty() || !is_mutable())
        return false;
    if (modifiers != UIEvents::Mod_None && modifiers != UIEvents::Mod_Shift)
        return false;

    // NB: The keys move from the option last chosen, or from the first selected option.
    Optional<size_t> current = m_list_box_focus_index;
    if (!current.has_value()) {
        for (size_t i = 0; i < m_list_box_rows.size(); ++i) {
            if (m_list_box_rows[i].option->selected()) {
                current = i;
                break;
            }
        }
    }

    auto is_choosable = [&](size_t i) { return !m_list_box_rows[i].option->disabled(); };
    Optional<size_t> target;
    switch (key) {
    case UIEvents::KeyCode::Key_Down:
        for (size_t i = current.has_value() ? *current + 1 : 0; i < m_list_box_rows.size(); ++i) {
            if (is_choosable(i)) {
                target = i;
                break;
            }
        }
        break;
    case UIEvents::KeyCode::Key_Up:
        // NB: Without a current option, Up starts from the end of the list.
        for (size_t i = current.value_or(m_list_box_rows.size()); i-- > 0;) {
            if (is_choosable(i)) {
                target = i;
                break;
            }
        }
        break;
    case UIEvents::KeyCode::Key_Home:
        for (size_t i = 0; i < m_list_box_rows.size(); ++i) {
            if (is_choosable(i)) {
                target = i;
                break;
            }
        }
        break;
    case UIEvents::KeyCode::Key_End:
        for (size_t i = m_list_box_rows.size(); i-- > 0;) {
            if (is_choosable(i)) {
                target = i;
                break;
            }
        }
        break;
    default:
        return false;
    }

    if (target.has_value())
        choose_list_box_row(*target, has_attribute(AttributeNames::multiple) && modifiers == UIEvents::Mod_Shift);
    return true;
}

}
