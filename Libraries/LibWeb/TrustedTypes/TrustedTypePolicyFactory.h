/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/TrustedTypePolicyFactoryPrototype.h>

namespace Web::TrustedTypes {

class TrustedTypePolicyFactory final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(TrustedTypePolicyFactory, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(TrustedTypePolicyFactory);

public:
    [[nodiscard]] static GC::Ref<TrustedTypePolicyFactory> create(JS::Realm&);

    virtual ~TrustedTypePolicyFactory() override { }

    Optional<String> get_attribute_type(String const& tag_name, String& attribute, Optional<String> element_ns, Optional<String> attr_ns);
    Optional<String> get_property_type(String const& tag_name, String const& property, Optional<String> element_ns);

private:
    explicit TrustedTypePolicyFactory(JS::Realm&);
    virtual void initialize(JS::Realm&) override;

    Vector<String> m_created_policy_names;
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
