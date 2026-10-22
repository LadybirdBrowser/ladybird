/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/DOMPointReadOnly.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#dompointreadonly
// The C++ side of a DOMPointReadOnly, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/geometry): what
// C++ creates.
class WEB_API DOMPointReadOnly final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(DOMPointReadOnly, dom_point_read_only)

public:
    static GC::Ref<DOMPointReadOnly> create(double x = 0, double y = 0, double z = 0, double w = 1);
};

}
