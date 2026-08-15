/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Percentage.h>
#include <LibWeb/Forward.h>

namespace Web::SVG {

// A plain <number> | <length> | <percentage> value, the backing store for SVGLength and friends.
// Non-scalar values (math functions, tree counting functions) have no plain representation;
// SVGLength keeps those as their canonical serialization and reparses them on demand.
class SVGLengthValue {
public:
    enum class Kind : u8 {
        Number,
        Length,
        Percentage,
    };

    [[nodiscard]] static SVGLengthValue number(double value) { return { Kind::Number, value, CSS::LengthUnit::Px }; }
    [[nodiscard]] static SVGLengthValue length(double value, CSS::LengthUnit unit) { return { Kind::Length, value, unit }; }
    [[nodiscard]] static SVGLengthValue percentage(double value) { return { Kind::Percentage, value, CSS::LengthUnit::Px }; }

    // Maps a literal number, length or percentage style value; anything else comes back empty.
    [[nodiscard]] static Optional<SVGLengthValue> from_style_value(CSS::StyleValue const&);

    [[nodiscard]] Kind kind() const { return m_kind; }
    [[nodiscard]] double value() const { return m_value; }

    [[nodiscard]] CSS::LengthUnit unit() const
    {
        VERIFY(m_kind == Kind::Length);
        return m_unit;
    }

    [[nodiscard]] CSS::Length to_length() const
    {
        VERIFY(m_kind == Kind::Length);
        return CSS::Length { m_value, m_unit };
    }

    [[nodiscard]] CSS::Percentage to_percentage() const
    {
        VERIFY(m_kind == Kind::Percentage);
        return CSS::Percentage { m_value };
    }

    // Serializes exactly as the equivalent number, length or percentage style value would.
    [[nodiscard]] Utf16String to_utf16_string() const;

    bool operator==(SVGLengthValue const&) const = default;

private:
    SVGLengthValue(Kind kind, double value, CSS::LengthUnit unit)
        : m_kind(kind)
        , m_unit(unit)
        , m_value(value)
    {
    }

    Kind m_kind;
    CSS::LengthUnit m_unit;
    double m_value;
};

}
