/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLSelectElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
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

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLSelectElement);

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

void HTMLSelectElement::adjust_computed_style(CSS::StyleProperties& style)
{
    // AD-HOC: We rewrite `display: inline` to `display: inline-block`.
    //         This is required for the internal shadow tree to work correctly in layout.
    if (style.display().is_inline_outside() && style.display().is_flow_inside())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::InlineBlock)));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-size
WebIDL::UnsignedLong HTMLSelectElement::size() const
{
    // The size IDL attribute must reflect the respective content attributes of the same name. The size IDL attribute has a default value of 0.
    if (auto size_string = get_attribute(HTML::AttributeNames::size); size_string.has_value()) {
        if (auto size = parse_non_negative_integer(*size_string); size.has_value())
            return *size;
    }
    return 0;
}

WebIDL::ExceptionOr<void> HTMLSelectElement::set_size(WebIDL::UnsignedLong size)
{
    return set_attribute(HTML::AttributeNames::size, MUST(String::number(size)));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-options
JS::GCPtr<HTMLOptionsCollection> const& HTMLSelectElement::options()
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
    return verify_cast<HTMLOptionElement>(const_cast<HTMLOptionsCollection&>(*options()).item(index));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-nameditem
HTMLOptionElement* HTMLSelectElement::named_item(FlyString const& name)
{
    // The namedItem(name) method must return the value returned by the method of the same name on the options collection, when invoked with the same argument.
    return verify_cast<HTMLOptionElement>(const_cast<HTMLOptionsCollection&>(*options()).named_item(name));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-add
WebIDL::ExceptionOr<void> HTMLSelectElement::add(HTMLOptionOrOptGroupElement element, Optional<HTMLElementOrElementIndex> before)
{
    // Similarly, the add(element, before) method must act like its namesake method on that same options collection.
    return const_cast<HTMLOptionsCollection&>(*options()).add(move(element), move(before));
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
JS::NonnullGCPtr<DOM::HTMLCollection> HTMLSelectElement::selected_options()
{
    // The selectedOptions IDL attribute must return an HTMLCollection rooted at the select node,
    // whose filter matches the elements in the list of options that have their selectedness set to true.
    if (!m_selected_options) {
        m_selected_options = DOM::HTMLCollection::create(*this, DOM::HTMLCollection::Scope::Descendants, [](Element const& element) {
            if (is<HTML::HTMLOptionElement>(element)) {
                auto const& option_element = verify_cast<HTMLOptionElement>(element);
                return option_element.selected();
            }
            return false;
        });
    }
    return *m_selected_options;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#concept-select-option-list
Vector<JS::Handle<HTMLOptionElement>> HTMLSelectElement::list_of_options() const
{
    // The list of options for a select element consists of all the option element children of the select element,
    // and all the option element children of all the optgroup element children of the select element, in tree order.
    Vector<JS::Handle<HTMLOptionElement>> list;

    for_each_child_of_type<HTMLOptionElement>([&](HTMLOptionElement& option_element) {
        list.append(JS::make_handle(option_element));
        return IterationDecision::Continue;
    });

    for_each_child_of_type<HTMLOptGroupElement>([&](HTMLOptGroupElement const& optgroup_element) {
        optgroup_element.for_each_child_of_type<HTMLOptionElement>([&](HTMLOptionElement& option_element) {
            list.append(JS::make_handle(option_element));
            return IterationDecision::Continue;
        });
        return IterationDecision::Continue;
    });

    return list;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-select-element:concept-form-reset-control
void HTMLSelectElement::reset_algorithm()
{
    // The reset algorithm for select elements is to go through all the option elements in the element's list of options,
    for (auto const& option_element : list_of_options()) {
        // set their selectedness to true if the option element has a selected attribute, and false otherwise,
        option_element->m_selected = option_element->has_attribute(AttributeNames::selected);
        // set their dirtiness to false,
        option_element->m_dirty = false;
        // and then have the option elements ask for a reset.
        option_element->ask_for_a_reset();
    }
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-select-selectedindex
WebIDL::Long HTMLSelectElement::selected_index() const
{
    // The selectedIndex IDL attribute, on getting, must return the index of the first option element in the list of options
    // in tree order that has its selectedness set to true, if any. If there isn't one, then it must return −1.

    WebIDL::Long index = 0;
    for (auto const& option_element : list_of_options()) {
        if (option_element->selected())
            return index;
        ++index;
    }
    return -1;
}

void HTMLSelectElement::set_selected_index(WebIDL::Long index)
{
    // On setting, the selectedIndex attribute must set the selectedness of all the option elements in the list of options to false,
    // and then the option element in the list of options whose index is the given new value,
    // if any, must have its selectedness set to true and its dirtiness set to true.
    auto options = list_of_options();
    for (auto& option : options)
        option->m_selected = false;

    if (index < 0 || index >= static_cast<int>(options.size()))
        return;

    auto& selected_option = options[index];
    selected_option->m_selected = true;
    selected_option->m_dirty = true;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLSelectElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
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
    if (has_attribute(AttributeNames::size)) {
        if (auto size_string = get_attribute(HTML::AttributeNames::size); size_string.has_value()) {
            if (auto size = size_string->to_number<int>(); size.has_value() && *size > 1)
                return ARIA::Role::listbox;
        }
    }
    // https://www.w3.org/TR/html-aria/#el-select
    return ARIA::Role::combobox;
}

String HTMLSelectElement::value() const
{
    for (auto const& option_element : list_of_options())
        if (option_element->selected())
            return option_element->value();
    return ""_string;
}

WebIDL::ExceptionOr<void> HTMLSelectElement::set_value(String const& value)
{
    for (auto const& option_element : list_of_options())
        option_element->set_selected(option_element->value() == value);
    update_inner_text_element();
    queue_input_and_change_events();
    return {};
}

void HTMLSelectElement::queue_input_and_change_events()
{
    // When the user agent is to send select update notifications, queue an element task on the user interaction task source given the select element to run these steps:
    queue_an_element_task(HTML::Task::Source::UserInteraction, [this] {
        // FIXME: 1. Set the select element's user interacted to true.

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
    invalidate_style();
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
    auto& global_object = relevant_global_object(*this);
    if (!is<HTML::Window>(global_object))
        return;
    auto& relevant_global_object = static_cast<HTML::Window&>(global_object);
    if (!relevant_global_object.has_transient_activation())
        return;

    // 2. If element is not mutable, then return.
    if (!enabled())
        return;

    // 3. Consume user activation given element's relevant global object.
    relevant_global_object.consume_user_activation();

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
        if (is<HTMLOptGroupElement>(*child)) {
            auto& opt_group_element = verify_cast<HTMLOptGroupElement>(*child);
            Vector<SelectItemOption> option_group_items;
            for (auto const& child : opt_group_element.children_as_vector()) {
                if (is<HTMLOptionElement>(*child)) {
                    auto& option_element = verify_cast<HTMLOptionElement>(*child);
                    option_group_items.append(SelectItemOption { id_counter++, strip_newlines(option_element.text_content()), option_element.value(), option_element.selected(), option_element.disabled(), option_element });
                }
            }
            m_select_items.append(SelectItemOptionGroup { opt_group_element.get_attribute(AttributeNames::label).value_or(String {}), option_group_items });
        }

        if (is<HTMLOptionElement>(*child)) {
            auto& option_element = verify_cast<HTMLOptionElement>(*child);
            m_select_items.append(SelectItemOption { id_counter++, strip_newlines(option_element.text_content()), option_element.value(), option_element.selected(), option_element.disabled(), option_element });
        }

        if (is<HTMLHRElement>(*child))
            m_select_items.append(SelectItemSeparator {});
    }

    // Request select dropdown
    auto weak_element = make_weak_ptr<HTMLSelectElement>();
    auto rect = get_bounding_client_rect();
    auto position = document().navigable()->to_top_level_position(Web::CSSPixelPoint { rect->x(), rect->y() });
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
        return WebIDL::InvalidStateError::create(realm(), "Element is not mutable"_fly_string);

    // 2. If this's relevant settings object's origin is not same origin with this's relevant settings object's top-level origin,
    // and this is a select element, then throw a "SecurityError" DOMException.
    if (!relevant_settings_object(*this).origin().is_same_origin(relevant_settings_object(*this).top_level_origin)) {
        return WebIDL::SecurityError::create(realm(), "Cross origin pickers are not allowed"_fly_string);
    }

    // 3. If this's relevant global object does not have transient activation, then throw a "NotAllowedError" DOMException.
    // FIXME: The global object we get here should probably not need casted to Window to check for transient activation
    auto& global_object = relevant_global_object(*this);
    if (!is<HTML::Window>(global_object) || !static_cast<HTML::Window&>(global_object).has_transient_activation()) {
        return WebIDL::NotAllowedError::create(realm(), "Too long since user activation to show picker"_fly_string);
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

    for (auto const& option_element : list_of_options())
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

    // Wait until children are ready
    queue_an_element_task(HTML::Task::Source::Microtask, [this] {
        // Select first option when no other option is selected
        if (selected_index() == -1) {
            auto options = list_of_options();
            if (options.size() > 0) {
                options.at(0)->set_selected(true);
            }
        }
        update_inner_text_element();
    });
}

void HTMLSelectElement::form_associated_element_was_removed(DOM::Node*)
{
    set_shadow_root(nullptr);
}

void HTMLSelectElement::computed_css_values_changed()
{
    // Hide chevron icon when appearance is none
    if (m_chevron_icon_element) {
        auto appearance = computed_css_values()->appearance();
        if (appearance.has_value() && *appearance == CSS::Appearance::None) {
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

    auto shadow_root = heap().allocate<DOM::ShadowRoot>(realm(), document(), *this, Bindings::ShadowRootMode::Closed);
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
    m_chevron_icon_element = DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    MUST(m_chevron_icon_element->set_attribute(HTML::AttributeNames::style, R"~~~(
        width: 16px;
        height: 16px;
        margin-left: 4px;
    )~~~"_string));
    MUST(m_chevron_icon_element->set_inner_html("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\"><path d=\"M7.41,8.58L12,13.17L16.59,8.58L18,10L12,16L6,10L7.41,8.58Z\" /></svg>"sv));
    MUST(border->append_child(*m_chevron_icon_element));

    update_inner_text_element();
}

void HTMLSelectElement::update_inner_text_element()
{
    if (!m_inner_text_element)
        return;

    // Update inner text element to text content of selected option
    for (auto const& option_element : list_of_options()) {
        if (option_element->selected()) {
            m_inner_text_element->set_text_content(strip_newlines(option_element->text_content()));
            return;
        }
    }
}
}
