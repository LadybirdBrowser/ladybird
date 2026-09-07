/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/RustDescriptorBlock.h>

namespace Web::CSS {

class CSSCounterStyleRule : public CSSRule {
    WEB_WRAPPABLE(CSSCounterStyleRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSCounterStyleRule);

public:
    static GC::Ref<CSSCounterStyleRule> create(RustRule);
    virtual ~CSSCounterStyleRule() = default;

    virtual Utf16String serialized() const override;

    Utf16View name() const;
    void set_name(Utf16String const& name);

    Utf16String system() const;
    void set_system(Utf16String const& system);
    RefPtr<StyleValue const> system_style_value() const { return m_descriptors.descriptor(DescriptorNameAndID::from_id(DescriptorID::System)); }

    Utf16String negative() const;
    void set_negative(Utf16String const& negative);

    Utf16String prefix() const;
    void set_prefix(Utf16String const& prefix);

    Utf16String suffix() const;
    void set_suffix(Utf16String const& suffix);

    Utf16String range() const;
    void set_range(Utf16String const& range);

    Utf16String pad() const;
    void set_pad(Utf16String const& pad);

    Utf16String fallback() const;
    void set_fallback(Utf16String const& fallback);

    Utf16String symbols() const;
    void set_symbols(Utf16String const& symbols);

    Utf16String additive_symbols() const;
    void set_additive_symbols(Utf16String const& additive_symbols);

    Utf16String speak_as() const;
    void set_speak_as(Utf16String const& speak_as);
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
    CSSCounterStyleRule(RustRule);

    Parser::ValueParserFFI::FfiCounterStyle const& m_rule;
    RustDescriptorBlock m_descriptors;
};

}
