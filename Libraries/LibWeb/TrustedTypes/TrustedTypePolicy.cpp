/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicyFactory.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::TrustedTypes {

GC_DEFINE_ALLOCATOR(TrustedTypePolicy);

GC::Ref<TrustedTypePolicy> TrustedTypePolicy::create(JS::Realm& realm, String const& name, TrustedTypePolicyOptions const& options)
{
    return realm.create<TrustedTypePolicy>(realm, name, options);
}

TrustedTypePolicy::TrustedTypePolicy(JS::Realm& realm, String const& name, TrustedTypePolicyOptions const& options)
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

// https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-createhtml
JS::ThrowCompletionOr<GC::Ref<TrustedHTML>> TrustedTypePolicy::create_html(String const& input, Vector<JS::Value> const& arguments)
{
    return create_a_trusted_type("TrustedHTML"_string, input, arguments);
}

// https://w3c.github.io/trusted-types/dist/spec/#create-a-trusted-type-algorithm
JS::ThrowCompletionOr<GC::Ref<TrustedHTML>> TrustedTypePolicy::create_a_trusted_type(String const& trusted_type_name, String const& value, Vector<JS::Value> const& arguments)
{
    // 1. Let policyValue be the result of executing Get Trusted Type policy value with the same arguments
    // as this algorithm and additionally true as throwIfMissing.
    auto policy_value = get_trusted_type_policy_value(trusted_type_name, value, arguments, true);

    // 2. If the algorithm threw an error, rethrow the error and abort the following steps.
    if (policy_value.is_error()) {
        return policy_value.release_error();
    }

    // 3. Let dataString be the result of stringifying policyValue.
    auto value_or_error = policy_value.value().to_string(vm());
    ASSERT(!value_or_error.is_error());
    auto dataString = value_or_error.release_value();

    // 4. If policyValue is null or undefined, set dataString to the empty string.
    if (policy_value.value().is_null() || policy_value.value().is_undefined()) {
        dataString = ""_string;
    }

    // 5. Return a new instance of an interface with a type name trustedTypeName, with its associated data value set to dataString.
    return realm().create<TrustedHTML>(realm(), dataString);
}

// https://w3c.github.io/trusted-types/dist/spec/#abstract-opdef-get-trusted-type-policy-value
JS::Completion TrustedTypePolicy::get_trusted_type_policy_value(String const& trusted_type_name, String const& value, Vector<JS::Value> const& values, bool throw_if_missing)
{
    // 1. Let functionName be a function name for the given trustedTypeName, based on the following table:
    // 2. Let function be policy’s options[functionName].
    Optional<GC::Ptr<WebIDL::CallbackType>> function;
    if (trusted_type_name == "TrustedHTML"_string) {
        function = m_options.create_html;
    } else if (trusted_type_name == "TrustedScript"_string) {
        function = m_options.create_script;
    } else if (trusted_type_name == "TrustedScriptURL"_string) {
        function = m_options.create_script_url;
    } else {
        VERIFY_NOT_REACHED();
    }

    // 3. If function is null, then:
    if (!function.has_value()) {
        // 1. If throwIfMissing throw a TypeError.
        if (throw_if_missing)
            return vm().throw_completion<JS::TypeError>(JS::ErrorType::TrustedTypesMissingCallback, trusted_type_name);

        // 2. Else return null
        return {};
    }

    // 4. Let args be << value >>.
    Vector<JS::Value> args { JS::PrimitiveString::create(vm(), value) };

    // 5. Append each item in arguments to args.
    for (auto const& item : values) {
        args.append(item);
    }

    // 6. Let policyValue be the result of invoking function with args and "rethrow".
    auto const policy_value = WebIDL::invoke_callback(*function.value(), JS::js_undefined(), WebIDL::ExceptionBehavior::Rethrow, args);

    // 7. Return policyValue.
    return policy_value;
}

// https://w3c.github.io/trusted-types/dist/spec/#create-trusted-type-policy-algorithm
WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> create_a_trusted_type_policy(TrustedTypePolicyFactory* factory, String const& policy_name, TrustedTypePolicyOptions const& options, JS::Object& global)
{
    auto& realm = factory->realm();
    auto& vm = factory->vm();

    // 1. Let allowedByCSP be the result of executing Should Trusted Type policy creation be blocked by Content Security Policy? algorithm with global, policyName and factory’s created policy names value.
    String const allowed_by_csp = should_trusted_type_policy_be_blocked_by_content_security_policy(global, policy_name, factory->created_policy_names());

    // 2. If allowedByCSP is "Blocked", throw a TypeError and abort further steps.
    if (allowed_by_csp == "Blocked") {
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::CSPDissallowsIt, policy_name);
    }

    // 3. If policyName is default and the factory’s default policy value is not null, throw a TypeError and abort further steps.
    if (policy_name == "default" && factory->default_policy()) {
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::PolicyFactoryAlreadyHasDefaultPolicy);
    }

    // 4. Let policy be a new TrustedTypePolicy object.
    // 5. Set policy’s name property value to policyName.
    // 6. Set policy’s options value to «[ "createHTML" -> options["createHTML", "createScript" -> options["createScript", "createScriptURL" -> options["createScriptURL" ]».
    auto const policy = realm.create<TrustedTypePolicy>(realm, policy_name, options);

    // 7. If the policyName is default, set the factory’s default policy value to policy.
    if (policy_name == "default")
        factory->set_default_policy(policy);

    // 8. Append policyName to factory’s created policy names.
    factory->append_policy_name(policy_name);

    // 9. Return policy.
    return policy;
}

