/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/DOMPoint.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#dompoint
// The C++ side of a DOMPoint, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/geometry): what C++
// creates.
class WEB_API DOMPoint final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(DOMPoint, dom_point)

public:
    static GC::Ref<DOMPoint> create(double x = 0, double y = 0, double z = 0, double w = 1);

    double x() const;
    double y() const;

    void set_x_and_y(double x, double y);
};

}
