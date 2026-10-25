/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Utf16String.h>

namespace Web::PaintTiming {

struct DocumentPaintTimingState {
    // https://w3c.github.io/paint-timing/#set-of-previously-reported-paints
    // Each Document has a set of previously reported paints, which is an ordered set of strings, initially empty.
    OrderedHashTable<Utf16String> set_of_previously_reported_paints;
};

}
