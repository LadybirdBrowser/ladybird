/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Vector.h>

namespace Unicode {

// All offsets and lengths are measured in UTF-16 code units.
struct FullwidthMappingEdit {
    size_t source_start { 0 };
    size_t source_length { 0 };
    size_t destination_start { 0 };
    size_t destination_length { 0 };
};

struct FullwidthMappingResult {
    Utf16String text;
    Vector<FullwidthMappingEdit> edits;
};

FullwidthMappingResult apply_fullwidth_mapping(Utf16String const&);

}
