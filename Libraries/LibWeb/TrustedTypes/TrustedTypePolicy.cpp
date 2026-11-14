/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

#include <LibGC/Ptr.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/Sanitizer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/SanitizerConfiguration.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>
#include <LibWeb/TrustedTypes/TrustedHTMLParserOptions.h>
#include <LibWeb/TrustedTypes/TrustedScript.h>
#include <LibWeb/TrustedTypes/TrustedScriptURL.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::TrustedTypes {

// NB: CompliantParserOptionsResultAbi in trusted_type_policy.rs matches this.
enum class CompliantParserOptionsResult : u8 {
    Input,
    TrustedHTMLParserOptions,
    Thrown,
    TypeError,
};

// NB: ParserOptionsInputAbi in trusted_type_policy.rs matches this.
struct ParserOptionsInput {
    bool run_scripts { false };
    HTML::SanitizerInputKind sanitizer_kind { HTML::SanitizerInputKind::None };
    void const* sanitizer { nullptr };
};

extern "C" {
// Defined in Bindings/Rust/src/trusted_types/trusted_type_policy.rs.
CompliantParserOptionsResult web_rust_trusted_types_get_trusted_type_compliant_parser_options(JSVM*, JSObject* global, ParserOptionsInput const*, bool throw_if_missing, FlatPtr sink, bool options_from_author, TrustedHTMLParserOptions** options, FlatPtr* message, JSValue* exception);
u8 web_rust_trusted_types_get_trusted_type_compliant_string(JSVM*, u8 expected_type, JSObject* global, u8 input_kind, void* input_cell, FlatPtr input_string, FlatPtr sink, FlatPtr sink_group, FlatPtr* string, JSValue* exception);
u8 web_rust_trusted_types_get_trusted_types_compliant_attribute_value(JSVM*, FlatPtr attribute_name, bool has_attribute_ns, FlatPtr attribute_ns, FlatPtr element_local_name, bool has_element_namespace, FlatPtr element_namespace, JSObject* global, u8 input_kind, void* input_cell, FlatPtr input_string, FlatPtr* string, JSValue* exception);
bool web_rust_trusted_types_process_value_with_a_default_policy(JSVM*, u8 trusted_type_name, JSObject* global, u8 input_kind, void* input_cell, FlatPtr input_string, FlatPtr sink, void** result, JSValue* exception);
}

static TrustedScript* trusted_script_from_value(JS::Value value)
{
    if (!value.is_object())
        return nullptr;
    return Bindings::impl_from<TrustedScript>(&value.as_object());
}

bool trusted_script_value_matches(JS::Value value, StringView expected)
{
    auto* trusted_script = trusted_script_from_value(value);
    return trusted_script && trusted_script->to_string() == expected;
}

bool trusted_script_values_match(ReadonlySpan<JS::Value> values, ReadonlySpan<String> expected)
{
    if (values.size() != expected.size())
        return false;

    for (size_t i = 0; i < values.size(); ++i) {
        if (!trusted_script_value_matches(values[i], expected[i]))
            return false;
    }

    return true;
}

