/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/Descriptor.h>
#include <LibWeb/CSS/DescriptorID.h>

namespace Web::CSS {

// A non-spec base class for descriptor-list classes
class CSSDescriptors : public CSSStyleDeclaration {
    WEB_PLATFORM_OBJECT(CSSDescriptors, CSSStyleDeclaration);

public:
    virtual ~CSSDescriptors() override;

    virtual size_t length() const override;
    virtual String item(size_t index) const override;
    virtual WebIDL::ExceptionOr<void> set_property(StringView property, StringView value, StringView priority) override;
    virtual WebIDL::ExceptionOr<String> remove_property(StringView property) override;
    virtual String get_property_value(StringView property) const override;
    virtual StringView get_property_priority(StringView property) const override;

    Vector<Descriptor> const& descriptors() const { return m_descriptors; }
    RefPtr<CSSStyleValue const> descriptor(DescriptorID) const;
    RefPtr<CSSStyleValue const> descriptor_or_initial_value(DescriptorID) const;
    virtual String serialized() const override;

    virtual WebIDL::ExceptionOr<void> set_css_text(StringView) override;

protected:
    CSSDescriptors(JS::Realm&, AtRuleID, Vector<Descriptor>);

private:
    bool set_a_css_declaration(DescriptorID, NonnullRefPtr<CSSStyleValue const>, Important);

    virtual void visit_edges(Visitor&) override;

    AtRuleID m_at_rule_id;
    Vector<Descriptor> m_descriptors;
};

}