String should_trusted_type_policy_be_blocked_by_content_security_policy(JS::Object& global, String const& policy_name, Vector<String> const& created_policy_names)
{
    // 1. Let result be "Allowed".
    auto result = "Allowed"_string;

    // 2. For each policy in global’s CSP list:
    for (auto const policy : ContentSecurityPolicy::PolicyList::from_object(global)->policies()) {
        // 1. Let createViolation be false.
        bool create_violation = false;

        // 2. If policy’s directive set does not contain a directive which name is "trusted-types", skip to the next policy.
        if (!policy->contains_directive_with_name("trusted-types"_fly_string))
            continue;

        // 3. Let directive be the policy’s directive set’s directive which name is "trusted-types"
        auto const directive = policy->get_directive_by_name("trusted-types"_fly_string);

        // 4. If directive’s value only contains a tt-keyword which is a match for a value 'none', set createViolation to true.
        if (directive->value().size() == 1 && directive->value().first() == "\'none\'"_string)
            create_violation = true;

        // 5. If createdPolicyNames contains policyName and directive’s value does not contain a tt-keyword which is a match for a value 'allow-duplicates', set createViolation to true.
        if (!created_policy_names.find(policy_name).is_end() && directive->value().find("\'allow-duplicates\'"_string).is_end())
            create_violation = true;

        // 6. If directive’s value does not contain a tt-policy-name, which value is policyName, and directive’s value does not contain a tt-wildcard, set createViolation to true.
        if (directive->value().find(policy_name).is_end() && directive->value().find("*"_string).is_end())
            create_violation = true;

        // 7. If createViolation is false, skip to the next policy.
        if (!create_violation)
            continue;

        // FIXME
        // 8. Let violation be the result of executing Create a violation object for global, policy, and directive on global, policy and "trusted-types"
        // 9. Set violation’s resource to "trusted-types-policy".
        // 10. Set violation’s sample to the substring of policyName, containing its first 40 characters.
        // 11. Execute Report a violation on violation.

        // 12. If policy’s disposition is "enforce", then set result to "Blocked".
        if (policy->disposition() == ContentSecurityPolicy::Policy::Disposition::Enforce)
            result = "Blocked"_string;
    }

    // 3. Return result.
    return result;
}

}
