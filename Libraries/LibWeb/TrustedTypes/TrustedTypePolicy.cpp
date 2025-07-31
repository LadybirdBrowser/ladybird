/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicyFactory.h>
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

// https://w3c.github.io/trusted-types/dist/spec/#create-trusted-type-policy-algorithm
WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> create_a_trusted_type_policy(TrustedTypePolicyFactory* factory, String const& policy_name, TrustedTypePolicyOptions const& options, const JS::Object&)
{
    auto& realm = factory->realm();
    auto& vm = factory->vm();

    // TODO
    // 1. Let allowedByCSP be the result of executing Should Trusted Type policy creation be blocked by Content Security Policy? algorithm with global, policyName and factory’s created policy names value.
    String const allowed_by_csp = "Blocked"_string;

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

}
