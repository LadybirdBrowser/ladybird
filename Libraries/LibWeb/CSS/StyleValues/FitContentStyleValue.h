/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/PercentageOr.h>

namespace Web::CSS {

class FitContentStyleValue final : public CSSStyleValue {
public:
    static ValueComparingNonnullRefPtr<FitContentStyleValue> create()
    {
        return adopt_ref(*new (nothrow) FitContentStyleValue(LengthPercentage { Length::make_auto() }));
    }
    static ValueComparingNonnullRefPtr<FitContentStyleValue> create(LengthPercentage length_percentage)
    {
        return adopt_ref(*new (nothrow) FitContentStyleValue(move(length_percentage)));
    }
    virtual ~FitContentStyleValue() override = default;

    virtual String to_string(SerializationMode) const override
    {
        if (m_length_percentage.is_auto())
            return "fit-content"_string;
        return MUST(String::formatted("fit-content({})", m_length_percentage.to_string()));
    }

    bool equals(CSSStyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        return m_length_percentage == other.as_fit_content().m_length_percentage;
    }

    [[nodiscard]] LengthPercentage const& length_percentage() const { return m_length_percentage; }

private:
    FitContentStyleValue(LengthPercentage length_percentage)
        : CSSStyleValue(Type::FitContent)
        , m_length_percentage(move(length_percentage))
    {
    }

    LengthPercentage m_length_percentage;
};

}
