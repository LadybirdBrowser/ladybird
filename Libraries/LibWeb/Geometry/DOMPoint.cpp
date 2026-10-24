/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Geometry/DOMPoint.h>

namespace Web::Geometry {

extern "C" {

// Defined in Bindings/Rust/src/geometry/exports.rs.
void* web_dom_point_create(JSVM*, double x, double y, double z, double w);
void web_dom_point_coordinates(void const* point, Array<double, 4>* out_coordinates);
void web_dom_point_set_x_and_y(void* point, double x, double y);
}

GC::Ref<DOMPoint> DOMPoint::create(double x, double y, double z, double w)
{
    return GC::Ref { *static_cast<DOMPoint*>(web_dom_point_create(JS::HostABI::vm_to_abi(JS::VM::the()), x, y, z, w)) };
}

double DOMPoint::x() const
{
    Array<double, 4> coordinates;
    web_dom_point_coordinates(this, &coordinates);
    return coordinates[0];
}

double DOMPoint::y() const
{
    Array<double, 4> coordinates;
    web_dom_point_coordinates(this, &coordinates);
    return coordinates[1];
}

void DOMPoint::set_x_and_y(double x, double y)
{
    web_dom_point_set_x_and_y(this, x, y);
}

}
