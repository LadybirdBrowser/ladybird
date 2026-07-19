/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API KeywordStyleValue : public StyleValueWithDefaultOperators<KeywordStyleValue> {
public:
    static ValueComparingNonnullRefPtr<KeywordStyleValue const> create(Keyword keyword)
    {
        // Keyword values are immutable and the keyword set is small, so every keyword is
        // interned: one instance per keyword for the lifetime of the process. Repeated
        // creations are then allocation-free, and identical keywords are pointer-identical.
        static auto const& instances = *[] {
            auto* instances = new (nothrow) Vector<NonnullRefPtr<KeywordStyleValue const>>();
            instances->ensure_capacity(number_of_keywords);
            for (size_t i = 0; i < number_of_keywords; ++i)
                instances->unchecked_append(adopt_ref(*new (nothrow) KeywordStyleValue(static_cast<Keyword>(i))));
            return instances;
        }();
        return instances[to_underlying(keyword)];
    }
    virtual ~KeywordStyleValue() override = default;

    Keyword keyword() const { return static_cast<Keyword>(m_value->keyword.keyword); }

    static bool is_color(Keyword);
    bool has_color() const;
    Optional<Color> to_color(ColorResolutionContext) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    void serialize(StringBuilder&, SerializationMode) const;
    void serialize(Utf16StringBuilder&, SerializationMode) const;
    Vector<Parser::ComponentValue> tokenize() const;
    GC::Ref<CSSStyleValue> reify(JS::Realm&, Utf16FlyString const& associated_property) const;

    bool properties_equal(KeywordStyleValue const& other) const { return keyword() == other.keyword(); }

private:
    explicit KeywordStyleValue(Keyword keyword)
        : StyleValueWithDefaultOperators(Type::Keyword, StyleValueFFI::rust_style_value_create_keyword(to_underlying(keyword)))
    {
    }
};

}
