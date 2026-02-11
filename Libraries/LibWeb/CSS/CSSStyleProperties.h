/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/GeneratedCSSStyleProperties.h>
#include <LibWeb/DOM/StyleInvalidationReason.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom/#cssstyleproperties
class WEB_API CSSStyleProperties
    : public CSSStyleDeclaration
    , public Bindings::GeneratedCSSStyleProperties {
    WEB_PLATFORM_OBJECT(CSSStyleProperties, CSSStyleDeclaration);
    GC_DECLARE_ALLOCATOR(CSSStyleProperties);

public:
    [[nodiscard]] static GC::Ref<CSSStyleProperties> create(JS::Realm&, Vector<StyleProperty>, OrderedHashMap<FlyString, StyleProperty> custom_properties);

    [[nodiscard]] static GC::Ref<CSSStyleProperties> create_resolved_style(JS::Realm&, Optional<DOM::AbstractElement>);
    [[nodiscard]] static GC::Ref<CSSStyleProperties> create_element_inline_style(DOM::AbstractElement, Vector<StyleProperty>, OrderedHashMap<FlyString, StyleProperty> custom_properties);

    virtual ~CSSStyleProperties() override = default;
    virtual void initialize(JS::Realm&) override;

    virtual size_t length() const override;
    virtual String item(size_t index) const override;

    Optional<StyleProperty> get_property(PropertyID) const;
    Optional<StyleProperty const&> custom_property(FlyString const& custom_property_name) const;

    WebIDL::ExceptionOr<void> set_property(PropertyID, StringView css_text, StringView priority = ""sv);
    WebIDL::ExceptionOr<String> remove_property(PropertyID);

    virtual WebIDL::ExceptionOr<void> set_property(FlyString const& property_name, StringView css_text, StringView priority) override;
    virtual WebIDL::ExceptionOr<String> remove_property(FlyString const& property_name) override;

    virtual String get_property_value(FlyString const& property_name) const override;
    virtual StringView get_property_priority(FlyString const& property_name) const override;

    Vector<StyleProperty> const& properties() const { return m_properties; }
    OrderedHashMap<FlyString, StyleProperty> const& custom_properties() const { return m_custom_properties; }

    size_t custom_property_count() const { return m_custom_properties.size(); }

    virtual bool has_property(PropertyNameAndID const&) const override;
    bool has_property(PropertyID) const;

    virtual RefPtr<StyleValue const> get_property_style_value(PropertyNameAndID const&) const override;
    RefPtr<StyleValue const> get_property_style_value(PropertyID) const;
    virtual WebIDL::ExceptionOr<void> set_property_style_value(PropertyNameAndID const&, NonnullRefPtr<StyleValue const>) override;

    String css_float() const;
    WebIDL::ExceptionOr<void> set_css_float(StringView);

    virtual String serialized() const final override;
    String serialize_a_css_value(StyleProperty const&) const;
    String serialize_a_css_value(Vector<StyleProperty>) const;
    virtual WebIDL::ExceptionOr<void> set_css_text(StringView) override;

    void set_declarations_from_text(StringView);

    // ^Bindings::GeneratedCSSStyleProperties
    virtual CSSStyleProperties& generated_style_properties_to_css_style_properties() override { return *this; }

private:
    CSSStyleProperties(JS::Realm&, Computed, Readonly, Vector<StyleProperty> properties, OrderedHashMap<FlyString, StyleProperty> custom_properties, Optional<DOM::AbstractElement>);
    static Vector<StyleProperty> convert_declarations_to_specified_order(Vector<StyleProperty>&);

    virtual void visit_edges(Cell::Visitor&) override;

    RefPtr<StyleValue const> style_value_for_computed_property(Layout::NodeWithStyle const&, PropertyID) const;
    Optional<StyleProperty> get_property_internal(PropertyNameAndID const&) const;
    Optional<StyleProperty> get_direct_property(PropertyNameAndID const&) const;
    WebIDL::ExceptionOr<void> set_property_internal(PropertyNameAndID const&, StringView css_text, StringView priority);
    WebIDL::ExceptionOr<String> remove_property_internal(Optional<PropertyNameAndID> const&);

    bool set_a_css_declaration(PropertyID, NonnullRefPtr<StyleValue const>, Important);
    void empty_the_declarations();
    void set_the_declarations(Vector<StyleProperty> properties, OrderedHashMap<FlyString, StyleProperty> custom_properties);

    void invalidate_owners(DOM::StyleInvalidationReason);

    Vector<StyleProperty> m_properties;
    OrderedHashMap<FlyString, StyleProperty> m_custom_properties;
};

}
