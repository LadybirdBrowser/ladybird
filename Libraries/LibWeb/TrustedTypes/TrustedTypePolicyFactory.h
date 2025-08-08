/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/TrustedTypePolicyFactoryPrototype.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::TrustedTypes {

class TrustedTypePolicyFactory final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedTypePolicyFactory, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedTypePolicyFactory);

public:
    [[nodiscard]] static GC::Ref<TrustedTypePolicyFactory> create(JS::Realm&);

    virtual ~TrustedTypePolicyFactory() override { }

    WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> create_policy(String const&, TrustedTypePolicyOptions const&);

    bool is_html(JS::Value);
    bool is_script(JS::Value);
    bool is_script_url(JS::Value);

    Optional<String> get_attribute_type(String const& tag_name, String& attribute, Optional<String> element_ns, Optional<String> attr_ns);
    Optional<String> get_property_type(String const& tag_name, String const& property, Optional<String> element_ns);

    GC::Ptr<TrustedTypePolicy> default_policy() const
    {
        return m_default_policy;
    }

private:
    explicit TrustedTypePolicyFactory(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> create_a_trusted_type_policy(String const&, TrustedTypePolicyOptions const&, JS::Object&);
    ContentSecurityPolicy::Directives::Directive::Result should_trusted_type_policy_be_blocked_by_content_security_policy(JS::Object&, String const&, Vector<String> const&);

    // https://w3c.github.io/trusted-types/dist/spec/#trustedtypepolicyfactory-created-policy-names
    Vector<String> m_created_policy_names;

    // https://w3c.github.io/trusted-types/dist/spec/#trustedtypepolicyfactory-default-policy
    GC::Ptr<TrustedTypePolicy> m_default_policy;
};

struct TrustedTypeData {
    String element;
    Optional<String> attribute_ns;
    String attribute_local_name;
    String trusted_type;
    String sink;
};

Optional<TrustedTypeData> get_trusted_type_data_for_attribute(String const&, String const&, Optional<String> const&);

}
