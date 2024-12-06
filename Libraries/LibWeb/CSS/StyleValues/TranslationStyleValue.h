/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/PercentageOr.h>

namespace Web::CSS {

class TranslationStyleValue : public StyleValueWithDefaultOperators<TranslationStyleValue> {
public:
    static ValueComparingNonnullRefPtr<TranslationStyleValue> create(LengthPercentage x, LengthPercentage y)
    {
        return adopt_ref(*new (nothrow) TranslationStyleValue(move(x), move(y)));
    }

    virtual ~TranslationStyleValue() override = default;

    LengthPercentage const& x() const { return m_properties.x; }
    LengthPercentage const& y() const { return m_properties.y; }

    virtual String to_string(SerializationMode) const override;

    bool properties_equal(TranslationStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    explicit TranslationStyleValue(
        LengthPercentage x,
        LengthPercentage y)
        : StyleValueWithDefaultOperators(Type::Translation)
        , m_properties {
            .x = move(x),
            .y = move(y),
        }
    {
    }

    struct Properties {
        LengthPercentage x;
        LengthPercentage y;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
