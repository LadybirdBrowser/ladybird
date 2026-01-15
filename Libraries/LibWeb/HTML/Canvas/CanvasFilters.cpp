/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Canvas/CanvasFilters.h>
#include <LibWeb/HTML/Canvas/DrawingState.h>

namespace Web::HTML {

String CanvasFilters::filter() const
{
    if (!drawing_state().filter_string.has_value()) {
        return String::from_utf8_without_validation("none"sv.bytes());
    }

    return drawing_state().filter_string.value();
}

}
