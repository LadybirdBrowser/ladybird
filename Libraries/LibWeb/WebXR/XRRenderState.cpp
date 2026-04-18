/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/XRRenderState.h>
#include <LibWeb/WebXR/XRRenderState.h>

namespace Web::WebXR {

GC_DEFINE_ALLOCATOR(XRRenderState);

XRRenderState::XRRenderState(JS::Realm& realm)
    : PlatformObject(realm)
{
}

}
