/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <LibGfx/Bitmap.h>
#ifdef USE_DIRECTX
#    include <LibGfx/Direct3DContext.h>
#endif
#include <LibGfx/SkiaBackendContext.h>

#include <core/SkSurface.h>
#include <gpu/ganesh/GrDirectContext.h>

#ifdef USE_DIRECTX
#    include <AK/Windows.h>
#    include <gpu/ganesh/d3d/GrD3DBackendContext.h>
#    include <gpu/ganesh/d3d/GrD3DDirectContext.h>
#endif

#ifdef USE_VULKAN
#    include <LibGfx/SkiaVulkanMemoryAllocator.h>
#    include <gpu/ganesh/vk/GrVkDirectContext.h>
#    include <gpu/vk/VulkanBackendContext.h>
#    include <gpu/vk/VulkanExtensions.h>
#endif

#ifdef AK_OS_MACOS
#    include <gpu/ganesh/GrBackendSurface.h>
#    include <gpu/ganesh/mtl/GrMtlBackendContext.h>
#    include <gpu/ganesh/mtl/GrMtlBackendSurface.h>
#    include <gpu/ganesh/mtl/GrMtlDirectContext.h>
#endif

namespace Gfx {

#if defined(AK_OS_MACOS) || defined(USE_DIRECTX) || defined(USE_VULKAN)
static constexpr size_t skia_resource_cache_limit = 256 * MiB;
#endif
static constexpr auto skia_deferred_cleanup_interval = AK::Duration::from_seconds(1);
static constexpr auto skia_aggressive_cleanup_interval = AK::Duration::from_seconds(5);
static constexpr auto skia_deferred_cleanup_resource_age = std::chrono::seconds(5);
static constexpr auto skia_resource_cache_high_watermark = 384 * MiB;
static constexpr auto skia_resource_cache_critical_watermark = 512 * MiB;

static auto& main_thread_context()
{
    static NeverDestroyed<RefPtr<SkiaBackendContext>> context;
    return *context;
}

#if defined(AK_OS_MACOS) || defined(USE_DIRECTX) || defined(USE_VULKAN)
static void invoke_async_flush_callback(void* context)
{
    auto* callback = static_cast<Function<void()>*>(context);
    auto callback_to_invoke = move(*callback);
    delete callback;
    callback_to_invoke();
}

static void flush_and_submit_async_to_context(GrDirectContext& context, SkSurface* surface, Function<void()>&& callback)
{
    GrFlushInfo flush_info {};
    flush_info.fFinishedProc = invoke_async_flush_callback;
    flush_info.fFinishedContext = new Function<void()>(move(callback));
    context.flush(surface, SkSurfaces::BackendSurfaceAccess::kPresent, flush_info);
    VERIFY(context.submit(GrSyncCpu::kNo));
}
#endif

void SkiaBackendContext::check_async_work_completion()
{
    if (auto* context = sk_context())
        context->checkAsyncWorkCompletion();
}

void SkiaBackendContext::flush_and_submit(SkSurface* surface)
{
    flush_and_submit_impl(surface);

    perform_post_flush_cleanup();
}

void SkiaBackendContext::flush_and_submit_async(SkSurface* surface, Function<void()>&& callback)
{
    flush_and_submit_async_impl(surface, move(callback));

    perform_post_flush_cleanup();
}

void SkiaBackendContext::perform_post_flush_cleanup()
{
    auto* context = sk_context();
    if (!context)
        return;

    static thread_local Optional<MonotonicTime> s_last_deferred_cleanup;
    static thread_local Optional<MonotonicTime> s_last_aggressive_cleanup;

    auto const now = MonotonicTime::now();
    if (s_last_deferred_cleanup.has_value() && now - *s_last_deferred_cleanup < skia_deferred_cleanup_interval)
        return;

    s_last_deferred_cleanup = now;
    context->performDeferredCleanup(skia_deferred_cleanup_resource_age);

    size_t resource_bytes = 0;
    context->getResourceCacheUsage(nullptr, &resource_bytes);
    if (resource_bytes < skia_resource_cache_high_watermark)
        return;
    if (s_last_aggressive_cleanup.has_value() && now - *s_last_aggressive_cleanup < skia_aggressive_cleanup_interval)
        return;

    s_last_aggressive_cleanup = now;
    context->performDeferredCleanup(std::chrono::milliseconds(0));
    context->getResourceCacheUsage(nullptr, &resource_bytes);
    if (resource_bytes >= skia_resource_cache_critical_watermark)
        context->purgeUnlockedResources(GrPurgeResourceOptions::kScratchResourcesOnly);
}

void SkiaBackendContext::initialize_gpu_backend()
{
    VERIFY(!main_thread_context());

    main_thread_context() = create_independent_gpu_backend();
}

RefPtr<SkiaBackendContext> SkiaBackendContext::create_independent_gpu_backend()
{
#ifdef AK_OS_MACOS
    auto metal_context = get_metal_context();
    if (!metal_context)
        return {};
    return create_metal_context(*metal_context);
#elif defined(USE_DIRECTX)
    auto maybe_direct3d_context = Gfx::create_direct3d_context();
    if (maybe_direct3d_context.is_error()) {
        dbgln("Direct3D 12 context creation failed: {}", maybe_direct3d_context.error());
        return {};
    }
    return create_direct3d_context(maybe_direct3d_context.release_value());
#elif defined(USE_VULKAN)
    auto maybe_vulkan_context = Gfx::create_vulkan_context();
    if (maybe_vulkan_context.is_error()) {
        dbgln("Vulkan context creation failed: {}", maybe_vulkan_context.error());
        return {};
    }
    auto vulkan_context = maybe_vulkan_context.release_value();
    return create_vulkan_context(vulkan_context);
#else
    return {};
#endif
}

RefPtr<SkiaBackendContext> SkiaBackendContext::the_main_thread_context()
{
    return main_thread_context();
}

#ifdef USE_DIRECTX
class SkiaDirect3DBackendContext final : public SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaDirect3DBackendContext);
    AK_MAKE_NONMOVABLE(SkiaDirect3DBackendContext);

