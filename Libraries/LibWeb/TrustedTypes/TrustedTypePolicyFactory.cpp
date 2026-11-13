/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicyFactory.h>

namespace Web::TrustedTypes {

extern "C" {
// Defined in Bindings/Rust/src/trusted_types/trusted_type_policy_factory.rs.
TrustedTypePolicyFactory* web_rust_trusted_types_create_trusted_type_policy_factory(JSVM*, JS::Realm*);
TrustedTypePolicy* web_rust_trusted_types_trusted_type_policy_factory_default_policy(TrustedTypePolicyFactory*);
}

GC::Ref<TrustedTypePolicyFactory> TrustedTypePolicyFactory::create(JS::Realm& realm)
{
    return *web_rust_trusted_types_create_trusted_type_policy_factory(JS::HostABI::vm_to_abi(realm.vm()), &realm);
}

GC::Ptr<TrustedTypePolicy> TrustedTypePolicyFactory::default_policy() const
{
    return web_rust_trusted_types_trusted_type_policy_factory_default_policy(const_cast<TrustedTypePolicyFactory*>(this));
}

}
