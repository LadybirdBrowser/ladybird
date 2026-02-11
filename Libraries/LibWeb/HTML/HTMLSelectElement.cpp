/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLSelectElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLDataListElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLHRElement.h>
#include <LibWeb/HTML/HTMLOptGroupElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLSelectedContentElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLSelectElement);

HTMLSelectElement::HTMLSelectElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags {
        .supports_indexed_properties = true,
        .has_indexed_property_setter = true,
        .indexed_property_setter_has_identifier = true,
    };
}

HTMLSelectElement::~HTMLSelectElement() = default;

void HTMLSelectElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLSelectElement);
    Base::initialize(realm);
}

void HTMLSelectElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_options);
    visitor.visit(m_selected_options);
    visitor.visit(m_inner_text_element);
    visitor.visit(m_chevron_icon_element);
    visitor.visit(m_cached_list_of_options);

    for (auto const& item : m_select_items) {
        if (item.has<SelectItemOption>())
            visitor.visit(item.get<SelectItemOption>().option_element);

        if (item.has<SelectItemOptionGroup>()) {
            auto item_option_group = item.get<SelectItemOptionGroup>();
            for (auto const& item : item_option_group.items)
                visitor.visit(item.option_element);
        }
    }
}

void HTMLSelectElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));

    // AD-HOC: We rewrite `display: inline` to `display: inline-block`.
    //         This is required for the internal shadow tree to work correctly in layout.
    if (style.display().is_inline_outside() && style.display().is_flow_inside())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::InlineBlock)));

    // AD-HOC: Enforce normal line-height for select elements. This matches the behavior of other engines.
    style.set_property(CSS::PropertyID::LineHeight, CSS::KeywordStyleValue::create(CSS::Keyword::Normal));
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
    set_attribute_value(HTML::AttributeNames::size, String::number(size));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-options
