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
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLHRElement.h>
#include <LibWeb/HTML/HTMLOptGroupElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
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
}

HTMLSelectElement::~HTMLSelectElement() = default;

void HTMLSelectElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLSelectElement);
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

WebIDL::ExceptionOr<void> HTMLSelectElement::set_size(WebIDL::UnsignedLong size)
{
    if (size > 2147483647)
        size = 0;
    return set_attribute(HTML::AttributeNames::size, String::number(size));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-options
GC::Ptr<HTMLOptionsCollection> const& HTMLSelectElement::options()
{
    if (!m_options) {
        m_options = HTMLOptionsCollection::create(*this, [](DOM::Element const& element) {
            // https://html.spec.whatwg.org/multipage/form-elements.html#concept-select-option-list
            // The list of options for a select element consists of all the option element children of
            // the select element, and all the option element children of all the optgroup element children
            // of the select element, in tree order.
            return is<HTMLOptionElement>(element);
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
        m_selected_options = DOM::HTMLCollection::create(*this, DOM::HTMLCollection::Scope::Descendants, [](Element const& element) {
            if (is<HTML::HTMLOptionElement>(element)) {
                auto const& option_element = as<HTMLOptionElement>(element);
                return option_element.selected();
            }
            return false;
        });
    }
    return *m_selected_options;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-select-option-list
void HTMLSelectElement::update_cached_list_of_options() const
{
    // The list of options for a select element consists of all the option element children of the select element,
    // and all the option element children of all the optgroup element children of the select element, in tree order.
    m_cached_list_of_options.clear();
    m_cached_number_of_selected_options = 0;

    for (auto* node = first_child(); node; node = node->next_sibling()) {
        if (auto* maybe_option = as_if<HTMLOptionElement>(*node)) {
            if (maybe_option->selected())
                ++m_cached_number_of_selected_options;
            m_cached_list_of_options.append(const_cast<HTMLOptionElement&>(*maybe_option));
            continue;
        }

        if (auto* maybe_opt_group = as_if<HTMLOptGroupElement>(node)) {
            maybe_opt_group->for_each_child_of_type<HTMLOptionElement>([&](HTMLOptionElement& option_element) {
                if (option_element.selected())
                    ++m_cached_number_of_selected_options;
                m_cached_list_of_options.append(option_element);
                return IterationDecision::Continue;
            });
        }
    }
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
    // The selectedIndex IDL attribute, on getting, must return the index of the first option element in the list of options
    // in tree order that has its selectedness set to true, if any. If there isn't one, then it must return −1.
    update_cached_list_of_options();

    WebIDL::Long index = 0;
    for (auto const& option_element : m_cached_list_of_options) {
        if (option_element->selected())
            return index;
        ++index;
    }
    return -1;
}

void HTMLSelectElement::set_selected_index(WebIDL::Long index)
{
    update_cached_list_of_options();
    // On setting, the selectedIndex attribute must set the selectedness of all the option elements in the list of options to false,
    // and then the option element in the list of options whose index is the given new value,
    // if any, must have its selectedness set to true and its dirtiness set to true.
    for (auto& option : m_cached_list_of_options)
        option->set_selected_internal(false);

    if (index < 0 || static_cast<size_t>(index) >= m_cached_list_of_options.size())
        return;

    auto& selected_option = m_cached_list_of_options[index];
    selected_option->set_selected_internal(true);
    selected_option->m_dirty = true;
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

String HTMLSelectElement::value() const
{
    update_cached_list_of_options();
    for (auto const& option_element : m_cached_list_of_options)
        if (option_element->selected())
            return option_element->value();
    return ""_string;
}

WebIDL::ExceptionOr<void> HTMLSelectElement::set_value(String const& value)
{
    update_cached_list_of_options();
    for (auto const& option_element : m_cached_list_of_options)
        option_element->set_selected(option_element->value() == value);
    update_inner_text_element();
    return {};
}

void HTMLSelectElement::queue_input_and_change_events()
{
    // When the user agent is to send select update notifications, queue an element task on the user interaction task source given the select element to run these steps:
    queue_an_element_task(HTML::Task::Source::UserInteraction, [this] {
        // 1. Set the select element's user validity to true.
        m_user_validity = true;

        // 2. Fire an event named input at the select element, with the bubbles and composed attributes initialized to true.
        auto input_event = DOM::Event::create(realm(), HTML::EventNames::input);
        input_event->set_bubbles(true);
        input_event->set_composed(true);
        dispatch_event(input_event);

        // 3. Fire an event named change at the select element, with the bubbles attribute initialized to true.
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

static String strip_newlines(Optional<String> string)
{
    // FIXME: Move this to a more general function
    if (!string.has_value())
        return {};

    StringBuilder builder;
    for (auto c : string.value().bytes_as_string_view()) {
        if (c == '\r' || c == '\n') {
            builder.append(' ');
        } else {
            builder.append(c);
        }
    }
    return MUST(Infra::strip_and_collapse_whitespace(MUST(builder.to_string())));
}

// https://html.spec.whatwg.org/multipage/input.html#show-the-picker,-if-applicable
void HTMLSelectElement::show_the_picker_if_applicable()
{
    // FIXME: Deduplicate with HTMLInputElement
    // To show the picker, if applicable for a select element:

    // 1. If element's relevant global object does not have transient activation, then return.
    auto& relevant_global = as<HTML::Window>(relevant_global_object(*this));
    if (!relevant_global.has_transient_activation())
        return;

    // 2. If element is not mutable, then return.
    if (!enabled())
        return;

    // 3. Consume user activation given element's relevant global object.
    relevant_global.consume_user_activation();

    // 4. If element's type attribute is in the File Upload state, then run these steps in parallel:
    // Not Applicable to select elements

    // 5. Otherwise, the user agent should show any relevant user interface for selecting a value for element,
    //    in the way it normally would when the user interacts with the control. (If no such UI applies to element, then this step does nothing.)
    //    If such a user interface is shown, it must respect the requirements stated in the relevant parts of the specification for how element
    //    behaves given its type attribute state. (For example, various sections describe restrictions on the resulting value string.)
    //    This step can have side effects, such as closing other pickers that were previously shown by this algorithm.
    //    (If this closes a file selection picker, then per the above that will lead to firing either input and change events, or a cancel event.)

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
                            option_group_items.append(SelectItemOption { id_counter++, option_element->selected(), option_element->disabled(), option_element, strip_newlines(option_element->label()), option_element->value() });
                    }
                }
                m_select_items.append(SelectItemOptionGroup { opt_group_element->get_attribute(AttributeNames::label).value_or(String {}), option_group_items });
            }
        }

        if (auto const& option_element = as_if<HTMLOptionElement>(*child)) {
            if (!option_element->has_attribute(Web::HTML::AttributeNames::hidden))
                m_select_items.append(SelectItemOption { id_counter++, option_element->selected(), option_element->disabled(), option_element, strip_newlines(option_element->label()), option_element->value() });
        }

        if (auto const* hr_element = as_if<HTMLHRElement>(*child)) {
            if (!hr_element->has_attribute(Web::HTML::AttributeNames::hidden))
                m_select_items.append(SelectItemSeparator {});
        }
    }

    // Request select dropdown
    auto weak_element = make_weak_ptr<HTMLSelectElement>();
    auto rect = get_bounding_client_rect();
    auto position = document().navigable()->to_top_level_position(Web::CSSPixelPoint { rect->x(), rect->y() + rect->height() });
    document().page().did_request_select_dropdown(weak_element, position, CSSPixels(rect->width()), m_select_items);
    set_is_open(true);
}

// https://html.spec.whatwg.org/multipage/input.html#dom-select-showpicker
WebIDL::ExceptionOr<void> HTMLSelectElement::show_picker()
{
    // FIXME: Deduplicate with HTMLInputElement
    // The showPicker() method steps are:

    // 1. If this is not mutable, then throw an "InvalidStateError" DOMException.
    if (!enabled())
        return WebIDL::InvalidStateError::create(realm(), "Element is not mutable"_string);

    // 2. If this's relevant settings object's origin is not same origin with this's relevant settings object's top-level origin,
    // and this is a select element, then throw a "SecurityError" DOMException.
    if (!relevant_settings_object(*this).origin().is_same_origin(relevant_settings_object(*this).top_level_origin)) {
        return WebIDL::SecurityError::create(realm(), "Cross origin pickers are not allowed"_string);
    }

    // 3. If this's relevant global object does not have transient activation, then throw a "NotAllowedError" DOMException.
    auto& global_object = relevant_global_object(*this);
    if (!as<HTML::Window>(global_object).has_transient_activation()) {
        return WebIDL::NotAllowedError::create(realm(), "Too long since user activation to show picker"_string);
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
    queue_input_and_change_events();
}

void HTMLSelectElement::form_associated_element_was_inserted()
{
    create_shadow_tree_if_needed();
}

void HTMLSelectElement::form_associated_element_was_removed(DOM::Node*)
{
    set_shadow_root(nullptr);
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
    set_shadow_root(shadow_root);

    auto border = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    MUST(border->set_attribute(HTML::AttributeNames::style, R"~~~(
        display: flex;
        align-items: center;
        height: 100%;
    )~~~"_string));
    MUST(shadow_root->append_child(border));

    m_inner_text_element = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    MUST(m_inner_text_element->set_attribute(HTML::AttributeNames::style, R"~~~(
        flex: 1;
    )~~~"_string));
    MUST(border->append_child(*m_inner_text_element));

    // FIXME: Find better way to add chevron icon
    static constexpr auto chevron_svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\"><path fill=\"currentcolor\" d=\"M7.41,8.58L12,13.17L16.59,8.58L18,10L12,16L6,10L7.41,8.58Z\"/></svg>"sv;

    m_chevron_icon_element = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    MUST(m_chevron_icon_element->set_attribute(HTML::AttributeNames::style, R"~~~(
        width: 16px;
        height: 16px;
        margin-left: 4px;
    )~~~"_string));
    MUST(m_chevron_icon_element->set_inner_html(chevron_svg));
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
            m_inner_text_element->set_text_content(strip_newlines(option_element->label()));
            return;
        }
    }
}

// https://html.spec.whatwg.org/multipage/form-elements.html#selectedness-setting-algorithm
void HTMLSelectElement::update_selectedness()
{
    if (has_attribute(AttributeNames::multiple))
        return;

    update_cached_list_of_options();

    // If element's multiple attribute is absent, and element's display size is 1,
    if (display_size() == 1) {
        // and no option elements in the element's list of options have their selectedness set to true,
        if (m_cached_number_of_selected_options == 0) {
            // then set the selectedness of the first option element in the list of options in tree order
            // that is not disabled, if any, to true, and return.
            for (auto const& option_element : m_cached_list_of_options) {
                if (!option_element->disabled()) {
                    option_element->set_selected_internal(true);
                    update_inner_text_element();
                    break;
                }
            }
            return;
        }
    }

    // If element's multiple attribute is absent,
    // and two or more option elements in element's list of options have their selectedness set to true,
    // then set the selectedness of all but the last option element with its selectedness set to true
    // in the list of options in tree order to false.
    if (m_cached_number_of_selected_options >= 2) {
        // then set the selectedness of all but the last option element with its selectedness set to true
        // in the list of options in tree order to false.
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
    }
    update_inner_text_element();
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

// https://html.spec.whatwg.org/multipage/form-elements.html#the-select-element%3Asuffering-from-being-missing
bool HTMLSelectElement::suffering_from_being_missing() const
{
    // If the element has its required attribute specified, and either none of the option elements in the select element's list of options have their selectedness
    // set to true, or the only option element in the select element's list of options with its selectedness set to true is the placeholder label option, then the element is suffering from being
    // missing.
    auto selected_options = this->selected_options();
    return has_attribute(HTML::AttributeNames::required) && (selected_options->length() == 0 || (selected_options->length() == 1 && selected_options->item(0) == placeholder_label_option()));
}

}
