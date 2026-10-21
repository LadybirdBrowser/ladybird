/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CanvasGradient.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasgradient
// The C++ side of a CanvasGradient, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/html): what the
// 2D contexts create and paint with.
class WEB_API CanvasGradient final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(CanvasGradient, canvas_gradient)

public:
    static GC::Ref<CanvasGradient> create_linear(double x0, double y0, double x1, double y1);
    static GC::Ref<CanvasGradient> create_radial(double x0, double y0, double r0, double x1, double y1, double r1);
    static GC::Ref<CanvasGradient> create_conic(double start_angle, double x, double y);

    // Whether one of the color stops is not transparent.
    bool is_visible() const;
};

}