GC::Ptr<HTMLOptionsCollection> const& HTMLSelectElement::options() const
{
    // The options IDL attribute must return an HTMLOptionsCollection rooted at the select node,
    // whose filter matches the elements in the list of options.
    if (!m_options) {
        m_options = HTMLOptionsCollection::create(const_cast<HTMLSelectElement&>(*this), [this](DOM::Element const& element) {
            auto const* maybe_option = as_if<HTML::HTMLOptionElement>(element);
            return maybe_option && maybe_option->nearest_select_element() == this;
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

// https://html.spec.whatwg.org/multipage/form-elements.html#the-select-element:htmlselectelement
Optional<JS::Value> HTMLSelectElement::item_value(size_t index) const
{
    // The options collection is also mirrored on the HTMLSelectElement object. The supported property indices at any
    // instant are the indices supported by the object returned by the options attribute at that instant.
    return (const_cast<HTMLOptionsCollection&>(*options()).item_value(index));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-nameditem
HTMLOptionElement* HTMLSelectElement::named_item(FlyString const& name)
{
    // The namedItem(name) method must return the value returned by the method of the same name on the options collection, when invoked with the same argument.
    return as<HTMLOptionElement>(const_cast<HTMLOptionsCollection&>(*options()).named_item(name));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-add
WebIDL::ExceptionOr<void> HTMLSelectElement::add(HTMLOptionOrOptGroupElement element, Optional<HTMLElementOrElementIndex> before)
{
    // Similarly, the add(element, before) method must act like its namesake method on that same options collection.
    TRY(const_cast<HTMLOptionsCollection&>(*options()).add(move(element), move(before)));

    update_selectedness(); // Not in spec

    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-select-element:set-the-value-of-a-new-indexed-property
WebIDL::ExceptionOr<void> HTMLSelectElement::set_value_of_indexed_property(u32 n, JS::Value new_value)
{
    // When the user agent is to set the value of a new indexed property or set the value of an existing indexed property
    // for a select element, it must instead run the corresponding algorithm on the select element's options collection.
    TRY(const_cast<HTMLOptionsCollection&>(*options()).set_value_of_indexed_property(n, new_value));

    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-remove
void HTMLSelectElement::remove()
{
    // The remove() method must act like its namesake method on that same options collection when it has arguments,
    // and like its namesake method on the ChildNode interface implemented by the HTMLSelectElement ancestor interface Element when it has no arguments.
    ChildNode::remove_binding();
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
        m_selected_options = DOM::HTMLCollection::create(*this, DOM::HTMLCollection::Scope::Descendants, [this](Element const& element) {
            auto const* maybe_option = as_if<HTML::HTMLOptionElement>(element);
            if (maybe_option && maybe_option->nearest_select_element() == this) {
                return maybe_option->selected();
            }
            return false;
        });
    }
    return *m_selected_options;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-select-option-list
void HTMLSelectElement::update_cached_list_of_options() const
{
    // 1. Let options be « ».
    m_cached_list_of_options.clear();
    m_cached_number_of_selected_options = 0;

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
        if (auto maybe_option = as_if<HTMLOptionElement>(node)) {
            if (maybe_option->selected())
                ++m_cached_number_of_selected_options;
            m_cached_list_of_options.append(const_cast<HTMLOptionElement&>(*maybe_option));
        }

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
    ScopeGuard guard { [&]() { update_inner_text_element(); } };

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

    if (m_cached_number_of_selected_options >= 2)
        return false;

    if (display_size() == 1 && m_cached_number_of_selected_options == 0)
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

void HTMLSelectElement::children_changed(ChildrenChangedMetadata const* metadata)
{
    Base::children_changed(metadata);

    if (metadata && can_skip_children_changed_selectedness_update(*metadata))
        return;

    update_cached_list_of_options();
    update_selectedness();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-type
String const& HTMLSelectElement::type() const
{
    // The type IDL attribute, on getting, must return the string "select-one" if the multiple attribute is absent, and the string "select-multiple" if the multiple attribute is present.
    static String const select_one = "select-one"_string;
    static String const select_multiple = "select-multiple"_string;

    if (!has_attribute(AttributeNames::multiple))
        return select_one;

    return select_multiple;
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
WebIDL::ExceptionOr<void> HTMLSelectElement::set_value(Utf16String const& value)
{
    // The value setter steps are:
    ScopeGuard guard { [&]() { update_inner_text_element(); } };
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
    queue_an_element_task(HTML::Task::Source::UserInteraction, [this] {
        // 1. Set the select element's user validity to true.
        m_user_validity = true;

        // 2. Run update a select's selectedcontent given element.
        MUST(update_selectedcontent());

        // FIXME: 3. Run clone selected option into select button given element.

        // 4. Fire an event named input at element, with the bubbles and composed attributes initialized to true.
        auto input_event = DOM::Event::create(realm(), HTML::EventNames::input);
        input_event->set_bubbles(true);
        input_event->set_composed(true);
        dispatch_event(input_event);

        // 5. Fire an event named change at element, with the bubbles attribute initialized to true.
        auto change_event = DOM::Event::create(realm(), HTML::EventNames::change);
        change_event->set_bubbles(true);
        dispatch_event(*change_event);
    });
}

void HTMLSelectElement::set_is_open(bool open)
{
    if (open == m_is_open)
        return;

    m_is_open = open;
    invalidate_style(DOM::StyleInvalidationReason::HTMLSelectElementSetIsOpen);
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
    auto& relevant_global = as<HTML::Window>(relevant_global_object(*this));
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
    m_select_items.clear();
    u32 id_counter = 1;
    for (auto const& child : children_as_vector()) {
        if (auto const* opt_group_element = as_if<HTMLOptGroupElement>(*child)) {
            if (!opt_group_element->has_attribute(Web::HTML::AttributeNames::hidden)) {
                Vector<SelectItemOption> option_group_items;
                for (auto const& child : opt_group_element->children_as_vector()) {
                    if (auto const& option_element = as_if<HTMLOptionElement>(*child)) {
                        if (!option_element->has_attribute(Web::HTML::AttributeNames::hidden))
                            option_group_items.append(SelectItemOption { id_counter++, option_element->selected(), option_element->disabled(), option_element, MUST(Infra::strip_and_collapse_whitespace(option_element->label())), option_element->value().to_utf8_but_should_be_ported_to_utf16() });
                    }
                }
                m_select_items.append(SelectItemOptionGroup { opt_group_element->get_attribute(AttributeNames::label).value_or(String {}), option_group_items });
            }
        }

        if (auto const& option_element = as_if<HTMLOptionElement>(*child)) {
            if (!option_element->has_attribute(Web::HTML::AttributeNames::hidden))
                m_select_items.append(SelectItemOption { id_counter++, option_element->selected(), option_element->disabled(), option_element, MUST(Infra::strip_and_collapse_whitespace(option_element->label())), option_element->value().to_utf8_but_should_be_ported_to_utf16() });
        }

        if (auto const* hr_element = as_if<HTMLHRElement>(*child)) {
            if (!hr_element->has_attribute(Web::HTML::AttributeNames::hidden))
                m_select_items.append(SelectItemSeparator {});
        }
    }

    // Request select dropdown
    auto weak_element = GC::Weak<HTMLSelectElement> { *this };
    auto rect = get_bounding_client_rect();
    auto position = document().navigable()->to_top_level_position(Web::CSSPixelPoint { rect.x(), rect.bottom() });
    document().page().did_request_select_dropdown(weak_element, position, rect.width(), m_select_items);
    set_is_open(true);
}

// https://html.spec.whatwg.org/multipage/input.html#dom-select-showpicker
WebIDL::ExceptionOr<void> HTMLSelectElement::show_picker()
{
    // FIXME: Deduplicate with HTMLInputElement
    // The showPicker() method steps are:

    // 1. If this is not mutable, then throw an "InvalidStateError" DOMException.
    if (!is_mutable())
        return WebIDL::InvalidStateError::create(realm(), "Element is not mutable"_utf16);

    // 2. If this's relevant settings object's origin is not same origin with this's relevant settings object's top-level origin,
    //    and this is a select element, then throw a "SecurityError" DOMException.
    if (!relevant_settings_object(*this).origin().is_same_origin(relevant_settings_object(*this).top_level_origin.value())) {
        return WebIDL::SecurityError::create(realm(), "Cross origin pickers are not allowed"_utf16);
    }

    // 3. If this's relevant global object does not have transient activation, then throw a "NotAllowedError" DOMException.
    auto& global_object = relevant_global_object(*this);
    if (!as<HTML::Window>(global_object).has_transient_activation()) {
        return WebIDL::NotAllowedError::create(realm(), "Too long since user activation to show picker"_utf16);
    }

    // FIXME: 4. If this is a select element, and this is not being rendered, then throw a "NotSupportedError" DOMException.

    // 5. Show the picker, if applicable, for this.
    show_the_picker_if_applicable();
    return {};
}

void HTMLSelectElement::activation_behavior(DOM::Event const& event)
{
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

    for (auto const& item : m_select_items) {
        if (item.has<SelectItemOption>()) {
            auto const& item_option = item.get<SelectItemOption>();
            if (item_option.id == *id)
                item_option.option_element->set_selected(true);
        }
        if (item.has<SelectItemOptionGroup>()) {
            auto item_option_group = item.get<SelectItemOptionGroup>();
            for (auto const& item_option : item_option_group.items) {
                if (item_option.id == *id)
                    item_option.option_element->set_selected(true);
            }
        }
    }

    update_inner_text_element();
    send_select_update_notifications();
}

void HTMLSelectElement::form_associated_element_was_inserted()
{
    create_shadow_tree_if_needed();
}

void HTMLSelectElement::form_associated_element_attribute_changed(FlyString const& name, Optional<String> const&, Optional<String> const& value, Optional<FlyString> const&)
{
    if (name == HTML::AttributeNames::multiple) {
        // If the multiple attribute is absent then update the selectedness of the option elements.
        if (!value.has_value()) {
            update_selectedness();
        }
    }
}

void HTMLSelectElement::computed_properties_changed()
{
    // Hide chevron icon when appearance is none
    if (m_chevron_icon_element) {
        auto appearance = computed_properties()->appearance();
        if (appearance == CSS::Appearance::None) {
            MUST(m_chevron_icon_element->style_for_bindings()->set_property(CSS::PropertyID::Display, "none"_string));
        } else {
            MUST(m_chevron_icon_element->style_for_bindings()->set_property(CSS::PropertyID::Display, "block"_string));
        }
    }
}

void HTMLSelectElement::create_shadow_tree_if_needed()
{
    if (shadow_root())
        return;

    auto shadow_root = realm().create<DOM::ShadowRoot>(document(), *this, Bindings::ShadowRootMode::Closed);
    shadow_root->set_user_agent_internal(true);
    set_shadow_root(shadow_root);

    auto border = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    border->set_attribute_value(HTML::AttributeNames::style, R"~~~(
        display: flex;
        align-items: center;
        height: 100%;
    )~~~"_string);
    MUST(shadow_root->append_child(border));

    m_inner_text_element = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    m_inner_text_element->set_attribute_value(HTML::AttributeNames::style, R"~~~(
        flex: 1;
    )~~~"_string);
    MUST(border->append_child(*m_inner_text_element));

    m_chevron_icon_element = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    m_chevron_icon_element->set_attribute_value(HTML::AttributeNames::style, R"~~~(
        width: 16px;
        height: 16px;
        margin-left: 4px;
    )~~~"_string);

    auto chevron_svg_element = DOM::create_element(document(), SVG::TagNames::svg, Namespace::SVG).release_value_but_fixme_should_propagate_errors();
    chevron_svg_element->set_attribute_value(SVG::AttributeNames::xmlns, Namespace::SVG.to_string());
    chevron_svg_element->set_attribute_value(SVG::AttributeNames::viewBox, "0 0 24 24"_string);
    MUST(m_chevron_icon_element->append_child(chevron_svg_element));

    auto chevron_path_element = DOM::create_element(document(), SVG::TagNames::path, Namespace::SVG).release_value_but_fixme_should_propagate_errors();
    chevron_path_element->set_attribute_value(SVG::AttributeNames::fill, "currentcolor"_string);
    chevron_path_element->set_attribute_value(SVG::AttributeNames::d, "M7.41,8.58L12,13.17L16.59,8.58L18,10L12,16L6,10L7.41,8.58Z"_string);
    MUST(chevron_svg_element->append_child(chevron_path_element));

    MUST(border->append_child(*m_chevron_icon_element));

    update_inner_text_element();
}

void HTMLSelectElement::update_inner_text_element(Badge<HTMLOptionElement>)
{
    update_cached_list_of_options();
    update_inner_text_element();
}

// FIXME: This needs to be called any time the selected option's children are modified.
void HTMLSelectElement::update_inner_text_element()
{
    if (!m_inner_text_element)
        return;

    // Update inner text element to the label of the selected option
    for (auto const& option_element : m_cached_list_of_options) {
        if (option_element->selected()) {
            m_inner_text_element->string_replace_all(Infra::strip_and_collapse_whitespace(Utf16String::from_utf8(option_element->label())));
            return;
        }
    }
}

// https://html.spec.whatwg.org/multipage/form-elements.html#selectedness-setting-algorithm
// https://whatpr.org/html/11890/form-elements.html#selectedness-setting-algorithm
void HTMLSelectElement::update_selectedness()
{
    // The selectedness setting algorithm, given a select element element, is to run the following steps:
    update_cached_list_of_options();

    // 1. Let updateSelectedcontent be false.
    auto should_update_selectedcontent = false;

    // 2. If element 's multiple attribute is absent, and element's display size is 1,
    //    and no option elements in the element's list of options have their selectedness set to true, then
    if (!has_attribute(AttributeNames::multiple) && display_size() == 1 && m_cached_number_of_selected_options == 0) {
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
    else if (!has_attribute(AttributeNames::multiple) && m_cached_number_of_selected_options >= 2) {
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
    if (should_update_selectedcontent) {
        MUST(update_selectedcontent());
        update_inner_text_element();
    }
}

bool HTMLSelectElement::is_focusable() const
{
    return enabled();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#placeholder-label-option
HTMLOptionElement* HTMLSelectElement::placeholder_label_option() const
{
    // If a select element has a required attribute specified, does not have a multiple attribute specified, and has a display size of 1;
    if (has_attribute(HTML::AttributeNames::required) && !has_attribute(HTML::AttributeNames::multiple) && display_size() == 1) {
        // and if the value of the first option element in the
        // select element's list of options (if any) is the empty string, and that option element's parent node is the select element (and not an optgroup element), then that option is the
        // select element's placeholder label option.
        auto first_option_element = list_of_options()[0];
        if (first_option_element->value().is_empty() && first_option_element->parent() == this)
            return first_option_element;
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

}
