/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>

namespace Web::WebVTT {

struct ParsedCue {
    Utf16String identifier;
    double start_time { 0 };
    double end_time { 0 };
    Utf16String text;
};

// Returns null if the file does not start with the WebVTT file signature.
Optional<Vector<ParsedCue>> parse_webvtt_file(ReadonlyBytes);

}
