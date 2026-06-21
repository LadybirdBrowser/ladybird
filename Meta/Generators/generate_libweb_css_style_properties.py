#!/usr/bin/env python3

# Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import json
import sys

from pathlib import Path
from typing import TextIO

sys.path.append(str(Path(__file__).resolve().parent.parent))


def css_property_to_idl_attribute(property_name: str, lowercase_first: bool = False) -> str:
    # https://drafts.csswg.org/cssom/#css-property-to-idl-attribute
    actual_property_name = property_name[1:] if lowercase_first else property_name
    output = []
    uppercase_next = False
    for c in actual_property_name:
        if c == "-":
            uppercase_next = True
        elif uppercase_next:
            uppercase_next = False
            output.append(c.upper())
        else:
            output.append(c)
    return "".join(output)


def write_header_file(out: TextIO, properties: dict) -> None:
    out.write("""
#pragma once

#include <LibWeb/Forward.h>

namespace Web::Bindings {

class GeneratedCSSStyleProperties {
public:
    static void initialize(JS::Realm&, JS::Object&);
}; // class GeneratedCSSStyleProperties

} // namespace Web::Bindings
""")


def write_implementation_file(out: TextIO, properties: dict) -> None:
    out.write("""
#include <AK/Array.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/GeneratedCSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/HTML/Scripting/SimilarOriginWindowAgent.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

namespace {

JS::ThrowCompletionOr<CSS::CSSStyleProperties*> impl_from(JS::VM& vm, JS::Value value)
{
    if (auto impl = value.as_if<CSS::CSSStyleProperties>())
        return impl.ptr();
    return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "CSSStyleProperties");
}

JS::ThrowCompletionOr<CSS::CSSStyleProperties*> impl_from(JS::VM& vm)
{
    auto this_value = vm.this_value();
    if (this_value.is_nullish())
        this_value = &vm.current_realm()->global_object();
    return impl_from(vm, this_value);
}

GC::Ref<JS::NativeFunction> create_getter(JS::Realm& realm, Utf16FlyString const& attribute_name, Utf16FlyString property_name)
{
    auto getter = [property_name = move(property_name)](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        auto* idl_object = TRY(impl_from(vm));

        auto result = TRY(throw_dom_exception_if_needed(vm, [&] {
            return idl_object->get_property_value(property_name);
        }));
        return JS::PrimitiveString::create(vm, Utf16String::from_utf8(result));
    };

    return JS::NativeFunction::create(realm, move(getter), 0, JS::PropertyKey { attribute_name }, &realm, "get"sv);
}

GC::Ref<JS::NativeFunction> create_setter(JS::Realm& realm, Utf16FlyString const& attribute_name, Utf16FlyString property_name)
{
    auto setter = [property_name = move(property_name)](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        auto* idl_object = TRY(impl_from(vm));

        auto value = vm.argument(0);
        String idl_value;
        if (!value.is_null())
            idl_value = TRY(WebIDL::to_string(vm, value));

        auto original_steps = [&]() -> JS::ThrowCompletionOr<JS::Value> {
            TRY(throw_dom_exception_if_needed(vm, [&] {
                return idl_object->set_property(property_name, idl_value, ""sv);
            }));
            return JS::js_undefined();
        };

        // For [CEReactions]: https://html.spec.whatwg.org/multipage/custom-elements.html#cereactions
        auto& reactions_stack = HTML::relevant_similar_origin_window_agent(*idl_object).custom_element_reactions_stack;
        reactions_stack.element_queue_stack.append({});

        auto value_or_exception = original_steps();

        auto queue = reactions_stack.element_queue_stack.take_last();
        Bindings::invoke_custom_element_reactions(queue);

        if (value_or_exception.is_error())
            return value_or_exception.release_error();
        return value_or_exception.release_value();
    };

    return JS::NativeFunction::create(realm, move(setter), 1, JS::PropertyKey { attribute_name }, &realm, "set"sv);
}

}

void GeneratedCSSStyleProperties::initialize(JS::Realm& realm, JS::Object& object)
{
    [[maybe_unused]] u8 default_attributes = JS::Attribute::Enumerable | JS::Attribute::Configurable | JS::Attribute::Writable;

    struct Property {
        StringView attribute_name;
        StringView property_name;
    };

    static constexpr auto properties = Array {
""")

    for name in properties:
        out.write(f'        Property {{ "{css_property_to_idl_attribute(name)}"sv, "{name}"sv }},\n')
        if name.startswith("-webkit-"):
            out.write(
                f'        Property {{ "{css_property_to_idl_attribute(name, lowercase_first=True)}"sv, "{name}"sv }},\n'
            )
        if "-" in name:
            out.write(f'        Property {{ "{name}"sv, "{name}"sv }},\n')

    out.write("""
    };

    for (auto property : properties) {
        auto attribute_name = Utf16FlyString::from_utf8(property.attribute_name);
        auto property_name = Utf16FlyString::from_utf8(property.property_name);
        auto native_getter = create_getter(realm, attribute_name, property_name);
        auto native_setter = create_setter(realm, attribute_name, property_name);
        object.define_direct_accessor(attribute_name, native_getter, native_setter, default_attributes);
    }
}

} // namespace Web::Bindings
""")


def write_idl_file(out: TextIO, properties: dict) -> None:
    out.write("""
interface mixin GeneratedCSSStyleProperties {
""")

    out.write("""
};
""")


def main():
    parser = argparse.ArgumentParser(description="Generate CSS StyleProperties", add_help=False)
    parser.add_argument("--help", action="help", help="Show this help message and exit")
    parser.add_argument("-h", "--header", required=True, help="Path to the CSSStyleProperties header file to generate")
    parser.add_argument(
        "-c", "--implementation", required=True, help="Path to the CSSStyleProperties implementation file to generate"
    )
    parser.add_argument("-i", "--idl", required=True, help="Path to the CSSStyleProperties IDL file to generate")
    parser.add_argument("-j", "--json", required=True, help="Path to the JSON file to read from")
    args = parser.parse_args()

    with open(args.json, "r", encoding="utf-8") as input_file:
        properties = json.load(input_file)

    with open(args.header, "w", encoding="utf-8") as output_file:
        write_header_file(output_file, properties)

    with open(args.implementation, "w", encoding="utf-8") as output_file:
        write_implementation_file(output_file, properties)

    with open(args.idl, "w", encoding="utf-8") as output_file:
        write_idl_file(output_file, properties)


if __name__ == "__main__":
    main()
