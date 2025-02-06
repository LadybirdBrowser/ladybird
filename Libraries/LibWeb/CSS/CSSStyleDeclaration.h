/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
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

namespace Web::CSS {

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

    virtual GC::Ptr<CSSRule> parent_rule() const;

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-computed-flag
    [[nodiscard]] virtual bool computed_flag() const { return false; }

protected:
    explicit CSSStyleDeclaration(JS::Realm&);

    virtual CSSStyleDeclaration& generated_style_properties_to_css_style_declaration() override { return *this; }

private:
    // ^PlatformObject
    virtual Optional<JS::Value> item_value(size_t index) const override;
    Optional<StyleProperty> get_property_internal(PropertyID) const;
};

class PropertyOwningCSSStyleDeclaration : public CSSStyleDeclaration {
    WEB_PLATFORM_OBJECT(PropertyOwningCSSStyleDeclaration, CSSStyleDeclaration);
    GC_DECLARE_ALLOCATOR(PropertyOwningCSSStyleDeclaration);

    friend class ElementInlineCSSStyleDeclaration;

public:
    [[nodiscard]] static GC::Ref<PropertyOwningCSSStyleDeclaration>
    create(JS::Realm&, Vector<StyleProperty>, HashMap<FlyString, StyleProperty> custom_properties);

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

    virtual GC::Ptr<CSSRule> parent_rule() const override;
    void set_parent_rule(GC::Ref<CSSRule>);

protected:
    PropertyOwningCSSStyleDeclaration(JS::Realm&, Vector<StyleProperty>, HashMap<FlyString, StyleProperty>);

    virtual void update_style_attribute() { }

    void empty_the_declarations();
    void set_the_declarations(Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties);

private:
    bool set_a_css_declaration(PropertyID, NonnullRefPtr<CSSStyleValue const>, Important);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<CSSRule> m_parent_rule;
    Vector<StyleProperty> m_properties;
    HashMap<FlyString, StyleProperty> m_custom_properties;
};

class ElementInlineCSSStyleDeclaration final : public PropertyOwningCSSStyleDeclaration {
    WEB_PLATFORM_OBJECT(ElementInlineCSSStyleDeclaration, PropertyOwningCSSStyleDeclaration);
    GC_DECLARE_ALLOCATOR(ElementInlineCSSStyleDeclaration);

public:
    [[nodiscard]] static GC::Ref<ElementInlineCSSStyleDeclaration> create(DOM::Element&, Vector<StyleProperty>, HashMap<FlyString, StyleProperty> custom_properties);

    virtual ~ElementInlineCSSStyleDeclaration() override = default;

    DOM::Element* element() { return m_element.ptr(); }
    const DOM::Element* element() const { return m_element.ptr(); }

    bool is_updating() const { return m_updating; }

    void set_declarations_from_text(StringView);

    virtual WebIDL::ExceptionOr<void> set_css_text(StringView) override;

private:
    ElementInlineCSSStyleDeclaration(DOM::Element&, Vector<StyleProperty> properties, HashMap<FlyString, StyleProperty> custom_properties);

    virtual void visit_edges(Cell::Visitor&) override;

    virtual void update_style_attribute() override;

    GC::Ptr<DOM::Element> m_element;

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-updating-flag
    bool m_updating { false };
};

}
