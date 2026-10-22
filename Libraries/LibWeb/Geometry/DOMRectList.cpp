/*
 * Copyright (c) 2022, DerpyCrabs <derpycrabs@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Geometry/DOMRectList.h>

namespace Web::Geometry {

extern "C" {

// Defined in Bindings/Rust/src/geometry/exports.rs.
void* web_dom_rect_list_create(JSVM*, ReadonlySpan<Array<double, 4>> rects);
}

GC::Ref<DOMRectList> DOMRectList::create(ReadonlySpan<Gfx::DoubleRect> rects)
{
    Vector<Array<double, 4>> values;
    values.ensure_capacity(rects.size());
    for (auto const& rect : rects)
        values.unchecked_append({ rect.x(), rect.y(), rect.width(), rect.height() });
    return GC::Ref { *static_cast<DOMRectList*>(web_dom_rect_list_create(JS::HostABI::vm_to_abi(JS::VM::the()), values.span())) };
}

}