template<typename Options>
WebIDL::ExceptionOr<CompliantParserOptions<Options>> get_trusted_type_compliant_parser_options(JS::Object& global, CompliantParserOptions<Options> const& input, ThrowIfCallbackMissing throw_if_missing, InjectionSink sink, OptionsFromAuthor options_from_author)
{
    // NB: Steps 1 to 3, so that pages that don't require Trusted Types never call into Rust.
    if (input.template has<GC::Ref<TrustedHTMLParserOptions>>() || !does_sink_require_trusted_types(global, Script.view(), IncludeReportOnlyPolicies::Yes))
        return input;
    auto const& dictionary = input.template get<Options>();

    TrustedHTMLParserOptions* options = nullptr;
    FlatPtr message = 0;
    JSValue exception {};
    auto result = CompliantParserOptionsResult::Input;
    auto get_compliant_parser_options = [&](ParserOptionsInput const& input_abi) {
        result = web_rust_trusted_types_get_trusted_type_compliant_parser_options(JS::HostABI::vm_to_abi(global.vm()), JS::HostABI::object_to_abi(&global), &input_abi, throw_if_missing == ThrowIfCallbackMissing::Yes, to_string(sink).into_raw(), options_from_author == OptionsFromAuthor::Yes, &options, &message, &exception);
    };
    bool run_scripts = false;
    if constexpr (requires { dictionary.run_scripts; })
        run_scripts = dictionary.run_scripts;
    if (dictionary.sanitizer.has_value()) {
        HTML::lend_sanitizer_input(*dictionary.sanitizer, [&](HTML::SanitizerInputKind kind, void const* sanitizer) {
            get_compliant_parser_options({ .run_scripts = run_scripts, .sanitizer_kind = kind, .sanitizer = sanitizer });
        });
    } else {
        get_compliant_parser_options({ .run_scripts = run_scripts });
    }

    switch (result) {
    case CompliantParserOptionsResult::Input:
        return input;
    case CompliantParserOptionsResult::TrustedHTMLParserOptions:
        return CompliantParserOptions<Options> { GC::Ref { *options } };
    case CompliantParserOptionsResult::Thrown:
        return JS::throw_completion(JS::HostABI::value_from_abi(exception));
    case CompliantParserOptionsResult::TypeError:
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::adopt_raw(message) };
    }
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-compliant-input
template<typename Options>
WebIDL::ExceptionOr<CompliantInput<Options>> get_trusted_type_compliant_input(JS::Object& global, TrustedHTMLOrString const& input, CompliantParserOptions<Options> const& options, InjectionSink sink, DocumentType document_type, OptionsFromAuthor options_from_author)
{
    // 1. Let compliantOptions be options.
    auto compliant_options = options;

    // 2. If documentType is "html":
    if (document_type == DocumentType::HTML) {
        // 1. Set compliantOptions to the result of calling get trusted type compliant parser options, passing global,
        //    options, false, sink, and optionsFromAuthor.
        // NOTE: Sinks that construct options themselves, rather than taking them from the author, pass false for
        //       optionsFromAuthor. For example, createContextualFragment() sets runScripts to true.
        compliant_options = TRY(get_trusted_type_compliant_parser_options(global, options, ThrowIfCallbackMissing::No, sink, options_from_author));

        // 2. If compliantOptions is an instance of TrustedHTMLParserOptions, return (stringified input,
        //    compliantOptions).
        if (compliant_options.template has<GC::Ref<TrustedHTMLParserOptions>>()) {
            auto stringified_input = input.visit(
                [](GC::Ref<TrustedHTML> const& trusted_html) { return trusted_html->to_string(); },
                [](Utf16String const& string) { return string; });
            return CompliantInput<Options> { move(stringified_input), move(compliant_options) };
        }
    }

    // 3. Let compliantHTML be the result of calling get trusted type compliant string, passing TrustedHTML, global,
    //    input, sink, and "script".
    // 4. If the algorithm threw an error, rethrow the error.
    auto compliant_html = TRY(get_trusted_type_compliant_string(TrustedTypeName::TrustedHTML, global, input, sink, Script.view()));

    // 5. Return (compliantHTML, compliantOptions).
    return CompliantInput<Options> { move(compliant_html), move(compliant_options) };
}

template WebIDL::ExceptionOr<CompliantInput<Bindings::SetHTMLUnsafeOptions>> get_trusted_type_compliant_input(JS::Object&, TrustedHTMLOrString const&, CompliantParserOptions<Bindings::SetHTMLUnsafeOptions> const&, InjectionSink, DocumentType, OptionsFromAuthor);
template WebIDL::ExceptionOr<CompliantInput<Bindings::ParseHTMLUnsafeOptions>> get_trusted_type_compliant_input(JS::Object&, TrustedHTMLOrString const&, CompliantParserOptions<Bindings::ParseHTMLUnsafeOptions> const&, InjectionSink, DocumentType, OptionsFromAuthor);

// The kind is 0 for a string, or the TrustedTypeName plus one.
struct InputABI {
    u8 kind { 0 };
    void* cell { nullptr };
    FlatPtr string { 0 };
};

static InputABI input_to_abi(Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> const& input)
{
    return input.visit(
        [](GC::Ref<TrustedHTML> const& value) { return InputABI { 1, value.ptr(), 0 }; },
        [](GC::Ref<TrustedScript> const& value) { return InputABI { 2, value.ptr(), 0 }; },
        [](GC::Ref<TrustedScriptURL> const& value) { return InputABI { 3, value.ptr(), 0 }; },
        [](Utf16String const& value) { return InputABI { 0, nullptr, Utf16String { value }.into_raw() }; });
}

static Utf16String stringified(Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> const& input)
{
    return input.visit(
        [](Utf16String const& value) { return value; },
        [](auto const& value) { return value->to_string(); });
}

