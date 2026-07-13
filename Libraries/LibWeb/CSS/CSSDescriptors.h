/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/Descriptor.h>
#include <LibWeb/CSS/DescriptorID.h>

namespace Web::CSS {

// A non-spec base class for descriptor-list classes
class CSSDescriptors : public CSSStyleDeclaration {
    WEB_NON_IDL_PLATFORM_OBJECT(CSSDescriptors, CSSStyleDeclaration);

public:
    virtual ~CSSDescriptors() override;

    virtual size_t length() const override;
    virtual Utf16String item(size_t index) const override;
    virtual WebIDL::ExceptionOr<void> set_property(Utf16FlyString const& property, Utf16View value, Utf16View priority) override;
    virtual WebIDL::ExceptionOr<Utf16String> remove_property(Utf16FlyString const& property) override;
    virtual Utf16String get_property_value(Utf16FlyString const& property) const override;
    virtual Utf16String get_property_priority(Utf16FlyString const& property) const override;

    Vector<Descriptor> const& descriptors() const { return m_descriptors; }
    RefPtr<StyleValue const> descriptor(DescriptorNameAndID const&) const;
    RefPtr<StyleValue const> descriptor_or_initial_value(DescriptorNameAndID const&) const;
    virtual Utf16String serialized() const override;

    virtual WebIDL::ExceptionOr<void> set_css_text(Utf16View) override;

protected:
    CSSDescriptors(JS::Realm&, AtRuleID, Vector<Descriptor>);

private:
    bool set_a_css_declaration(DescriptorNameAndID const&, NonnullRefPtr<StyleValue const>, Important);

    WebIDL::ExceptionOr<void> set_property_internal(Utf16FlyString const& property, Utf16View value, Utf16View priority);

    AtRuleID m_at_rule_id;
    Vector<Descriptor> m_descriptors;
};

bool is_shorthand(AtRuleID, DescriptorNameAndID const&);
void for_each_expanded_longhand(AtRuleID, DescriptorNameAndID const&, RefPtr<StyleValue const>, Function<void(DescriptorNameAndID const&, RefPtr<StyleValue const>)>);

}
