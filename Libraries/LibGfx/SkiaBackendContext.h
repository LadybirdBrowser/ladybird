/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Noncopyable.h>
#include <LibThreading/Mutex.h>

#ifdef AK_OS_MACOS
#    include <LibGfx/MetalContext.h>
#endif

class GrDirectContext;
class SkSurface;

namespace Gfx {

class VulkanContext;
class MetalContext;

class SkiaBackendContext : public AtomicRefCounted<SkiaBackendContext> {
    AK_MAKE_NONCOPYABLE(SkiaBackendContext);
    AK_MAKE_NONMOVABLE(SkiaBackendContext);

public:
#ifdef USE_VULKAN
    static RefPtr<SkiaBackendContext> create_vulkan_context(NonnullRefPtr<VulkanContext> vulkan_context);
#endif

#ifdef AK_OS_MACOS
    static RefPtr<SkiaBackendContext> create_metal_context(NonnullRefPtr<MetalContext>);
#endif

    SkiaBackendContext() { }
    virtual ~SkiaBackendContext() { }

    virtual void flush_and_submit(SkSurface*) { }
    virtual GrDirectContext* sk_context() const = 0;

    virtual MetalContext& metal_context() = 0;
    virtual VulkanContext const& vulkan_context() = 0;

    void lock() { m_mutex.lock(); }
    void unlock() { m_mutex.unlock(); }

private:
    Threading::Mutex m_mutex;
};

}
