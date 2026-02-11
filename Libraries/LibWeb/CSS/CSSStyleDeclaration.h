/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom/#css-declaration-blocks
class CSSStyleDeclaration
    : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CSSStyleDeclaration, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CSSStyleDeclaration);

public:
    virtual ~CSSStyleDeclaration() = default;
    virtual void initialize(JS::Realm&) override;

    virtual size_t length() const = 0;
    virtual String item(size_t index) const = 0;

    virtual WebIDL::ExceptionOr<void> set_property(FlyString const& property_name, StringView css_text, StringView priority) = 0;
    virtual WebIDL::ExceptionOr<String> remove_property(FlyString const& property_name) = 0;

    virtual String get_property_value(FlyString const& property_name) const = 0;
    virtual StringView get_property_priority(FlyString const& property_name) const = 0;

    String css_text() const;
    virtual WebIDL::ExceptionOr<void> set_css_text(StringView) = 0;

    virtual String serialized() const = 0;

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-computed-flag
    [[nodiscard]] bool is_computed() const { return m_computed; }

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-readonly-flag
    [[nodiscard]] bool is_readonly() const { return m_readonly; }

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-parent-css-rule
    GC::Ptr<CSSRule> parent_rule() const { return m_parent_rule; }
    void set_parent_rule(GC::Ptr<CSSRule> parent) { m_parent_rule = parent; }

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-owner-node
    Optional<DOM::AbstractElement> owner_node() const { return m_owner_node; }
    void set_owner_node(Optional<DOM::AbstractElement> owner_node) { m_owner_node = move(owner_node); }

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-updating-flag
    [[nodiscard]] bool is_updating() const { return m_updating; }
    void set_is_updating(bool value) { m_updating = value; }

    virtual bool has_property(PropertyNameAndID const&) const { VERIFY_NOT_REACHED(); }
    virtual RefPtr<StyleValue const> get_property_style_value(PropertyNameAndID const&) const { VERIFY_NOT_REACHED(); }
    virtual WebIDL::ExceptionOr<void> set_property_style_value(PropertyNameAndID const&, NonnullRefPtr<StyleValue const>) { VERIFY_NOT_REACHED(); }

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

    void update_style_attribute();

private:
    // ^PlatformObject
    virtual Optional<JS::Value> item_value(size_t index) const override;

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-parent-css-rule
    GC::Ptr<CSSRule> m_parent_rule { nullptr };

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-owner-node
    Optional<DOM::AbstractElement> m_owner_node {};

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-computed-flag
    bool m_computed { false };

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-readonly-flag
    bool m_readonly { false };

    // https://drafts.csswg.org/cssom/#cssstyledeclaration-updating-flag
    bool m_updating { false };
};

}
