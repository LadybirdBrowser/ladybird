/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class FitContentStyleValue final : public StyleValue {
public:
    static ValueComparingNonnullRefPtr<FitContentStyleValue const> create()
    {
        return adopt_ref(*new (nothrow) FitContentStyleValue());
    }
    static ValueComparingNonnullRefPtr<FitContentStyleValue const> create(LengthPercentage length_percentage)
    {
        return adopt_ref(*new (nothrow) FitContentStyleValue(move(length_percentage)));
    }
    virtual ~FitContentStyleValue() override = default;

    virtual String to_string(SerializationMode mode) const override
    {
        if (!m_length_percentage.has_value())
            return "fit-content"_string;
        return MUST(String::formatted("fit-content({})", m_length_percentage->to_string(mode)));
    }

    bool equals(StyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        return m_length_percentage == other.as_fit_content().m_length_percentage;
    }

    [[nodiscard]] Optional<LengthPercentage> const& length_percentage() const { return m_length_percentage; }

private:
    FitContentStyleValue(Optional<LengthPercentage> length_percentage = {})
        : StyleValue(Type::FitContent)
        , m_length_percentage(move(length_percentage))
    {
    }

    Optional<LengthPercentage> m_length_percentage;
};

}
