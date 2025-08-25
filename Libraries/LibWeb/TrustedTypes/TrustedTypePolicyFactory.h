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
#include <LibWeb/TrustedTypes/InjectionSink.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::TrustedTypes {

class TrustedTypePolicyFactory final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedTypePolicyFactory, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedTypePolicyFactory);

public:
    virtual ~TrustedTypePolicyFactory() override { }

    WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> create_policy(Utf16String const&, TrustedTypePolicyOptions const&);

    bool is_html(JS::Value);
    bool is_script(JS::Value);
    bool is_script_url(JS::Value);

    GC::Ref<TrustedHTML const> empty_html();
    GC::Ref<TrustedScript const> empty_script();

    Optional<Utf16String> get_attribute_type(Utf16String const& tag_name, Utf16String& attribute, Optional<Utf16String> element_ns, Optional<Utf16String> attr_ns);
    Optional<Utf16String> get_property_type(Utf16String const& tag_name, Utf16String const& property, Optional<Utf16String> element_ns);

    GC::Ptr<TrustedTypePolicy> default_policy() const
    {
        return m_default_policy;
    }

private:
    explicit TrustedTypePolicyFactory(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> create_a_trusted_type_policy(Utf16String const&, TrustedTypePolicyOptions const&, JS::Object&);
    ContentSecurityPolicy::Directives::Directive::Result should_trusted_type_policy_be_blocked_by_content_security_policy(JS::Object&, Utf16String const&, Vector<Utf16String> const&);

    // https://w3c.github.io/trusted-types/dist/spec/#trustedtypepolicyfactory-created-policy-names
    Vector<Utf16String> m_created_policy_names;

    // https://w3c.github.io/trusted-types/dist/spec/#trustedtypepolicyfactory-default-policy
    GC::Ptr<TrustedTypePolicy> m_default_policy;

    // https://www.w3.org/TR/trusted-types/#dom-trustedtypepolicyfactory-emptyhtml
    GC::Ptr<TrustedHTML const> m_empty_html;

    // https://www.w3.org/TR/trusted-types/#dom-trustedtypepolicyfactory-emptyscript
    GC::Ptr<TrustedScript const> m_empty_script;
};

struct TrustedTypeData {
    Utf16String element;
    Optional<Utf16String> attribute_ns;
    FlyString attribute_local_name;
    TrustedTypeName trusted_type;
    InjectionSink sink;
};

Optional<TrustedTypeData> get_trusted_type_data_for_attribute(Utf16String const&, Utf16String const&, Optional<Utf16String> const&);

}
