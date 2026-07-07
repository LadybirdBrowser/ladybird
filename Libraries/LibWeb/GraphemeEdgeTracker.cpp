/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibWeb/GraphemeEdgeTracker.h>

namespace Web {

size_t find_line_start(Utf16View const& view, size_t offset)
{
    while (offset != 0 && view.code_unit_at(offset - 1) != '\n')
        --offset;
    return offset;
}

size_t find_line_end(Utf16View const& view, size_t offset)
{
    auto length = view.length_in_code_units();
    while (offset < length && view.code_unit_at(offset) != '\n')
        ++offset;
    return offset;
}

}
