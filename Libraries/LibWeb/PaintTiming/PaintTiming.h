/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PaintTiming/PaintTimingMixin.h>

namespace Web::PaintTiming {

// https://w3c.github.io/paint-timing/#mark-paint-timing
void mark_paint_timing(DOM::Document&);

// https://w3c.github.io/paint-timing/#report-paint-timing
void report_paint_timing(DOM::Document&, Utf16String const& paint_type, PaintTimingInfo const&);

// https://w3c.github.io/paint-timing/#exposed-for-paint-timing
bool is_exposed_for_paint_timing(DOM::Element const&, GC::Ptr<DOM::Document const>);

}