public:
    SkiaDirect3DBackendContext(sk_sp<GrDirectContext> context, NonnullRefPtr<Direct3DContext> direct3d_context)
        : m_context(move(context))
        , m_direct3d_context(move(direct3d_context))
    {
    }

    ~SkiaDirect3DBackendContext() override
    {
        m_context.reset();
    }

    void flush_and_submit_impl(SkSurface* surface) override
    {
        GrFlushInfo const flush_info {};
        m_context->flush(surface, SkSurfaces::BackendSurfaceAccess::kPresent, flush_info);
        m_context->submit(GrSyncCpu::kYes);
    }

    void flush_and_submit_async_impl(SkSurface* surface, Function<void()>&& callback) override
    {
        flush_and_submit_async_to_context(*m_context, surface, move(callback));
    }

    GrDirectContext* sk_context() const override { return m_context.get(); }

    VulkanContext const& vulkan_context() override { VERIFY_NOT_REACHED(); }

    MetalContext& metal_context() override { VERIFY_NOT_REACHED(); }

private:
    sk_sp<GrDirectContext> m_context;
    NonnullRefPtr<Direct3DContext> m_direct3d_context;
};

RefPtr<SkiaBackendContext> SkiaBackendContext::create_direct3d_context(NonnullRefPtr<Direct3DContext> direct3d_context)
{
    GrD3DBackendContext backend_context;
    backend_context.fAdapter.retain(direct3d_context->adapter());
    backend_context.fDevice.retain(direct3d_context->device());
    backend_context.fQueue.retain(direct3d_context->queue());
    backend_context.fProtectedContext = GrProtected::kNo;

    auto context = GrDirectContexts::MakeD3D(backend_context);
    if (!context) {
        dbgln("Skia Direct3D context creation failed");
        return {};
    }

    context->setResourceCacheLimit(skia_resource_cache_limit);
    return adopt_ref(*new SkiaDirect3DBackendContext(move(context), move(direct3d_context)));
}
#endif

#ifdef USE_VULKAN
class SkiaVulkanBackendContext final : public SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaVulkanBackendContext);
    AK_MAKE_NONMOVABLE(SkiaVulkanBackendContext);

