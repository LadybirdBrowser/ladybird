/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class FitContentStyleValue final : public StyleValue {
public:
    static ValueComparingNonnullRefPtr<FitContentStyleValue const> create();
    static ValueComparingNonnullRefPtr<FitContentStyleValue const> create(NonnullRefPtr<StyleValue const> length_percentage);
    virtual ~FitContentStyleValue() override = default;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const& computation_context) const override;
    virtual void serialize(StringBuilder& builder, SerializationMode mode) const override;

    bool equals(StyleValue const& other) const override;

    virtual bool is_computationally_independent() const override { return !m_length_percentage || m_length_percentage->is_computationally_independent(); }

    [[nodiscard]] Optional<LengthPercentage> length_percentage() const;
    RefPtr<StyleValue const> length_percentage_style_value() const { return m_length_percentage; }

private:
    FitContentStyleValue(ValueComparingRefPtr<StyleValue const> length_percentage = {})
        : StyleValue(Type::FitContent)
        , m_length_percentage(move(length_percentage))
    {
    }

    ValueComparingRefPtr<StyleValue const> m_length_percentage;
};

}
