/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2022, Martin Falisse <mfalisse@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GridTrackPlacement.h"
#include <AK/StringBuilder.h>

namespace Web::CSS {

String GridTrackPlacement::to_string() const
{
    StringBuilder builder;
    m_value.visit(
        [&](Auto const&) {
            builder.append("auto"sv);
        },
        [&](AreaOrLine const& area_or_line) {
            if (area_or_line.line_number.has_value() && area_or_line.name.has_value()) {
                builder.appendff("{} {}", *area_or_line.line_number, *area_or_line.name);
            } else if (area_or_line.line_number.has_value()) {
                builder.appendff("{}", *area_or_line.line_number);
            }
            if (area_or_line.name.has_value()) {
                builder.appendff("{}", *area_or_line.name);
            }
        },
        [&](Span const& span) {
            builder.appendff("span {}", span.value);
        });
    return MUST(builder.to_string());
}

}
