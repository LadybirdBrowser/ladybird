/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2022, Martin Falisse <mfalisse@outlook.com>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GridTrackPlacement.h"
#include <AK/StringBuilder.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>

namespace Web::CSS {

void GridTrackPlacement::serialize(StringBuilder& builder, SerializationMode mode) const
{
    m_value.visit(
        [&](Auto const&) {
            builder.append("auto"sv);
        },
        [&](AreaOrLine const& area_or_line) {
            if (area_or_line.line_number.has_value() && area_or_line.name.has_value()) {
                area_or_line.line_number->serialize(builder, mode);
                builder.append(' ');
                builder.append(serialize_an_identifier(*area_or_line.name));
            } else if (area_or_line.line_number.has_value()) {
                area_or_line.line_number->serialize(builder, mode);
            } else if (area_or_line.name.has_value()) {
                builder.append(serialize_an_identifier(*area_or_line.name));
            }
        },
        [&](Span const& span) {
            builder.append("span"sv);

            if (!span.name.has_value() || span.value.is_calculated() || span.value.value() != 1) {
                builder.append(' ');
                span.value.serialize(builder, mode);
            }

            if (span.name.has_value()) {
                builder.append(' ');
                builder.append(span.name.value());
            }
        });
}

String GridTrackPlacement::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    serialize(builder, mode);
    return MUST(builder.to_string());
}

GridTrackPlacement GridTrackPlacement::absolutized(ComputationContext const& context) const
{
    auto absolutize_integer_or_calculated = [&context](IntegerOrCalculated const& integer_or_calculated) {
        if (!integer_or_calculated.is_calculated())
            return integer_or_calculated;
        auto absolutized = integer_or_calculated.calculated()->absolutized(context);
        if (absolutized->is_calculated())
            return IntegerOrCalculated { absolutized->as_calculated() };
        VERIFY(absolutized->is_integer());
        return IntegerOrCalculated { absolutized->as_integer().integer() };
    };

    return m_value.visit(
        [this](Auto const&) {
            return *this;
        },
        [&](AreaOrLine const& area_or_line) -> GridTrackPlacement {
            return AreaOrLine {
                .line_number = area_or_line.line_number.map(absolutize_integer_or_calculated),
                .name = area_or_line.name,
            };
        },
        [&](Span const& span) -> GridTrackPlacement {
            return Span {
                .value = absolutize_integer_or_calculated(span.value),
                .name = span.name,
            };
        });
}

}
