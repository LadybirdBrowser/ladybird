/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSDescriptors.h>
#include <LibWeb/CSS/CSSRule.h>

namespace Web::CSS {

class CSSCounterStyleRule : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSCounterStyleRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSCounterStyleRule);

public:
    static GC::Ref<CSSCounterStyleRule> create(JS::Realm&, FlyString name);
    virtual ~CSSCounterStyleRule() = default;

    virtual String serialized() const override;

    FlyString name() const { return m_name; }
    void set_name(FlyString name);

    // https://drafts.csswg.org/css-counter-styles-3/#non-overridable-counter-style-names
    static bool matches_non_overridable_counter_style_name(FlyString const& name)
    {
        // The non-overridable counter-style names are the keywords decimal, disc, square, circle, disclosure-open, and disclosure-closed.
        return name.is_one_of_ignoring_ascii_case("decimal"sv, "disc"sv, "square"sv, "circle"sv, "disclosure-open"sv, "disclosure-closed"sv);
    }

protected:
    CSSCounterStyleRule(JS::Realm&, FlyString name);

    FlyString m_name;

    virtual void initialize(JS::Realm&) override;
};

}
