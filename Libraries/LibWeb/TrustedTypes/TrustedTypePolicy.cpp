/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>
#include <LibWeb/TrustedTypes/TrustedScript.h>
#include <LibWeb/TrustedTypes/TrustedScriptURL.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicyFactory.h>
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

// https://www.w3.org/TR/trusted-types/#process-value-with-a-default-policy-algorithm
WebIDL::ExceptionOr<Optional<TrustedType>> process_value_with_a_default_policy(TrustedTypeName trusted_type_name, JS::Object& global, Variant<GC::Root<TrustedHTML>, GC::Root<TrustedScript>, GC::Root<TrustedScriptURL>, Utf16String> input, InjectionSink sink)
{
    auto& vm = global.vm();
    auto& realm = HTML::relevant_realm(global);

    // 1. Let defaultPolicy be the value of global’s trusted type policy factory’s default policy.
    auto const& default_policy = as<HTML::WindowOrWorkerGlobalScopeMixin>(global).trusted_types()->default_policy();

    // This algorithm routes a value to be assigned to an injection sink through a default policy, should one exist.
    // FIXME: Open an issue upstream. It is not immediately clear what to do if the default policy does not exist.
    // Ref: https://github.com/w3c/trusted-types/issues/595
    if (!default_policy)
        return Optional<TrustedType> {};

    // 2. Let policyValue be the result of executing Get Trusted Type policy value, with the following arguments:
    //    policy:
    //      defaultPolicy
    //    value:
    //      stringified input
    //    trustedTypeName:
    //      expectedType’s type name
    //    arguments:
    //      « trustedTypeName, sink »
    //    throwIfMissing:
    //      false
    //  3. If the algorithm threw an error, rethrow the error and abort the following steps.
    auto arguments = GC::RootVector<JS::Value>(vm.heap());
    arguments.append(JS::PrimitiveString::create(vm, to_string(trusted_type_name)));
    arguments.append(JS::PrimitiveString::create(vm, to_string(sink)));
    auto policy_value = TRY(default_policy->get_trusted_type_policy_value(
        trusted_type_name,
        input.visit(
            [](auto& value) { return value->to_string(); },
            [](Utf16String& value) { return value; }),
        arguments,
        ThrowIfCallbackMissing::No));

    //  4. If policyValue is null or undefined, return policyValue.
    if (policy_value.is_nullish())
        return Optional<TrustedType> {};

    //  5. Let dataString be the result of stringifying policyValue.
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

    //  6. Return a new instance of an interface with a type name trustedTypeName, with its associated data value set to dataString.
    switch (trusted_type_name) {
    case TrustedTypeName::TrustedHTML:
        return realm.create<TrustedHTML>(realm, move(data_string));
    case TrustedTypeName::TrustedScript:
        return realm.create<TrustedScript>(realm, move(data_string));
    case TrustedTypeName::TrustedScriptURL:
        return realm.create<TrustedScriptURL>(realm, move(data_string));
    }
    VERIFY_NOT_REACHED();
}

// https://www.w3.org/TR/trusted-types/#get-trusted-type-compliant-string-algorithm
WebIDL::ExceptionOr<Utf16String> get_trusted_type_compliant_string(TrustedTypeName expected_type, JS::Object& global, Variant<GC::Root<TrustedHTML>, GC::Root<TrustedScript>, GC::Root<TrustedScriptURL>, Utf16String> input, InjectionSink sink, String const& sink_group)
{
    // 1. If input is an instance of expectedType, return stringified input and abort these steps.
    switch (expected_type) {
    case TrustedTypeName::TrustedHTML:
        if (auto* const value = input.get_pointer<GC::Root<TrustedHTML>>(); value)
            return (*value)->to_string();
        break;
    case TrustedTypeName::TrustedScript:
        if (auto* const value = input.get_pointer<GC::Root<TrustedScript>>(); value)
            return (*value)->to_string();
        break;
    case TrustedTypeName::TrustedScriptURL:
        if (auto* const value = input.get_pointer<GC::Root<TrustedScriptURL>>(); value)
            return (*value)->to_string();
        break;
    }

    // 2. Let requireTrustedTypes be the result of executing Does sink type require trusted types? algorithm, passing global, sinkGroup, and true.
    auto const require_trusted_types = does_sink_require_trusted_types(global, sink_group, IncludeReportOnlyPolicies::Yes);

    // 3. If requireTrustedTypes is false, return stringified input and abort these steps.
    if (!require_trusted_types)
        return input.visit(
            [](auto const& value) {
                return value->to_string();
            },
            [](Utf16String const& value) {
                return value;
            });

    // 4. Let convertedInput be the result of executing Process value with a default policy with the same arguments as this algorithm.
    // 5. If the algorithm threw an error, rethrow the error and abort the following steps.
    auto const converted_input = TRY(process_value_with_a_default_policy(expected_type, global, input, sink));

    // 6. If convertedInput is null or undefined, execute the following steps:
    if (!converted_input.has_value()) {
        // 1. Let disposition be the result of executing Should sink type mismatch violation be blocked by Content Security Policy?
        //    algorithm, passing global, stringified input as source, sinkGroup and sink.
        auto const disposition = should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(
            global,
            sink,
            sink_group,
            input.visit(
                [](auto const& value) {
                    return value->to_string();
                },
                [](Utf16String const& value) {
                    return value;
                }));

        // 2. If disposition is “Allowed”, return stringified input and abort further steps.
        if (disposition == ContentSecurityPolicy::Directives::Directive::Result::Allowed) {
            return input.visit(
                [](auto const& value) {
                    return value->to_string();
                },
                [](Utf16String const& value) {
                    return value;
                });
        }

        // 3. Throw a TypeError and abort further steps.
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Sink {} of type {} requires a TrustedType to be used", to_string(sink), sink_group)) };
    }

    // 7. Assert: convertedInput is an instance of expectedType.
    // 8. Return stringified convertedInput.
    VERIFY(converted_input.has_value());
    return converted_input.value().visit([&]<typename Type>(Type const& trusted_type) {
        switch (expected_type) {
        case TrustedTypeName::TrustedHTML:
            VERIFY(IsSame<Type, GC::Root<TrustedHTML>>);
            break;
        case TrustedTypeName::TrustedScript:
            VERIFY(IsSame<Type, GC::Root<TrustedScript>>);
            break;
        case TrustedTypeName::TrustedScriptURL:
            VERIFY(IsSame<Type, GC::Root<TrustedScriptURL>>);
            break;
        }
        return trusted_type->to_string();
    });
}

}
