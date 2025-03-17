/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleDeclaration.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom/#cssstyleproperties
class CSSStyleProperties : public CSSStyleDeclaration {
    WEB_PLATFORM_OBJECT(CSSStyleProperties, CSSStyleDeclaration);
    GC_DECLARE_ALLOCATOR(CSSStyleProperties);

public:
    [[nodiscard]] static GC::Ref<CSSStyleProperties> create(JS::Realm&, Vector<StyleProperty>, HashMap<FlyString, StyleProperty> custom_properties);

    [[nodiscard]] static GC::Ref<CSSStyleProperties> create_resolved_style(DOM::ElementReference);
    [[nodiscard]] static GC::Ref<CSSStyleProperties> create_element_inline_style(DOM::ElementReference, Vector<StyleProperty>, HashMap<FlyString, StyleProperty> custom_properties);

    virtual ~CSSStyleProperties() override = default;
    virtual void initialize(JS::Realm&) override;

    virtual size_t length() const override;
    virtual String item(size_t index) const override;

    virtual Optional<StyleProperty> property(PropertyID) const override;
    virtual Optional<StyleProperty const&> custom_property(FlyString const& custom_property_name) const override;

    // Temporary for one commit.
    using Base::remove_property, Base::set_property;

    virtual WebIDL::ExceptionOr<void> set_property(StringView property_name, StringView css_text, StringView priority) override;
    virtual WebIDL::ExceptionOr<String> remove_property(StringView property_name) override;
    Vector<StyleProperty> const& properties() const { return m_properties; }
    HashMap<FlyString, StyleProperty> const& custom_properties() const { return m_custom_properties; }

    size_t custom_property_count() const { return m_custom_properties.size(); }

    virtual String serialized() const final override;
    virtual WebIDL::ExceptionOr<void> set_css_text(StringView) override;

    void set_declarations_from_text(StringView);

private:
    CSSStyleProperties(JS::Realm&, Computed, Readonly, Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties, Optional<DOM::ElementReference>);

    virtual void visit_edges(Cell::Visitor&) override;

    RefPtr<CSSStyleValue const> style_value_for_computed_property(Layout::NodeWithStyle const&, PropertyID) const;

    bool set_a_css_declaration(PropertyID, NonnullRefPtr<CSSStyleValue const>, Important);
    void empty_the_declarations();
    void set_the_declarations(Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties);

    Vector<StyleProperty> m_properties;
    HashMap<FlyString, StyleProperty> m_custom_properties;
};

}
