/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>
#include <LibWeb/TrustedTypes/TrustedScript.h>
#include <LibWeb/TrustedTypes/TrustedScriptURL.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedTypePolicy);

TrustedTypePolicy::TrustedTypePolicy(JS::Realm& realm, Utf16String const& name, TrustedTypePolicyOptions const& options)
    : PlatformObject(realm)
    , m_name(name)
    , m_options(options)
{
}

void TrustedTypePolicy::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrustedTypePolicy);
    Base::initialize(realm);
}

Utf16String to_string(TrustedTypeName trusted_type_name)
{
    switch (trusted_type_name) {
    case TrustedTypeName::TrustedHTML:
        return "TrustedHTML"_utf16;
    case TrustedTypeName::TrustedScript:
        return "TrustedScript"_utf16;
    case TrustedTypeName::TrustedScriptURL:
        return "TrustedScriptURL"_utf16;
    default:
        VERIFY_NOT_REACHED();
    }
}
// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-createhtml
WebIDL::ExceptionOr<GC::Root<TrustedHTML>> TrustedTypePolicy::create_html(Utf16String const& input, GC::RootVector<JS::Value> const& arguments)
{
    // 1. Returns the result of executing the Create a Trusted Type algorithm, with the following arguments:
    //    policy
    //      this value
    //    trustedTypeName
    //      "TrustedHTML"
    //    value
    //      input
    //    arguments
    //      arguments
    auto const trusted_type = TRY(create_a_trusted_type(TrustedTypeName::TrustedHTML, input, arguments));
    return trusted_type.get<GC::Root<TrustedHTML>>();
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-createscript
WebIDL::ExceptionOr<GC::Root<TrustedScript>> TrustedTypePolicy::create_script(Utf16String const& input, GC::RootVector<JS::Value> const& arguments)
{
    // 1. Returns the result of executing the Create a Trusted Type algorithm, with the following arguments:
    //    policy
    //      this value
    //    trustedTypeName
    //      "TrustedScript"
    //    value
    //      input
    //    arguments
    //      arguments
    auto const trusted_type = TRY(create_a_trusted_type(TrustedTypeName::TrustedScript, input, arguments));
    return trusted_type.get<GC::Root<TrustedScript>>();
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-createscripturl
WebIDL::ExceptionOr<GC::Root<TrustedScriptURL>> TrustedTypePolicy::create_script_url(Utf16String const& input, GC::RootVector<JS::Value> const& arguments)
{
    // 1. Returns the result of executing the Create a Trusted Type algorithm, with the following arguments:
    //    policy
    //      this value
    //    trustedTypeName
    //      "TrustedScriptURL"
    //    value
    //      input
    //    arguments
    //      arguments
    auto const trusted_type = TRY(create_a_trusted_type(TrustedTypeName::TrustedScriptURL, input, arguments));
    return trusted_type.get<GC::Root<TrustedScriptURL>>();
}

// https://w3c.github.io/trusted-types/dist/spec/#create-a-trusted-type-algorithm
TrustedTypesVariants TrustedTypePolicy::create_a_trusted_type(TrustedTypeName trusted_type_name, Utf16String const& value, GC::RootVector<JS::Value> const& arguments)
{
    auto& vm = this->vm();
    auto& realm = this->realm();

    // 1. Let policyValue be the result of executing Get Trusted Type policy value with the same arguments
    // as this algorithm and additionally true as throwIfMissing.
    // 2. If the algorithm threw an error, rethrow the error and abort the following steps.
    auto const policy_value = TRY(get_trusted_type_policy_value(trusted_type_name, value, arguments, ThrowIfCallbackMissing::Yes));

    // 3. Let dataString be the result of stringifying policyValue.
    Utf16String data_string;
    switch (trusted_type_name) {
    case TrustedTypeName::TrustedHTML:
    case TrustedTypeName::TrustedScript:
        data_string = TRY(WebIDL::to_utf16_string(vm, policy_value));
        break;
    case TrustedTypeName::TrustedScriptURL:
        data_string = TRY(WebIDL::to_utf16_usv_string(vm, policy_value));
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    // 4. If policyValue is null or undefined, set dataString to the empty string.
    if (policy_value.is_nullish())
        data_string = ""_utf16;

    // 5. Return a new instance of an interface with a type name trustedTypeName, with its associated data value set to dataString.
    switch (trusted_type_name) {
    case TrustedTypeName::TrustedHTML:
        return realm.create<TrustedHTML>(realm, move(data_string));
    case TrustedTypeName::TrustedScript:
        return realm.create<TrustedScript>(realm, move(data_string));
    case TrustedTypeName::TrustedScriptURL:
        return realm.create<TrustedScriptURL>(realm, move(data_string));
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://w3c.github.io/trusted-types/dist/spec/#abstract-opdef-get-trusted-type-policy-value
WebIDL::ExceptionOr<JS::Value> TrustedTypePolicy::get_trusted_type_policy_value(TrustedTypeName trusted_type_name, Utf16String const& value, GC::RootVector<JS::Value> const& values, ThrowIfCallbackMissing throw_if_missing)
{
    auto& vm = this->vm();

    // 1. Let functionName be a function name for the given trustedTypeName, based on the following table:
    // 2. Let function be policy’s options[functionName].
    GC::Ptr<WebIDL::CallbackType> function;
    switch (trusted_type_name) {
    case TrustedTypeName::TrustedHTML:
        function = m_options.create_html;
        break;
    case TrustedTypeName::TrustedScript:
        function = m_options.create_script;
        break;
    case TrustedTypeName::TrustedScriptURL:
        function = m_options.create_script_url;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    // 3. If function is null, then:
    if (!function) {
        // 1. If throwIfMissing throw a TypeError.
        if (throw_if_missing == ThrowIfCallbackMissing::Yes)
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Trying to create a trusted type without a callback"_string };

        // 2. Else return null
        return JS::js_null();
    }

    // 4. Let args be << value >>.
    GC::RootVector<JS::Value> args(heap());
    args.append(JS::PrimitiveString::create(vm, value));

    // 5. Append each item in arguments to args.
    args.extend(values);

    // 6. Let policyValue be the result of invoking function with args and "rethrow".
    auto const policy_value = TRY(WebIDL::invoke_callback(*function, {}, WebIDL::ExceptionBehavior::Rethrow, args));

    // 7. Return policyValue.
    return policy_value;
}

}
