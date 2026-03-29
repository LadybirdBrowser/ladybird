/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2022, Martin Falisse <mfalisse@outlook.com>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class GridTrackPlacement {
public:
    static GridTrackPlacement make_auto()
    {
        return GridTrackPlacement();
    }

    static GridTrackPlacement make_line(RefPtr<StyleValue const> line_number, Optional<String> name)
    {
        return GridTrackPlacement(AreaOrLine { .line_number = move(line_number), .name = move(name) });
    }

    static GridTrackPlacement make_span(NonnullRefPtr<StyleValue const> value, Optional<String> name)
    {
        return GridTrackPlacement(Span { .value = move(value), .name = move(name) });
    }

    bool is_auto() const { return m_value.has<Auto>(); }
    bool is_span() const { return m_value.has<Span>(); }
    bool is_area_or_line() const { return m_value.has<AreaOrLine>(); }

    bool is_auto_positioned() const { return is_auto() || is_span(); }
    bool is_positioned() const { return !is_auto_positioned(); }

    bool is_custom_ident() const { return is_area_or_line() && !m_value.get<AreaOrLine>().line_number; }

    bool has_identifier() const
    {
        return is_area_or_line() && m_value.get<AreaOrLine>().name.has_value();
    }

    bool has_line_number() const
    {
        return is_area_or_line() && m_value.get<AreaOrLine>().line_number;
    }

    String identifier() const { return *m_value.get<AreaOrLine>().name; }

    NonnullRefPtr<StyleValue const> line_number() const { return *m_value.get<AreaOrLine>().line_number; }
    NonnullRefPtr<StyleValue const> span() const { return *m_value.get<Span>().value; }

    void serialize(StringBuilder&, SerializationMode) const;
    String to_string(SerializationMode mode) const;

    GridTrackPlacement absolutized(ComputationContext const&) const;

    bool is_computationally_independent() const
    {
        return m_value.visit([](auto const& value) { return value.is_computationally_independent(); });
    }

    bool operator==(GridTrackPlacement const& other) const = default;

private:
    struct Auto {
        bool operator==(Auto const&) const = default;
        bool is_computationally_independent() const { return true; }
    };

    struct AreaOrLine {
        ValueComparingRefPtr<StyleValue const> line_number;
        Optional<String> name;
        bool operator==(AreaOrLine const& other) const = default;
        bool is_computationally_independent() const { return !line_number || line_number->is_computationally_independent(); }
    };

    struct Span {
        ValueComparingNonnullRefPtr<StyleValue const> value;
        Optional<String> name;
        bool operator==(Span const& other) const = default;
        bool is_computationally_independent() const { return value->is_computationally_independent(); }
    };

    GridTrackPlacement()
        : m_value(Auto {})
    {
    }
    GridTrackPlacement(AreaOrLine value)
        : m_value(move(value))
    {
    }
    GridTrackPlacement(Span value)
        : m_value(move(value))
    {
    }

    Variant<Auto, AreaOrLine, Span> m_value;
};

}
