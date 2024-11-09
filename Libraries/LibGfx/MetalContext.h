/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if !defined(AK_OS_MACOS)
static_assert(false, "This file must only be used for macOS");
#endif

#include <AK/Forward.h>
#include <LibCore/IOSurface.h>

namespace Gfx {

class MetalTexture {
public:
    virtual void const* texture() const = 0;
    virtual size_t width() const = 0;
    virtual size_t height() const = 0;

    virtual ~MetalTexture() {};
};

class MetalContext {
public:
    virtual void const* device() const = 0;
    virtual void const* queue() const = 0;

    virtual OwnPtr<MetalTexture> create_texture_from_iosurface(Core::IOSurfaceHandle const&) = 0;

    virtual ~MetalContext() {};
};

OwnPtr<MetalContext> get_metal_context();

}
