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
    static GC::Ref<CSSCounterStyleRule> create(JS::Realm&, FlyString name, RefPtr<StyleValue const> system, RefPtr<StyleValue const> negative, RefPtr<StyleValue const> prefix, RefPtr<StyleValue const> suffix, RefPtr<StyleValue const> range, RefPtr<StyleValue const> pad, RefPtr<StyleValue const> fallback, RefPtr<StyleValue const> symbols, RefPtr<StyleValue const> additive_symbols, RefPtr<StyleValue const> speak_as);
    virtual ~CSSCounterStyleRule() = default;

    virtual String serialized() const override;

    FlyString name() const { return m_name; }
    void set_name(FlyString name);

    FlyString system() const;
    void set_system(FlyString const& system);
    RefPtr<StyleValue const> const& system_style_value() const { return m_system; }

    FlyString negative() const;
    void set_negative(FlyString const& negative);
    RefPtr<StyleValue const> const& negative_style_value() const { return m_negative; }

    FlyString prefix() const;
    void set_prefix(FlyString const& prefix);
    RefPtr<StyleValue const> const& prefix_style_value() const { return m_prefix; }

    FlyString suffix() const;
    void set_suffix(FlyString const& suffix);
    RefPtr<StyleValue const> const& suffix_style_value() const { return m_suffix; }

    FlyString range() const;
    void set_range(FlyString const& range);
    RefPtr<StyleValue const> const& range_style_value() const { return m_range; }

    FlyString pad() const;
    void set_pad(FlyString const& pad);
    RefPtr<StyleValue const> const& pad_style_value() const { return m_pad; }

    FlyString fallback() const;
    void set_fallback(FlyString const& fallback);
    RefPtr<StyleValue const> const& fallback_style_value() const { return m_fallback; }

    FlyString symbols() const;
    void set_symbols(FlyString const& symbols);
    RefPtr<StyleValue const> const& symbols_style_value() const { return m_symbols; }

    FlyString additive_symbols() const;
    void set_additive_symbols(FlyString const& additive_symbols);
    RefPtr<StyleValue const> const& additive_symbols_style_value() const { return m_additive_symbols; }

    FlyString speak_as() const;
    void set_speak_as(FlyString const& speak_as);
    RefPtr<StyleValue const> const& speak_as_style_value() const { return m_speak_as; }

    // https://drafts.csswg.org/css-counter-styles-3/#non-overridable-counter-style-names
    static bool matches_non_overridable_counter_style_name(FlyString const& name)
    {
        // The non-overridable counter-style names are the keywords decimal, disc, square, circle, disclosure-open, and disclosure-closed.
        return name.is_one_of_ignoring_ascii_case("decimal"sv, "disc"sv, "square"sv, "circle"sv, "disclosure-open"sv, "disclosure-closed"sv);
    }

protected:
    CSSCounterStyleRule(JS::Realm&, FlyString name, RefPtr<StyleValue const> system, RefPtr<StyleValue const> negative, RefPtr<StyleValue const> prefix, RefPtr<StyleValue const> suffix, RefPtr<StyleValue const> range, RefPtr<StyleValue const> pad, RefPtr<StyleValue const> fallback, RefPtr<StyleValue const> symbols, RefPtr<StyleValue const> additive_symbols, RefPtr<StyleValue const> speak_as);

    FlyString m_name;
    RefPtr<StyleValue const> m_system;
    RefPtr<StyleValue const> m_negative;
    RefPtr<StyleValue const> m_prefix;
    RefPtr<StyleValue const> m_suffix;
    RefPtr<StyleValue const> m_range;
    RefPtr<StyleValue const> m_pad;
    RefPtr<StyleValue const> m_fallback;
    RefPtr<StyleValue const> m_symbols;
    RefPtr<StyleValue const> m_additive_symbols;
    RefPtr<StyleValue const> m_speak_as;

    virtual void initialize(JS::Realm&) override;
};

}
