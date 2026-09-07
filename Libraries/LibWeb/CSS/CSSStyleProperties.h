/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/GeneratedCSSStyleProperties.h>
#include <LibWeb/CSS/RustDeclarationBlock.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom/#cssstyleproperties
class WEB_API CSSStyleProperties
    : public CSSStyleDeclaration {
    WEB_WRAPPABLE(CSSStyleProperties, CSSStyleDeclaration);
    GC_DECLARE_ALLOCATOR(CSSStyleProperties);

public:
    [[nodiscard]] static GC::Ref<CSSStyleProperties> create(Vector<StyleProperty>, OrderedHashMap<Utf16FlyString, StyleProperty> custom_properties);
    [[nodiscard]] static GC::Ref<CSSStyleProperties> create(RustDeclarationBlock);

    [[nodiscard]] static GC::Ref<CSSStyleProperties> create_resolved_style(Optional<DOM::AbstractElement>);
    [[nodiscard]] static GC::Ref<CSSStyleProperties> create_element_inline_style(DOM::AbstractElement);

    virtual ~CSSStyleProperties() override = default;

    virtual size_t length() const override;
    virtual Utf16String item(size_t index) const override;

    Optional<StyleProperty> get_property(PropertyID) const;

    [[nodiscard]] static Optional<RefPtr<StyleValue const>> resolved_value_read_from_computed_style(DOM::AbstractElement, PropertyID);
    Optional<StyleProperty const&> custom_property(Utf16FlyString const& custom_property_name) const;

    WebIDL::ExceptionOr<void> set_property(PropertyID, Utf16View css_text, Utf16View priority = u""sv);
    WebIDL::ExceptionOr<Utf16String> remove_property(PropertyID);

    virtual WebIDL::ExceptionOr<void> set_property(Utf16FlyString const& property_name, Utf16View css_text, Utf16View priority) override;
    virtual WebIDL::ExceptionOr<Utf16String> remove_property(Utf16FlyString const& property_name) override;

    virtual Utf16String get_property_value(Utf16FlyString const& property_name) const override;
    virtual Utf16String get_property_priority(Utf16FlyString const& property_name) const override;

    Vector<StyleProperty> const& properties() const { return m_declarations.properties(); }
    OrderedHashMap<Utf16FlyString, StyleProperty> const& custom_properties() const { return m_declarations.custom_properties(); }
    RustDeclarationBlock const& declaration_block() const { return m_declarations; }
    u64 identity() const { return m_declarations.identity(); }
    u64 revision() const { return m_declarations.revision(); }

    // Every custom property a var() in this block's values refers to, and whether each reference
    // names its property with a plain identifier. A reference that substitutes its name can read
    // anything, so no list of names stands for it.
    struct CustomPropertyReferences {
        Vector<Utf16FlyString> names;
        bool all_references_visible { true };
    };
    CustomPropertyReferences const& custom_property_references() const;

    virtual bool has_property(PropertyNameAndID const&) const override;
    bool has_property(PropertyID) const;

    virtual RefPtr<StyleValue const> get_property_style_value(PropertyNameAndID const&) const override;
    RefPtr<StyleValue const> get_property_style_value(PropertyID) const;
    virtual WebIDL::ExceptionOr<void> set_property_style_value(PropertyNameAndID const&, NonnullRefPtr<StyleValue const>) override;

    Utf16String css_float() const;
    WebIDL::ExceptionOr<void> set_css_float(Utf16View);

    virtual Utf16String serialized() const final override;
    Utf16String serialize_a_css_value_to_utf16(StyleProperty const&) const;
    Utf16String serialize_a_css_value_to_utf16(Vector<StyleProperty>) const;
    virtual WebIDL::ExceptionOr<void> set_css_text(Utf16View) override;

    void set_declarations_from_text(Utf16View);

private:
    CSSStyleProperties(Computed, Readonly, RustDeclarationBlock, Optional<DOM::AbstractElement>);

    virtual size_t external_memory_size() const override;

    RefPtr<StyleValue const> style_value_for_computed_property(Layout::NodeWithStyle const&, PropertyID) const;
    Optional<Utf16String> serialized_computed_value_from_stored_handle(PropertyID) const;
    Optional<StyleProperty> get_property_internal(PropertyNameAndID const&) const;
    Optional<StyleProperty> get_direct_property(PropertyNameAndID const&) const;
    WebIDL::ExceptionOr<void> set_property_internal(PropertyNameAndID const&, Utf16View css_text, Utf16View priority);
    WebIDL::ExceptionOr<Utf16String> remove_property_internal(Optional<PropertyNameAndID> const&);
    bool set_a_css_declaration(PropertyID, NonnullRefPtr<StyleValue const>, Important);

    void invalidate_owners();

    RustDeclarationBlock m_declarations;
    mutable OwnPtr<CustomPropertyReferences> m_custom_property_references;
    mutable u64 m_custom_property_references_revision { 0 };
};

#undef ENUMERATE_GENERATED_CSS_STYLE_PROPERTIES

}
