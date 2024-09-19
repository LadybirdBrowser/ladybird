/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>

#ifdef AK_OS_MACOS
#    include <LibCore/MetalContext.h>
#endif

#ifdef USE_VULKAN
#    include <LibCore/VulkanContext.h>
#endif

class GrDirectContext;

namespace Gfx {

class SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaBackendContext);
    AK_MAKE_NONMOVABLE(SkiaBackendContext);

public:
#ifdef USE_VULKAN
    static OwnPtr<SkiaBackendContext> create_vulkan_context(Core::VulkanContext&);
#endif

#ifdef AK_OS_MACOS
    static OwnPtr<Gfx::SkiaBackendContext> create_metal_context(Core::MetalContext const&);
#endif

    SkiaBackendContext() {};
    virtual ~SkiaBackendContext() {};

    virtual void flush_and_submit() {};
    virtual GrDirectContext* sk_context() const = 0;
};

}
