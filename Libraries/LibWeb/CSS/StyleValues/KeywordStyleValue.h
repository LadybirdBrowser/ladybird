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

namespace Web::CSS {

class KeywordStyleValue : public StyleValueWithDefaultOperators<KeywordStyleValue> {
public:
    static ValueComparingNonnullRefPtr<KeywordStyleValue const> create(Keyword keyword)
    {
        switch (keyword) {
        case Keyword::Inherit: {
            static ValueComparingNonnullRefPtr<KeywordStyleValue const> const inherit_instance = adopt_ref(*new (nothrow) KeywordStyleValue(Keyword::Inherit));
            return inherit_instance;
        }
        case Keyword::Initial: {
            static ValueComparingNonnullRefPtr<KeywordStyleValue const> const initial_instance = adopt_ref(*new (nothrow) KeywordStyleValue(Keyword::Initial));
            return initial_instance;
        }
        case Keyword::Revert: {
            static ValueComparingNonnullRefPtr<KeywordStyleValue const> const revert_instance = adopt_ref(*new (nothrow) KeywordStyleValue(Keyword::Revert));
            return revert_instance;
        }
        case Keyword::RevertLayer: {
            static ValueComparingNonnullRefPtr<KeywordStyleValue const> const revert_layer_instance = adopt_ref(*new (nothrow) KeywordStyleValue(Keyword::RevertLayer));
            return revert_layer_instance;
        }
        case Keyword::Unset: {
            static ValueComparingNonnullRefPtr<KeywordStyleValue const> const unset_instance = adopt_ref(*new (nothrow) KeywordStyleValue(Keyword::Unset));
            return unset_instance;
        }
        default:
            return adopt_ref(*new (nothrow) KeywordStyleValue(keyword));
        }
    }
    virtual ~KeywordStyleValue() override = default;

    Keyword keyword() const { return m_keyword; }

    static bool is_color(Keyword);
    virtual bool has_color() const override;
    virtual Optional<Color> to_color(ColorResolutionContext) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual Vector<Parser::ComponentValue> tokenize() const override;
    virtual GC::Ref<CSSStyleValue> reify(JS::Realm&, FlyString const& associated_property) const override;

    bool properties_equal(KeywordStyleValue const& other) const { return m_keyword == other.m_keyword; }

private:
    explicit KeywordStyleValue(Keyword keyword)
        : StyleValueWithDefaultOperators(Type::Keyword)
        , m_keyword(keyword)
    {
    }

    Keyword m_keyword { Keyword::Invalid };
};

inline Keyword StyleValue::to_keyword() const
{
    if (is_keyword())
        return static_cast<KeywordStyleValue const&>(*this).keyword();
    return Keyword::Invalid;
}

}
