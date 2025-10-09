/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/TrustedTypePolicyPrototype.h>
#include <LibWeb/TrustedTypes/InjectionSink.h>

namespace Web::TrustedTypes {

// https://www.w3.org/TR/trusted-types/#typedefdef-trustedtype
using TrustedType = Variant<
    GC::Root<TrustedHTML>,
    GC::Root<TrustedScript>,
    GC::Root<TrustedScriptURL>>;

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

struct TrustedTypePolicyOptions {
    GC::Root<WebIDL::CallbackType> create_html;
    GC::Root<WebIDL::CallbackType> create_script;
    GC::Root<WebIDL::CallbackType> create_script_url;
};

class TrustedTypePolicy final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedTypePolicy, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedTypePolicy);

public:
    virtual ~TrustedTypePolicy() override = default;

    Utf16String const& name() const { return m_name; }

    WebIDL::ExceptionOr<GC::Root<TrustedHTML>> create_html(Utf16String const&, GC::RootVector<JS::Value> const&);
    WebIDL::ExceptionOr<GC::Root<TrustedScript>> create_script(Utf16String const&, GC::RootVector<JS::Value> const&);
    WebIDL::ExceptionOr<GC::Root<TrustedScriptURL>> create_script_url(Utf16String const&, GC::RootVector<JS::Value> const&);

    WebIDL::ExceptionOr<JS::Value> get_trusted_type_policy_value(TrustedTypeName, Utf16String const& value, GC::RootVector<JS::Value> const& values, ThrowIfCallbackMissing throw_if_missing);

private:
    explicit TrustedTypePolicy(JS::Realm&, Utf16String const&, TrustedTypePolicyOptions const&);
    virtual void initialize(JS::Realm&) override;

    TrustedTypesVariants create_a_trusted_type(TrustedTypeName, Utf16String const&, GC::RootVector<JS::Value> const& values);

    Utf16String const m_name;
    TrustedTypePolicyOptions const m_options;
};

WebIDL::ExceptionOr<Optional<TrustedType>> process_value_with_a_default_policy(TrustedTypeName, JS::Object&, Variant<GC::Root<TrustedHTML>, GC::Root<TrustedScript>, GC::Root<TrustedScriptURL>, Utf16String>, InjectionSink);

WebIDL::ExceptionOr<Utf16String> get_trusted_type_compliant_string(TrustedTypeName, JS::Object&, Variant<GC::Root<TrustedHTML>, GC::Root<TrustedScript>, GC::Root<TrustedScriptURL>, Utf16String> input, InjectionSink sink, String const& sink_group);

}
