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

class TrustedTypePolicy final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedTypePolicy, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedTypePolicy);

public:
    [[nodiscard]] static GC::Ref<TrustedTypePolicy> create(JS::Realm&);

    virtual ~TrustedTypePolicy() override { }

private:
    explicit TrustedTypePolicy(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
};

struct TrustedTypePolicyOptions {
    Optional<GC::Ptr<WebIDL::CallbackType>> create_html;
    Optional<GC::Ptr<WebIDL::CallbackType>> create_script;
    Optional<GC::Ptr<WebIDL::CallbackType>> create_script_url;
};

}
