/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/TrustedTypePolicyPrototype.h>

namespace Web::TrustedTypes {

using TrustedTypesVariants = WebIDL::ExceptionOr<Variant<
    GC::Root<TrustedHTML>,
    GC::Root<TrustedScript>,
    GC::Root<TrustedScriptURL>>>;

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

private:
    explicit TrustedTypePolicy(JS::Realm&, Utf16String const&, TrustedTypePolicyOptions const&);
    virtual void initialize(JS::Realm&) override;

    TrustedTypesVariants create_a_trusted_type(TrustedTypeName, Utf16String const&, GC::RootVector<JS::Value> const& values);

    WebIDL::ExceptionOr<JS::Value> get_trusted_type_policy_value(TrustedTypeName, Utf16String const& value, GC::RootVector<JS::Value> const& values, ThrowIfCallbackMissing throw_if_missing);

    Utf16String const m_name;
    TrustedTypePolicyOptions const m_options;
};

}
