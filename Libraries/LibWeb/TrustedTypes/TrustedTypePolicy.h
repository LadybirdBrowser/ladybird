/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/TrustedTypePolicy.h>
#include <LibWeb/Bindings/Wrappable.h>
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

using TrustedTypePolicyOptions = Bindings::TrustedTypePolicyOptions;

// https://www.w3.org/TR/trusted-types/#typedefdef-trustedtype
using TrustedType = Variant<
    GC::Ref<TrustedHTML>,
    GC::Ref<TrustedScript>,
    GC::Ref<TrustedScriptURL>>;

using TrustedTypesVariants = WebIDL::ExceptionOr<TrustedType>;

enum class TrustedTypeName {
    TrustedHTML,
    TrustedScript,
    TrustedScriptURL,
};

Utf16String to_string(TrustedTypeName);

enum class ThrowIfCallbackMissing {
    Yes,
    No
};

class TrustedTypePolicy final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(TrustedTypePolicy, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(TrustedTypePolicy);

public:
    virtual ~TrustedTypePolicy() override = default;

    Utf16String const& name() const { return m_name; }

    WebIDL::ExceptionOr<GC::Ref<TrustedHTML>> create_html(JS::VM&, Utf16String const&, GC::RootVector<JS::Value> const&);
    WebIDL::ExceptionOr<GC::Ref<TrustedScript>> create_script(JS::VM&, Utf16String const&, GC::RootVector<JS::Value> const&);
    WebIDL::ExceptionOr<GC::Ref<TrustedScriptURL>> create_script_url(JS::VM&, Utf16String const&, GC::RootVector<JS::Value> const&);
    WebIDL::ExceptionOr<GC::Ref<TrustedHTMLParserOptions>> create_parser_options(JS::Object&, Bindings::SetHTMLUnsafeOptions const&, GC::RootVector<JS::Value> const&);

    bool has_create_parser_options() const { return m_create_parser_options != nullptr; }

    template<typename Options>
    WebIDL::ExceptionOr<JS::Value> get_parser_options_policy_value(JS::Object&, Options const& value, GC::RootVector<JS::Value> const& arguments, ThrowIfCallbackMissing);

    WebIDL::ExceptionOr<JS::Value> get_trusted_type_policy_value(JS::VM&, TrustedTypeName, Utf16String const& value, GC::RootVector<JS::Value> const& values, ThrowIfCallbackMissing throw_if_missing);

    virtual void visit_edges(GC::Cell::Visitor&) override;

private:
    explicit TrustedTypePolicy(Utf16String const&, TrustedTypePolicyOptions const&);

    TrustedTypesVariants create_a_trusted_type(JS::VM&, TrustedTypeName, Utf16String const&, GC::RootVector<JS::Value> const& values);

    Utf16String const m_name;
    GC::Ptr<WebIDL::CallbackType> const m_create_html;
    GC::Ptr<WebIDL::CallbackType> const m_create_script;
    GC::Ptr<WebIDL::CallbackType> const m_create_script_url;
    GC::Ptr<WebIDL::CallbackType> const m_create_parser_options;
};

WebIDL::ExceptionOr<Optional<TrustedType>> process_value_with_a_default_policy(TrustedTypeName, JS::Object&, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String>, InjectionSink);

WebIDL::ExceptionOr<Utf16String> get_trusted_type_compliant_string(TrustedTypeName, JS::Object&, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> input, InjectionSink sink, Utf16View sink_group);

WebIDL::ExceptionOr<Utf16String> get_trusted_types_compliant_attribute_value(Utf16FlyString const& attribute_name, Optional<Utf16FlyString> attribute_ns, DOM::Element const& element, Variant<GC::Ref<TrustedHTML>, GC::Ref<TrustedScript>, GC::Ref<TrustedScriptURL>, Utf16String> const& new_value);
TrustedHTML* trusted_html_from_value(JS::Value);
TrustedScript* trusted_script_from_value(JS::Value);
TrustedScriptURL* trusted_script_url_from_value(JS::Value);
bool is_trusted_html_value(JS::Value);
bool is_trusted_script_value(JS::Value);
bool is_trusted_script_url_value(JS::Value);
bool trusted_script_value_matches(JS::Value, StringView expected);
bool trusted_script_values_match(ReadonlySpan<JS::Value>, ReadonlySpan<String> expected);
JS::Completion invoke_trusted_type_policy_callback(WebIDL::CallbackType&, GC::RootVector<JS::Value> const& arguments);

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

// FIXME: Add-hoc definition of an element interface
struct ElementInterface {
    Utf16String element_name;
    Utf16FlyString element_ns;
};

ElementInterface element_interface(Utf16FlyString const& local_name, Utf16FlyString const& element_ns);

}
