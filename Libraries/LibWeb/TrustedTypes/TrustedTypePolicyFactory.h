/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/TrustedTypes/InjectionSink.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::TrustedTypes {

class TrustedTypePolicyFactory final : public Bindings::Wrappable {
    WEB_WRAPPABLE(TrustedTypePolicyFactory, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(TrustedTypePolicyFactory);

public:
    virtual ~TrustedTypePolicyFactory() override { }

    JS::Realm& relevant_realm() const;

    WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> create_policy(JS::Realm&, Utf16String const&, TrustedTypePolicyOptions const&);

    bool is_html(JS::Value) const;
    bool is_script(JS::Value) const;
    bool is_script_url(JS::Value) const;

    GC::Ref<TrustedHTML const> empty_html();
    GC::Ref<TrustedScript const> empty_script();

    Optional<Utf16String> get_attribute_type(Utf16String const& tag_name, Utf16String& attribute, Optional<Utf16String> element_ns, Optional<Utf16String> attr_ns);
    Optional<Utf16String> get_property_type(Utf16String const& tag_name, Utf16String const& property, Optional<Utf16String> element_ns);

    GC::Ptr<TrustedTypePolicy> default_policy() const
    {
        return m_default_policy;
    }

private:
    explicit TrustedTypePolicyFactory(DOM::EventTarget&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    WebIDL::ExceptionOr<GC::Ref<TrustedTypePolicy>> create_a_trusted_type_policy(JS::Realm&, Utf16String const&, TrustedTypePolicyOptions const&, JS::Object&);
    ContentSecurityPolicy::Directives::Directive::Result should_trusted_type_policy_be_blocked_by_content_security_policy(JS::Realm&, JS::Object&, Utf16String const&, Vector<Utf16String> const&);

    GC::Ref<DOM::EventTarget> m_owner;

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

Optional<TrustedTypeData> get_trusted_type_data_for_attribute(ElementInterface const& element, Utf16String const&, Optional<Utf16String> const&);

}
