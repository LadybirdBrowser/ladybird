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
#include <LibWeb/CSS/RustDescriptorBlock.h>

namespace Web::CSS {

// The descriptors of an at-rule, which an object of an interface implemented in Rust owns
// (Libraries/LibWeb/Bindings/Rust/src/css/css_descriptors.rs).
class CSSDescriptors final : public CSSStyleDeclaration {
    WEB_NON_IDL_WRAPPABLE(CSSDescriptors, CSSStyleDeclaration);
    GC_DECLARE_ALLOCATOR(CSSDescriptors);

public:
    [[nodiscard]] static GC::Ref<CSSDescriptors> create(AtRuleID, RustDescriptorBlock);

    virtual ~CSSDescriptors() override;

    virtual size_t length() const override;
    virtual Utf16String item(size_t index) const override;
    virtual WebIDL::ExceptionOr<void> set_property(Utf16FlyString const& property, Utf16View value, Utf16View priority) override;
    virtual WebIDL::ExceptionOr<Utf16String> remove_property(Utf16FlyString const& property) override;
    virtual Utf16String get_property_value(Utf16FlyString const& property) const override;
    virtual Utf16String get_property_priority(Utf16FlyString const& property) const override;

    RefPtr<StyleValue const> descriptor(DescriptorNameAndID const&) const;
    RefPtr<StyleValue const> descriptor_or_initial_value(DescriptorNameAndID const&) const;
    virtual Utf16String serialized() const override;

    virtual WebIDL::ExceptionOr<void> set_css_text(Utf16View) override;

private:
    CSSDescriptors(AtRuleID, RustDescriptorBlock);

    virtual size_t external_memory_size() const override;
    bool set_a_css_declaration(DescriptorNameAndID const&, NonnullRefPtr<StyleValue const>, Important);
    void invalidate_owners();

    WebIDL::ExceptionOr<void> set_property_internal(Utf16FlyString const& property, Utf16View value, Utf16View priority);

    AtRuleID m_at_rule_id;
    RustDescriptorBlock m_descriptors;
};

bool is_shorthand(AtRuleID, DescriptorNameAndID const&);
void for_each_expanded_longhand(AtRuleID, DescriptorNameAndID const&, RefPtr<StyleValue const>, Function<void(DescriptorNameAndID const&, RefPtr<StyleValue const>)>);

}
