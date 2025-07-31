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

    String const& name() const { return m_name; }

private:
    explicit TrustedTypePolicy(JS::Realm&, String const&, TrustedTypePolicyOptions const&);
    virtual void initialize(JS::Realm&) override;

    String const m_name;
    TrustedTypePolicyOptions const m_options;
};

}
