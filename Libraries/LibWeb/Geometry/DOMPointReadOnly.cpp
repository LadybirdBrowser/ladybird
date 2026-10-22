/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Geometry/DOMPointReadOnly.h>

namespace Web::Geometry {

extern "C" {

// Defined in Bindings/Rust/src/geometry/exports.rs.
void* web_dom_point_read_only_create(JSVM*, double x, double y, double z, double w);
}

GC::Ref<DOMPointReadOnly> DOMPointReadOnly::create(double x, double y, double z, double w)
{
    return GC::Ref { *static_cast<DOMPointReadOnly*>(web_dom_point_read_only_create(JS::HostABI::vm_to_abi(JS::VM::the()), x, y, z, w)) };
}

}