static WebIDL::ExceptionOr<Utf16String> result_from_abi(u8 result, FlatPtr string, JSValue exception)
{
    switch (result) {
    case 0:
        return Utf16String::adopt_raw(string);
    case 1:
        return JS::throw_completion(JS::HostABI::value_from_abi(exception));
    case 2:
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::adopt_raw(string) };
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://w3c.github.io/trusted-types/dist/spec/#process-value-with-a-default-policy-algorithm
WebIDL::ExceptionOr<Optional<TrustedType>> process_value_with_a_default_policy(TrustedTypeName trusted_type_name, JS::Object& global, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> input, InjectionSink sink)
{
    auto input_abi = input_to_abi(input);
    void* result = nullptr;
    JSValue exception {};
    if (web_rust_trusted_types_process_value_with_a_default_policy(JS::HostABI::vm_to_abi(global.vm()), to_underlying(trusted_type_name), JS::HostABI::object_to_abi(&global), input_abi.kind, input_abi.cell, input_abi.string, to_string(sink).into_raw(), &result, &exception))
        return JS::throw_completion(JS::HostABI::value_from_abi(exception));
    if (!result)
        return Optional<TrustedType> {};
    switch (trusted_type_name) {
    case TrustedTypeName::TrustedHTML:
        return TrustedType { GC::Ref { *static_cast<TrustedHTML*>(result) } };
    case TrustedTypeName::TrustedScript:
        return TrustedType { GC::Ref { *static_cast<TrustedScript*>(result) } };
    case TrustedTypeName::TrustedScriptURL:
        return TrustedType { GC::Ref { *static_cast<TrustedScriptURL*>(result) } };
    }
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-compliant-string-algorithm
WebIDL::ExceptionOr<Utf16String> get_trusted_type_compliant_string(TrustedTypeName expected_type, JS::Object& global, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> input, InjectionSink sink, Utf16View sink_group)
{
    // NB: Steps 1 to 3, so that pages that don't require Trusted Types never call into Rust.
    auto input_is_expected_type = input.visit(
        [&](GC::Ref<TrustedHTML> const&) { return expected_type == TrustedTypeName::TrustedHTML; },
        [&](GC::Ref<TrustedScript> const&) { return expected_type == TrustedTypeName::TrustedScript; },
        [&](GC::Ref<TrustedScriptURL> const&) { return expected_type == TrustedTypeName::TrustedScriptURL; },
        [](Utf16String const&) { return false; });
    if (input_is_expected_type || !does_sink_require_trusted_types(global, sink_group, IncludeReportOnlyPolicies::Yes))
        return stringified(input);

    auto input_abi = input_to_abi(input);
    FlatPtr string = 0;
    JSValue exception {};
    auto result = web_rust_trusted_types_get_trusted_type_compliant_string(JS::HostABI::vm_to_abi(global.vm()), to_underlying(expected_type), JS::HostABI::object_to_abi(&global), input_abi.kind, input_abi.cell, input_abi.string, to_string(sink).into_raw(), Utf16String::from_utf16(sink_group).into_raw(), &string, &exception);
    return result_from_abi(result, string, exception);
}

// https://w3c.github.io/trusted-types/dist/spec/#validate-attribute-mutation
WebIDL::ExceptionOr<Utf16String> get_trusted_types_compliant_attribute_value(Utf16FlyString const& attribute_name, Optional<Utf16FlyString> attribute_ns, DOM::Element const& element, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> const& new_value)
{
    auto& global = HTML::relevant_global_object(element.document());

    // NB: Unless 'script' requires Trusted Types, every step returns stringified newValue.
    if (!does_sink_require_trusted_types(global, Script.view(), IncludeReportOnlyPolicies::Yes))
        return stringified(new_value);

    auto input_abi = input_to_abi(new_value);
    FlatPtr string = 0;
    JSValue exception {};
    auto result = web_rust_trusted_types_get_trusted_types_compliant_attribute_value(
        JS::HostABI::vm_to_abi(global.vm()),
        attribute_name.to_utf16_string().into_raw(),
        attribute_ns.has_value(),
        attribute_ns.has_value() ? attribute_ns->to_utf16_string().into_raw() : 0,
        element.local_name().to_utf16_string().into_raw(),
        element.namespace_uri().has_value(),
        element.namespace_uri().has_value() ? element.namespace_uri()->to_utf16_string().into_raw() : 0,
        JS::HostABI::object_to_abi(&global),
        input_abi.kind,
        input_abi.cell,
        input_abi.string,
        &string,
        &exception);
    return result_from_abi(result, string, exception);
}

}
