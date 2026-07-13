/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/CSSDescriptors.h>
#include <LibWeb/CSS/CSSRule.h>

namespace Web::CSS {

class CSSCounterStyleRule : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSCounterStyleRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSCounterStyleRule);

public:
    static GC::Ref<CSSCounterStyleRule> create(JS::Realm&, Utf16FlyString name, RefPtr<StyleValue const> system, RefPtr<StyleValue const> negative, RefPtr<StyleValue const> prefix, RefPtr<StyleValue const> suffix, RefPtr<StyleValue const> range, RefPtr<StyleValue const> pad, RefPtr<StyleValue const> fallback, RefPtr<StyleValue const> symbols, RefPtr<StyleValue const> additive_symbols, RefPtr<StyleValue const> speak_as);
    virtual ~CSSCounterStyleRule() = default;

    virtual Utf16String serialized() const override;

    Utf16FlyString name() const { return m_name; }
    void set_name(Utf16String const& name);
    void set_name(Utf16FlyString name);

    Utf16String system() const;
    void set_system(Utf16String const& system);
    RefPtr<StyleValue const> const& system_style_value() const { return m_system; }

    Utf16String negative() const;
    void set_negative(Utf16String const& negative);
    RefPtr<StyleValue const> const& negative_style_value() const { return m_negative; }

    Utf16String prefix() const;
    void set_prefix(Utf16String const& prefix);
    RefPtr<StyleValue const> const& prefix_style_value() const { return m_prefix; }

    Utf16String suffix() const;
    void set_suffix(Utf16String const& suffix);
    RefPtr<StyleValue const> const& suffix_style_value() const { return m_suffix; }

    Utf16String range() const;
    void set_range(Utf16String const& range);
    RefPtr<StyleValue const> const& range_style_value() const { return m_range; }

    Utf16String pad() const;
    void set_pad(Utf16String const& pad);
    RefPtr<StyleValue const> const& pad_style_value() const { return m_pad; }

    Utf16String fallback() const;
    void set_fallback(Utf16String const& fallback);
    RefPtr<StyleValue const> const& fallback_style_value() const { return m_fallback; }

    Utf16String symbols() const;
    void set_symbols(Utf16String const& symbols);
    RefPtr<StyleValue const> const& symbols_style_value() const { return m_symbols; }

    Utf16String additive_symbols() const;
    void set_additive_symbols(Utf16String const& additive_symbols);
    RefPtr<StyleValue const> const& additive_symbols_style_value() const { return m_additive_symbols; }

    Utf16String speak_as() const;
    void set_speak_as(Utf16String const& speak_as);
    RefPtr<StyleValue const> const& speak_as_style_value() const { return m_speak_as; }

    // https://drafts.csswg.org/css-counter-styles-3/#non-overridable-counter-style-names
    static bool matches_non_overridable_counter_style_name(Utf16View name)
    {
        // The non-overridable counter-style names are the keywords decimal, disc, square, circle, disclosure-open, and disclosure-closed.
        return name.equals_ignoring_ascii_case("decimal"sv)
            || name.equals_ignoring_ascii_case("disc"sv)
            || name.equals_ignoring_ascii_case("square"sv)
            || name.equals_ignoring_ascii_case("circle"sv)
            || name.equals_ignoring_ascii_case("disclosure-open"sv)
            || name.equals_ignoring_ascii_case("disclosure-closed"sv);
    }

    virtual void clear_caches() override;

protected:
    CSSCounterStyleRule(JS::Realm&, Utf16FlyString name, RefPtr<StyleValue const> system, RefPtr<StyleValue const> negative, RefPtr<StyleValue const> prefix, RefPtr<StyleValue const> suffix, RefPtr<StyleValue const> range, RefPtr<StyleValue const> pad, RefPtr<StyleValue const> fallback, RefPtr<StyleValue const> symbols, RefPtr<StyleValue const> additive_symbols, RefPtr<StyleValue const> speak_as);

    Utf16FlyString m_name;
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
