/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/PaintTiming/PerformancePaintTiming.h>

namespace Web::PaintTiming {

extern "C" {
// Defined in Bindings/Rust/src/paint_timing/performance_paint_timing.rs.
PerformancePaintTiming* web_rust_paint_timing_create_performance_paint_timing(JSVM*, FlatPtr paint_type, double rendering_update_end_time, bool has_presentation_time, double presentation_time);
}

GC::Ref<PerformancePaintTiming> PerformancePaintTiming::create(Utf16String const& paint_type, PaintTimingInfo paint_timing_info)
{
    return *web_rust_paint_timing_create_performance_paint_timing(
        JS::HostABI::vm_to_abi(JS::VM::the()),
        Utf16String { paint_type }.into_raw(),
        paint_timing_info.rendering_update_end_time,
        paint_timing_info.implementation_defined_presentation_time.has_value(),
        paint_timing_info.implementation_defined_presentation_time.value_or(0));
}

}
