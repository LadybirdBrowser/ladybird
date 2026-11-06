/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Frequency.h>
#include <LibWeb/CSS/StyleValues/DimensionStyleValue.h>

namespace Web::CSS {

class FrequencyStyleValue final : public DimensionStyleValue {
public:
    virtual ~FrequencyStyleValue() override = default;

    Frequency frequency() const { return Frequency(m_value->frequency.value, static_cast<FrequencyUnit>(m_value->frequency.unit)); }
    virtual double raw_value() const override { return m_value->frequency.value; }
    virtual Utf16FlyString unit_name() const override { return frequency().unit_name(); }

private:
    friend class StyleValue;

    explicit FrequencyStyleValue(StyleValueFFI::StyleValueData const* data)
        : DimensionStyleValue(Type::Frequency, data)
    {
    }
};

}
