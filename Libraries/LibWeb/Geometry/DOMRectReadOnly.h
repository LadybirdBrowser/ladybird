/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/DOMRectReadOnly.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#domrectreadonly
// The C++ side of a DOMRectReadOnly, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/geometry): what
// C++ creates and passes on. A DOMRectReadOnly that C++ receives from script can be a DOMRect.
class WEB_API DOMRectReadOnly final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(DOMRectReadOnly, dom_rect_read_only)

public:
    static GC::Ref<DOMRectReadOnly> create(double x = 0, double y = 0, double width = 0, double height = 0);
};

}
