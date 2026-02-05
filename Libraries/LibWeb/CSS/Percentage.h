/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

class Percentage {
public:
    explicit Percentage(double value)
        : m_value(value)
    {
    }

    double value() const { return m_value; }
    double as_fraction() const { return m_value * 0.01; }

    void serialize(StringBuilder& builder, SerializationMode = SerializationMode::Normal) const
    {
        // https://drafts.csswg.org/cssom/#serialize-a-css-value
        // -> <percentage>
        // The <number> component serialized as per <number> followed by the literal string "%" (U+0025).
        serialize_a_number(builder, m_value);
        builder.append('%');
    }

    String to_string(SerializationMode mode = SerializationMode::Normal) const
    {
        StringBuilder builder;
        serialize(builder, mode);
        return builder.to_string_without_validation();
    }

    bool operator==(Percentage const& other) const { return m_value == other.m_value; }

    int operator<=>(Percentage const& other) const
    {
        if (m_value < other.m_value)
            return -1;
        if (m_value > other.m_value)
            return 1;
        return 0;
    }

private:
    double m_value;
};

}

template<>
struct AK::Formatter<Web::CSS::Percentage> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Percentage const& percentage)
    {
        return Formatter<StringView>::format(builder, percentage.to_string());
    }
};
