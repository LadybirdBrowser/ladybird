/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/HighlightRegistry.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/DOM/Document.h>

// What the Rust implementation of the CSS namespace (Libraries/LibWeb/Bindings/Rust/src/css/css.rs) asks of the
// document it works on.

namespace Web::CSS {

using StyleValueData = StyleValueFFI::StyleValueData;

extern "C" {

// Declared in Bindings/Rust/src/css/css.rs.
void* web_css_namespace_highlight_registry(void* document);
bool web_css_namespace_is_registered(void* document, FlatPtr name);
StyleValueData const* web_css_namespace_parse_initial_value_descriptor(void* document, FlatPtr value);
StyleValueData const* web_css_namespace_parse_with_a_syntax(void* document, FlatPtr value, void const* syntax);
void web_css_namespace_register(void* document, FlatPtr name, void const* syntax, bool inherits, StyleValueData const* initial_value);
}

static DOM::Document& document_from_abi(void* document)
{
    return *static_cast<DOM::Document*>(static_cast<GC::Cell*>(document));
}

static StyleValueData const* into_rust(RefPtr<StyleValue const> const& value)
{
    return value ? StyleValueFFI::rust_style_value_retain(value->rust_style_value_data()) : nullptr;
}

void* web_css_namespace_highlight_registry(void* document)
{
    return document_from_abi(document).highlight_registry().ptr();
}

bool web_css_namespace_is_registered(void* document, FlatPtr name)
{
    return document_from_abi(document).registered_property_set().contains(Utf16FlyString { Utf16String::adopt_raw(name) });
}

StyleValueData const* web_css_namespace_parse_initial_value_descriptor(void* document, FlatPtr value)
{
    Parser::ParsingParams parsing_params { document_from_abi(document) };
    return into_rust(parse_css_descriptor(parsing_params, AtRuleID::Property, DescriptorNameAndID::from_id(DescriptorID::InitialValue), Utf16String::adopt_raw(value)));
}

StyleValueData const* web_css_namespace_parse_with_a_syntax(void* document, FlatPtr value, void const* syntax)
{
    Parser::ParsingParams parsing_params { document_from_abi(document) };
    Parser::RustSyntaxHandle handle { Parser::ValueParserFFI::rust_syntax_retain(syntax) };
    return into_rust(Parser::parse_with_a_syntax(parsing_params, Utf16String::adopt_raw(value), handle));
}

void web_css_namespace_register(void* raw_document, FlatPtr name, void const* syntax, bool inherits, StyleValueData const* initial_value)
{
    auto& document = document_from_abi(raw_document);
    auto property_name = Utf16FlyString { Utf16String::adopt_raw(name) };
    CustomPropertyRegistration registered_property {
        .property_name = property_name,
        .syntax = Parser::RustSyntaxHandle { syntax },
        .inherit = inherits,
        .initial_value = initial_value ? RefPtr<StyleValue const> { StyleValue::adopt_rust_style_value_data(initial_value) } : nullptr,
    };
    document.registered_property_set().set(registered_property.property_name, move(registered_property));
    document.did_change_custom_property_registrations(property_name);
}

}
