/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Vector.h>

namespace Web::Layout {

struct RenderedTextOffset {
    size_t dom_start_offset { 0 };
    size_t dom_end_offset { 0 };
};

struct RenderedTextForDOM {
    Utf16String text;
    Vector<RenderedTextOffset> offset_mapping;
};

}
