/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/GeneratedCSSStyleProperties.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/DOM/ElementReference.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom/#css-declaration-blocks
class CSSStyleDeclaration
    : public Bindings::PlatformObject
    , public Bindings::GeneratedCSSStyleProperties {
    WEB_PLATFORM_OBJECT(CSSStyleDeclaration, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CSSStyleDeclaration);

public:
    virtual ~CSSStyleDeclaration() = default;
    virtual void initialize(JS::Realm&) override;

    virtual size_t length() const = 0;
    virtual String item(size_t index) const = 0;

    virtual Optional<StyleProperty> property(PropertyID) const = 0;
    virtual Optional<StyleProperty const&> custom_property(FlyString const& custom_property_name) const = 0;

    virtual WebIDL::ExceptionOr<void> set_property(PropertyID, StringView css_text, StringView priority = ""sv);
    virtual WebIDL::ExceptionOr<String> remove_property(PropertyID);

    virtual WebIDL::ExceptionOr<void> set_property(StringView property_name, StringView css_text, StringView priority) = 0;
    virtual WebIDL::ExceptionOr<String> remove_property(StringView property_name) = 0;

    String get_property_value(StringView property) const;
    StringView get_property_priority(StringView property) const;

    String css_text() const;
    virtual WebIDL::ExceptionOr<void> set_css_text(StringView) = 0;

    String css_float() const;
    WebIDL::ExceptionOr<void> set_css_float(StringView);

    virtual String serialized() const = 0;

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-computed-flag
    [[nodiscard]] bool is_computed() const { return m_computed; }

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-readonly-flag
    [[nodiscard]] bool is_readonly() const { return m_readonly; }

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-parent-css-rule
    GC::Ptr<CSSRule> parent_rule() const { return m_parent_rule; }
    void set_parent_rule(GC::Ptr<CSSRule> parent) { m_parent_rule = parent; }

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-owner-node
    Optional<DOM::ElementReference> owner_node() const { return m_owner_node; }
    void set_owner_node(Optional<DOM::ElementReference> owner_node) { m_owner_node = move(owner_node); }

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-updating-flag
    [[nodiscard]] bool is_updating() const { return m_updating; }
    void set_is_updating(bool value) { m_updating = value; }

protected:
    enum class Computed : u8 {
        No,
        Yes,
    };
    enum class Readonly : u8 {
        No,
        Yes,
    };
    explicit CSSStyleDeclaration(JS::Realm&, Computed, Readonly);

    virtual void visit_edges(Visitor&) override;

    virtual CSSStyleDeclaration& generated_style_properties_to_css_style_declaration() override { return *this; }

    void update_style_attribute();

private:
    // ^PlatformObject
    virtual Optional<JS::Value> item_value(size_t index) const override;
    Optional<StyleProperty> get_property_internal(PropertyID) const;

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-parent-css-rule
    GC::Ptr<CSSRule> m_parent_rule { nullptr };

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-owner-node
    Optional<DOM::ElementReference> m_owner_node {};

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-computed-flag
    bool m_computed { false };

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-readonly-flag
    bool m_readonly { false };

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-updating-flag
    bool m_updating { false };
};

class PropertyOwningCSSStyleDeclaration : public CSSStyleDeclaration {
    WEB_PLATFORM_OBJECT(PropertyOwningCSSStyleDeclaration, CSSStyleDeclaration);
    GC_DECLARE_ALLOCATOR(PropertyOwningCSSStyleDeclaration);

public:
    [[nodiscard]] static GC::Ref<PropertyOwningCSSStyleDeclaration>
    create(JS::Realm&, Vector<StyleProperty>, HashMap<FlyString, StyleProperty> custom_properties);

    [[nodiscard]] static GC::Ref<PropertyOwningCSSStyleDeclaration>
    create_element_inline_style(JS::Realm&, GC::Ref<DOM::Element>, Vector<StyleProperty>, HashMap<FlyString, StyleProperty> custom_properties);

    virtual ~PropertyOwningCSSStyleDeclaration() override = default;

    virtual size_t length() const override;
    virtual String item(size_t index) const override;

    virtual Optional<StyleProperty> property(PropertyID) const override;
    virtual Optional<StyleProperty const&> custom_property(FlyString const& custom_property_name) const override { return m_custom_properties.get(custom_property_name); }

    virtual WebIDL::ExceptionOr<void> set_property(StringView property_name, StringView css_text, StringView priority) override;
    virtual WebIDL::ExceptionOr<String> remove_property(StringView property_name) override;
    Vector<StyleProperty> const& properties() const { return m_properties; }
    HashMap<FlyString, StyleProperty> const& custom_properties() const { return m_custom_properties; }

    size_t custom_property_count() const { return m_custom_properties.size(); }

    virtual String serialized() const final override;
    virtual WebIDL::ExceptionOr<void> set_css_text(StringView) override;

    void set_declarations_from_text(StringView);

protected:
    PropertyOwningCSSStyleDeclaration(JS::Realm&, GC::Ptr<DOM::Element> owner_node, Vector<StyleProperty>, HashMap<FlyString, StyleProperty>);

    void empty_the_declarations();
    void set_the_declarations(Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties);

private:
    bool set_a_css_declaration(PropertyID, NonnullRefPtr<CSSStyleValue const>, Important);

    virtual void visit_edges(Cell::Visitor&) override;

    Vector<StyleProperty> m_properties;
    HashMap<FlyString, StyleProperty> m_custom_properties;
};

}
