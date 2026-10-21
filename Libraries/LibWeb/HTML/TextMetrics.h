/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/TextMetrics.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

// The measurements of a text, in CSS pixels, as the members of TextMetrics describe them. `TextMeasurements` of
// Bindings/Rust/src/html/text_metrics.rs mirrors it.
struct TextMeasurements {
    // x-direction
    double width { 0 };
    double actual_bounding_box_left { 0 };
    double actual_bounding_box_right { 0 };

    // y-direction
    double font_bounding_box_ascent { 0 };
    double font_bounding_box_descent { 0 };
    double actual_bounding_box_ascent { 0 };
    double actual_bounding_box_descent { 0 };
    double em_height_ascent { 0 };
    double em_height_descent { 0 };
    double hanging_baseline { 0 };
    double alphabetic_baseline { 0 };
    double ideographic_baseline { 0 };
};

// https://html.spec.whatwg.org/multipage/canvas.html#textmetrics
// The C++ side of a TextMetrics, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/html): what
// measureText() returns.
class WEB_API TextMetrics final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(TextMetrics, text_metrics)

public:
    [[nodiscard]] static GC::Ref<TextMetrics> create(TextMeasurements const&);
};

}
