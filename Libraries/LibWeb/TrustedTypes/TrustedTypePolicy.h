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
    Optional<GC::Ptr<WebIDL::CallbackType>> create_html;
    Optional<GC::Ptr<WebIDL::CallbackType>> create_script;
    Optional<GC::Ptr<WebIDL::CallbackType>> create_script_url;
};

class TrustedTypePolicy final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedTypePolicy, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedTypePolicy);

public:
    [[nodiscard]] static GC::Ref<TrustedTypePolicy> create(JS::Realm&, String const&, TrustedTypePolicyOptions const&);

    virtual ~TrustedTypePolicy() override { }

    String name() const { return m_name; }

private:
    explicit TrustedTypePolicy(JS::Realm&, String const&, TrustedTypePolicyOptions const&);
    virtual void initialize(JS::Realm&) override;

    String const m_name;
    TrustedTypePolicyOptions const m_options;
};

WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> create_a_trusted_type_policy(TrustedTypePolicyFactory*, String const&, TrustedTypePolicyOptions const&, JS::Object&);

String should_trusted_type_policy_be_blocked_by_content_security_policy(JS::Object&, String const&, Vector<String> const&);

}
