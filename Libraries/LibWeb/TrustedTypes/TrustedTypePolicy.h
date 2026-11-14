/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/TrustedTypePolicy.h>
#include <LibWeb/Forward.h>
#include <LibWeb/TrustedTypes/InjectionSink.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

struct ParseHTMLUnsafeOptions;
struct SetHTMLUnsafeOptions;

}

namespace Web::TrustedTypes {

class TrustedHTMLParserOptions;

// https://www.w3.org/TR/trusted-types/#typedefdef-trustedtype
using TrustedType = Variant<
    GC::Ref<TrustedHTML>,
    GC::Ref<TrustedScript>,
    GC::Ref<TrustedScriptURL>>;

enum class TrustedTypeName : u8 {
    TrustedHTML,
    TrustedScript,
    TrustedScriptURL,
};

enum class ThrowIfCallbackMissing {
    Yes,
    No
};

class TrustedTypePolicy final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(TrustedTypePolicy, trusted_type_policy)
};

WebIDL::ExceptionOr<Optional<TrustedType>> process_value_with_a_default_policy(TrustedTypeName, JS::Object&, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String>, InjectionSink);

WebIDL::ExceptionOr<Utf16String> get_trusted_type_compliant_string(TrustedTypeName, JS::Object&, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> input, InjectionSink sink, Utf16View sink_group);

WebIDL::ExceptionOr<Utf16String> get_trusted_types_compliant_attribute_value(Utf16FlyString const& attribute_name, Optional<Utf16FlyString> attribute_ns, DOM::Element const& element, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> const& new_value);
bool trusted_script_value_matches(JS::Value, StringView expected);
bool trusted_script_values_match(ReadonlySpan<JS::Value>, ReadonlySpan<String> expected);

enum class DocumentType : u8 {
    HTML,
    XML,
};

enum class OptionsFromAuthor : u8 {
    No,
    Yes,
};

template<typename Options>
using CompliantParserOptions = Variant<Options, GC::Ref<TrustedHTMLParserOptions>>;

template<typename Options>
struct CompliantInput {
    Utf16String html;
    CompliantParserOptions<Options> options;
};

template<typename Options>
WebIDL::ExceptionOr<CompliantParserOptions<Options>> get_trusted_type_compliant_parser_options(JS::Object& global, CompliantParserOptions<Options> const& input, ThrowIfCallbackMissing, InjectionSink, OptionsFromAuthor = OptionsFromAuthor::Yes);

template<typename Options>
WebIDL::ExceptionOr<CompliantInput<Options>> get_trusted_type_compliant_input(JS::Object& global, TrustedHTMLOrString const& input, CompliantParserOptions<Options> const& options, InjectionSink, DocumentType = DocumentType::HTML, OptionsFromAuthor = OptionsFromAuthor::Yes);

}
