/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Geometry/DOMRect.h>

namespace Web::Geometry {

extern "C" {

// Defined in Bindings/Rust/src/geometry/exports.rs.
void* web_dom_rect_create(JSVM*, double x, double y, double width, double height);
void web_dom_rect_set(void* rect, double x, double y, double width, double height);
}

GC::Ref<DOMRect> DOMRect::create(double x, double y, double width, double height)
{
    return GC::Ref { *static_cast<DOMRect*>(web_dom_rect_create(JS::HostABI::vm_to_abi(JS::VM::the()), x, y, width, height)) };
}

GC::Ref<DOMRect> DOMRect::create(Gfx::FloatRect const& rect)
{
    return create(rect.x(), rect.y(), rect.width(), rect.height());
}

void DOMRect::set(Gfx::DoubleRect const& rect)
{
    web_dom_rect_set(this, rect.x(), rect.y(), rect.width(), rect.height());
}

}
