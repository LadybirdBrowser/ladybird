/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Noncopyable.h>
#include <LibThreading/Mutex.h>

#ifdef USE_VULKAN
#    include <LibGfx/VulkanContext.h>
#endif

#ifdef AK_OS_MACOS
#    include <LibGfx/MetalContext.h>
#endif

class GrDirectContext;
class SkSurface;

namespace Gfx {

struct VulkanContext;
class MetalContext;
class Direct3DContext;

class SkiaBackendContext : public AtomicRefCounted<SkiaBackendContext> {
    AK_MAKE_NONCOPYABLE(SkiaBackendContext);
    AK_MAKE_NONMOVABLE(SkiaBackendContext);

public:
#ifdef USE_VULKAN
    static RefPtr<SkiaBackendContext> create_vulkan_context(const VulkanContext& vulkan_context);
#endif

#ifdef AK_OS_MACOS
    static RefPtr<SkiaBackendContext> create_metal_context(NonnullRefPtr<MetalContext>);
#endif

#if defined(AK_OS_WINDOWS)
    static RefPtr<SkiaBackendContext> create_direct3d_context(NonnullOwnPtr<Direct3DContext>);
#endif

    SkiaBackendContext() { }
    virtual ~SkiaBackendContext() { }

    virtual void flush_and_submit(SkSurface*) { }
    virtual GrDirectContext* sk_context() const = 0;

    virtual MetalContext& metal_context() = 0;
    virtual VulkanContext const& vulkan_context() = 0;
    virtual Direct3DContext const& direct3d_context() = 0;

    void lock() { m_mutex.lock(); }
    void unlock() { m_mutex.unlock(); }

private:
    Threading::Mutex m_mutex;
};

}
