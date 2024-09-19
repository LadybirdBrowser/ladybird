/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/Platform.h>
#include <LibGfx/SkiaBackendContext.h>

#include <gpu/GrDirectContext.h>

#ifdef USE_VULKAN
#    include <gpu/ganesh/vk/GrVkDirectContext.h>
#    include <gpu/vk/VulkanBackendContext.h>
#    include <gpu/vk/VulkanExtensions.h>
#endif

#ifdef AK_OS_MACOS
#    include <gpu/GrBackendSurface.h>
#    include <gpu/ganesh/mtl/GrMtlBackendContext.h>
#    include <gpu/ganesh/mtl/GrMtlBackendSurface.h>
#    include <gpu/ganesh/mtl/GrMtlDirectContext.h>
#endif

namespace Gfx {

#ifdef USE_VULKAN
class SkiaVulkanBackendContext final : public SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaVulkanBackendContext);
    AK_MAKE_NONMOVABLE(SkiaVulkanBackendContext);

public:
    SkiaVulkanBackendContext(sk_sp<GrDirectContext> context, NonnullOwnPtr<skgpu::VulkanExtensions> extensions)
        : m_context(move(context))
        , m_extensions(move(extensions))
    {
    }

    ~SkiaVulkanBackendContext() override { }

    void flush_and_submit() override
    {
        m_context->flush();
        m_context->submit(GrSyncCpu::kYes);
    }

    sk_sp<SkSurface> create_surface(int width, int height)
    {
        auto image_info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
        return SkSurfaces::RenderTarget(m_context.get(), skgpu::Budgeted::kYes, image_info);
    }

    skgpu::VulkanExtensions const* extensions() const { return m_extensions.ptr(); }

    GrDirectContext* sk_context() const override { return m_context.get(); }

private:
    sk_sp<GrDirectContext> m_context;
    NonnullOwnPtr<skgpu::VulkanExtensions> m_extensions;
};

OwnPtr<SkiaBackendContext> SkiaBackendContext::create_vulkan_context(Core::VulkanContext& vulkan_context)
{
    skgpu::VulkanBackendContext backend_context;

    backend_context.fInstance = vulkan_context.instance;
    backend_context.fDevice = vulkan_context.logical_device;
    backend_context.fQueue = vulkan_context.graphics_queue;
    backend_context.fPhysicalDevice = vulkan_context.physical_device;
    backend_context.fMaxAPIVersion = vulkan_context.api_version;
    backend_context.fGetProc = [](char const* proc_name, VkInstance instance, VkDevice device) {
        if (device != VK_NULL_HANDLE) {
            return vkGetDeviceProcAddr(device, proc_name);
        }
        return vkGetInstanceProcAddr(instance, proc_name);
    };

    auto extensions = make<skgpu::VulkanExtensions>();
    backend_context.fVkExtensions = extensions.ptr();

    sk_sp<GrDirectContext> ctx = GrDirectContexts::MakeVulkan(backend_context);
    VERIFY(ctx);
    return make<SkiaVulkanBackendContext>(ctx, move(extensions));
}
#endif

#ifdef AK_OS_MACOS
class SkiaMetalBackendContext final : public SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaMetalBackendContext);
    AK_MAKE_NONMOVABLE(SkiaMetalBackendContext);

public:
    SkiaMetalBackendContext(sk_sp<GrDirectContext> context)
        : m_context(move(context))
    {
    }

    ~SkiaMetalBackendContext() override { }

    void flush_and_submit() override
    {
        m_context->flush();
        m_context->submit(GrSyncCpu::kYes);
    }

    GrDirectContext* sk_context() const override { return m_context.get(); }

private:
    sk_sp<GrDirectContext> m_context;
};

OwnPtr<SkiaBackendContext> SkiaBackendContext::create_metal_context(Core::MetalContext const& metal_context)
{
    GrMtlBackendContext backend_context;
    backend_context.fDevice.retain((GrMTLHandle)metal_context.device());
    backend_context.fQueue.retain((GrMTLHandle)metal_context.queue());
    sk_sp<GrDirectContext> ctx = GrDirectContexts::MakeMetal(backend_context);
    return make<SkiaMetalBackendContext>(ctx);
}

#endif

}
