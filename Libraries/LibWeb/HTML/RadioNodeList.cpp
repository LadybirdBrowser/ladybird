/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/RadioNodeListPrototype.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/RadioNodeList.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(RadioNodeList);

JS::NonnullGCPtr<RadioNodeList> RadioNodeList::create(JS::Realm& realm, DOM::Node const& root, Scope scope, Function<bool(DOM::Node const&)> filter)
{
    return realm.heap().allocate<RadioNodeList>(realm, realm, root, scope, move(filter));
}

RadioNodeList::RadioNodeList(JS::Realm& realm, DOM::Node const& root, Scope scope, Function<bool(DOM::Node const&)> filter)
    : DOM::LiveNodeList(realm, root, scope, move(filter))
{
}

RadioNodeList::~RadioNodeList() = default;

void RadioNodeList::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(RadioNodeList);
}

static HTMLInputElement const* radio_button(DOM::Node const& node)
{
    if (!is<HTMLInputElement>(node))
        return nullptr;

    auto const& input_element = verify_cast<HTMLInputElement>(node);
    if (input_element.type_state() != HTMLInputElement::TypeAttributeState::RadioButton)
        return nullptr;

    return &input_element;
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-radionodelist-value
FlyString RadioNodeList::value() const
{
    // 1. Let element be the first element in tree order represented by the RadioNodeList object that is an input element whose type
    //    attribute is in the Radio Button state and whose checkedness is true. Otherwise, let it be null.
    auto* element = verify_cast<HTMLInputElement>(first_matching([](DOM::Node const& node) -> bool {
        auto const* button = radio_button(node);
        if (!button)
            return false;

        return button->checked();
    }));

    // 2. If element is null, return the empty string.
    if (!element)
        return String {};

    // 3. If element is an element with no value attribute, return the string "on".
    // 4. Otherwise, return the value of element's value attribute.
    return element->get_attribute(AttributeNames::value).value_or("on"_string);
}

void RadioNodeList::set_value(FlyString const& value)
{
    HTMLInputElement* element = nullptr;

    // 1. If the new value is the string "on": let element be the first element in tree order represented by the RadioNodeList object
    //    that is an input element whose type attribute is in the Radio Button state and whose value content attribute is either absent,
    //    or present and equal to the new value, if any. If no such element exists, then instead let element be null.
    if (value == "on"sv) {
        element = verify_cast<HTMLInputElement>(first_matching([&value](auto const& node) {
            auto const* button = radio_button(node);
            if (!button)
                return false;

            auto const maybe_value = button->get_attribute(AttributeNames::value);
            return !maybe_value.has_value() || maybe_value.value() == value;
        }));
    }
    // 2. Otherwise: let element be the first element in tree order represented by the RadioNodeList object that is an input element whose
    //    type attribute is in the Radio Button state and whose value content attribute is present and equal to the new value, if any. If
    //    no such element exists, then instead let element be null.
    else {
        element = verify_cast<HTMLInputElement>(first_matching([&value](auto const& node) {
            auto const* button = radio_button(node);
            if (!button)
                return false;

            auto const maybe_value = button->get_attribute(AttributeNames::value);
            return maybe_value.has_value() && maybe_value.value() == value;
        }));
    }

    // 3. If element is not null, then set its checkedness to true.
    if (element)
        element->set_checked(true);
}

}