public:
    SkiaVulkanBackendContext(sk_sp<GrDirectContext> context, VulkanContext const& vulkan_context, NonnullOwnPtr<skgpu::VulkanExtensions> extensions)
        : m_context(move(context))
        , m_extensions(move(extensions))
        , m_vulkan_context(vulkan_context)
    {
    }

    ~SkiaVulkanBackendContext() override
    {
        m_context.reset();
#    ifdef USE_VULKAN_DMABUF_IMAGES
        if (m_vulkan_context.command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(m_vulkan_context.logical_device, m_vulkan_context.command_pool, nullptr);
#    endif
        if (m_vulkan_context.logical_device != VK_NULL_HANDLE)
            vkDestroyDevice(m_vulkan_context.logical_device, nullptr);
        if (m_vulkan_context.instance != VK_NULL_HANDLE)
            vkDestroyInstance(m_vulkan_context.instance, nullptr);
    }

    void flush_and_submit_impl(SkSurface* surface) override
    {
        GrFlushInfo const flush_info {};
        m_context->flush(surface, SkSurfaces::BackendSurfaceAccess::kPresent, flush_info);
        m_context->submit(GrSyncCpu::kYes);
    }

    void flush_and_submit_async_impl(SkSurface* surface, Function<void()>&& callback) override
    {
        flush_and_submit_async_to_context(*m_context, surface, move(callback));
    }

    skgpu::VulkanExtensions const* extensions() const { return m_extensions.ptr(); }

    GrDirectContext* sk_context() const override { return m_context.get(); }

    VulkanContext const& vulkan_context() override { return m_vulkan_context; }

    MetalContext& metal_context() override { VERIFY_NOT_REACHED(); }

private:
    sk_sp<GrDirectContext> m_context;
    NonnullOwnPtr<skgpu::VulkanExtensions> m_extensions;
    VulkanContext const m_vulkan_context;
};

RefPtr<SkiaBackendContext> SkiaBackendContext::create_vulkan_context(VulkanContext const& vulkan_context)
{
    skgpu::VulkanBackendContext backend_context;

    backend_context.fInstance = vulkan_context.instance;
    backend_context.fDevice = vulkan_context.logical_device;
    backend_context.fQueue = vulkan_context.graphics_queue;
    backend_context.fGraphicsQueueIndex = vulkan_context.graphics_queue_family;
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

    backend_context.fMemoryAllocator = create_skia_vulkan_memory_allocator(vulkan_context);
    VERIFY(backend_context.fMemoryAllocator);

    sk_sp<GrDirectContext> ctx = GrDirectContexts::MakeVulkan(backend_context);
    VERIFY(ctx);
    ctx->setResourceCacheLimit(skia_resource_cache_limit);
    return adopt_ref(*new SkiaVulkanBackendContext(ctx, vulkan_context, move(extensions)));
}
#endif

#ifdef AK_OS_MACOS
class SkiaMetalBackendContext final : public SkiaBackendContext {
    AK_MAKE_NONCOPYABLE(SkiaMetalBackendContext);
    AK_MAKE_NONMOVABLE(SkiaMetalBackendContext);

public:
    SkiaMetalBackendContext(sk_sp<GrDirectContext> context, NonnullRefPtr<MetalContext> metal_context)
        : m_context(move(context))
        , m_metal_context(move(metal_context))
    {
    }

    ~SkiaMetalBackendContext() override
    {
        m_context.reset();
    }

    void flush_and_submit_impl(SkSurface* surface) override
    {
        GrFlushInfo const flush_info {};
        m_context->flush(surface, SkSurfaces::BackendSurfaceAccess::kPresent, flush_info);
        m_context->submit(GrSyncCpu::kYes);
    }

    void flush_and_submit_async_impl(SkSurface* surface, Function<void()>&& callback) override
    {
        flush_and_submit_async_to_context(*m_context, surface, move(callback));
    }

    GrDirectContext* sk_context() const override { return m_context.get(); }

    VulkanContext const& vulkan_context() override { VERIFY_NOT_REACHED(); }

    MetalContext& metal_context() override { return m_metal_context; }

private:
    sk_sp<GrDirectContext> m_context;
    NonnullRefPtr<MetalContext> m_metal_context;
};

RefPtr<SkiaBackendContext> SkiaBackendContext::create_metal_context(NonnullRefPtr<MetalContext> metal_context)
{
    GrMtlBackendContext backend_context;
    backend_context.fDevice.retain(metal_context->device());
    backend_context.fQueue.retain(metal_context->queue());
    sk_sp<GrDirectContext> ctx = GrDirectContexts::MakeMetal(backend_context);
    VERIFY(ctx);
    ctx->setResourceCacheLimit(skia_resource_cache_limit);
    return adopt_ref(*new SkiaMetalBackendContext(move(ctx), move(metal_context)));
}
#endif

}
