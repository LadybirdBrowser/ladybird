/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/XRLayerPrototype.h>
#include <LibWeb/WebXR/XRLayer.h>

namespace Web::WebXR {

GC_DEFINE_ALLOCATOR(XRLayer);

XRLayer::XRLayer(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void XRLayer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
