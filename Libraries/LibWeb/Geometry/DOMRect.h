/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Rect.h>
#include <LibWeb/Bindings/DOMRect.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#domrect
// The C++ side of a DOMRect, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/geometry): what C++
// creates, and changes as script can.
class WEB_API DOMRect final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(DOMRect, dom_rect)

public:
    static GC::Ref<DOMRect> create(double x = 0, double y = 0, double width = 0, double height = 0);
    static GC::Ref<DOMRect> create(Gfx::FloatRect const&);

    // Sets the x coordinate, y coordinate, width dimension and height dimension to those of the rectangle.
    void set(Gfx::DoubleRect const&);
};

}
