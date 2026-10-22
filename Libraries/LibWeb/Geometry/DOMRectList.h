/*
 * Copyright (c) 2022, DerpyCrabs <derpycrabs@gmail.com>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Bindings/DOMRectList.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#domrectlist
// The C++ side of a DOMRectList, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/geometry): what
// C++ creates.
class WEB_API DOMRectList final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(DOMRectList, dom_rect_list)

public:
    // A new list of new DOMRects with the rectangles.
    static GC::Ref<DOMRectList> create(ReadonlySpan<Gfx::DoubleRect>);
};

}
