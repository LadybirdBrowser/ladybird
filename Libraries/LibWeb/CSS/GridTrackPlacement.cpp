/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2022, Martin Falisse <mfalisse@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GridTrackPlacement.h"
#include <AK/StringBuilder.h>

namespace Web::CSS {

String GridTrackPlacement::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    m_value.visit(
        [&](Auto const&) {
            builder.append("auto"sv);
        },
        [&](AreaOrLine const& area_or_line) {
            if (area_or_line.line_number.has_value() && area_or_line.name.has_value()) {
                builder.appendff("{} {}", area_or_line.line_number->to_string(mode), serialize_an_identifier(*area_or_line.name));
            } else if (area_or_line.line_number.has_value()) {
                builder.appendff("{}", area_or_line.line_number->to_string(mode));
            } else if (area_or_line.name.has_value()) {
                builder.appendff("{}", serialize_an_identifier(*area_or_line.name));
            }
        },
        [&](Span const& span) {
            builder.append("span"sv);

            if (!span.name.has_value() || span.value.is_calculated() || span.value.value() != 1)
                builder.appendff(" {}", span.value.to_string(mode));

            if (span.name.has_value())
                builder.appendff(" {}", span.name.value());
        });
    return MUST(builder.to_string());
}

}
