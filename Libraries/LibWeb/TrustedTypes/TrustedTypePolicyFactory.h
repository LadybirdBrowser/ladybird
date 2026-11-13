/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/TrustedTypePolicyFactory.h>
#include <LibWeb/Forward.h>

namespace Web::TrustedTypes {

class TrustedTypePolicyFactory final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(TrustedTypePolicyFactory, trusted_type_policy_factory)

public:
    [[nodiscard]] static GC::Ref<TrustedTypePolicyFactory> create(JS::Realm&);

    // https://w3c.github.io/trusted-types/dist/spec/#trustedtypepolicyfactory-default-policy
    GC::Ptr<TrustedTypePolicy> default_policy() const;
};

}
