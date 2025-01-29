/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/RefCounted.h>

#ifdef USE_VULKAN
#    include <LibGfx/VulkanContext.h>
#endif

#ifdef AK_OS_MACOS
#    include <LibGfx/MetalContext.h>
#endif

class GrDirectContext;
class SkSurface;

namespace Gfx {

class MetalContext;

class SkiaBackendContext : public RefCounted<SkiaBackendContext> {
    AK_MAKE_NONCOPYABLE(SkiaBackendContext);
    AK_MAKE_NONMOVABLE(SkiaBackendContext);

public:
#ifdef USE_VULKAN
    static RefPtr<SkiaBackendContext> create_vulkan_context(VulkanContext&);
#endif

#ifdef AK_OS_MACOS
    static RefPtr<SkiaBackendContext> create_metal_context(MetalContext&);
#endif

    SkiaBackendContext() { }
    virtual ~SkiaBackendContext() { }

    virtual void flush_and_submit(SkSurface*) { }
    virtual GrDirectContext* sk_context() const = 0;

    virtual MetalContext& metal_context() = 0;
};

}
