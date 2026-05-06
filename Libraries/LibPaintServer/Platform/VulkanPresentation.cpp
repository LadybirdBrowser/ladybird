/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/NumericLimits.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/VulkanImage.h>
#include <LibPaintServer/BrokerOfPaintServer.h>
#include <LibPaintServer/Policy.h>
#include <LibPaintServer/Presentation.h>
#include <drm/drm_fourcc.h>

namespace WebView {

class VulkanPresentation final : public Presentation {
public:
    virtual void ensure_broker_owned_presentation_buffers(BrokerOfPaintServer& broker, PaintServer::SurfaceID surface_id, Gfx::IntSize requested_size) override
    {
        auto& surface_state = m_surfaces.ensure(surface_id, [] { return SurfaceState {}; });

        Gfx::IntSize buffer_size = PaintServer::presentation_buffer_capacity_for_size(requested_size);

        if (!surface_state.pool.is_empty() && surface_state.buffer_size.contains(requested_size))
            return;

        surface_state.pool.clear();
        surface_state.buffer_size = buffer_size;

        if (!m_vulkan_context.has_value()) {
            auto context_or_error = Gfx::create_vulkan_context();
            if (context_or_error.is_error()) {
                warnln("VulkanPresentation: failed to create Vulkan context: {}", context_or_error.error());
                return;
            }
            m_vulkan_context = context_or_error.release_value();
        }

        Gfx::BitmapFormat const pixel_format = Gfx::BitmapFormat::BGRA8888;
        VkFormat const vk_format = VK_FORMAT_B8G8R8A8_UNORM;

        u32 drm_format = Gfx::vk_format_to_drm_format(vk_format);
        if (drm_format == DRM_FORMAT_INVALID)
            return;

        static Atomic<u64> s_next_presentation_image_id { 0x8000'0000'0000'0001ULL };

        Vector<u64> image_ids;
        Vector<Gfx::SharedImagePayload> image_payloads;
        image_ids.ensure_capacity(PaintServer::PRESENTATION_BUFFER_COUNT);
        image_payloads.ensure_capacity(PaintServer::PRESENTATION_BUFFER_COUNT);

        size_t image_create_error_count = 0;
        for (size_t i = 0; i < PaintServer::PRESENTATION_BUFFER_COUNT; ++i) {
            u64 image_id = s_next_presentation_image_id.fetch_add(1, MemoryOrder::memory_order_relaxed);
            if (image_id == 0)
                continue;

            auto image_or_error = Gfx::create_dma_buf_vulkan_image(*m_vulkan_context, buffer_size, vk_format, true);
            if (image_or_error.is_error()) {
                ++image_create_error_count;
                if (image_create_error_count <= 3)
                    warnln("VulkanPresentation: create_dma_buf_vulkan_image failed surface={} buffer_size={} vk_format={} error={}", surface_id, buffer_size, to_underlying(vk_format), image_or_error.error());
                continue;
            }
            NonnullRefPtr<Gfx::VulkanImage> image = image_or_error.release_value();

            int dma_buf_fd = image->get_dma_buf_fd();
            if (dma_buf_fd < 0) {
                warnln("VulkanPresentation: get_dma_buf_fd failed surface={} image_id={} vk_format={} modifier={}", surface_id, image_id, to_underlying(vk_format), image->info.modifier);
                continue;
            }

            Vector<IPC::File> broker_files;
            broker_files.append(IPC::File::adopt_fd(dma_buf_fd));

            Vector<IPC::File> gpu_files;
            auto cloned_or_error = IPC::File::clone_fd(broker_files[0].fd());
            if (cloned_or_error.is_error()) {
                warnln("VulkanPresentation: clone_fd failed surface={} image_id={} error={}", surface_id, image_id, cloned_or_error.error());
                continue;
            }
            gpu_files.append(cloned_or_error.release_value());

            if (image->info.row_pitch > NumericLimits<u32>::max()) {
                continue;
            }
            u32 stride = static_cast<u32>(image->info.row_pitch);

            surface_state.pool.set(image_id, LinuxPresentationBuffer {
                                                 .image = image,
                                                 .dmabuf_files = move(broker_files),
                                             });

            image_ids.append(image_id);
            image_payloads.append(Gfx::SharedImagePayload({
                                                              .size = buffer_size,
                                                              .row_bytes = stride,
                                                              .pixel_format = pixel_format,
                                                              .color_space = Gfx::BitmapColorSpace::SRGB,
                                                              .alpha_type = Gfx::BitmapAlpha::Premultiplied,
                                                              .origin = Gfx::BitmapOrigin::TopLeft,
                                                          },
                Gfx::LinuxDmaBufPayload {
                    .drm_format = drm_format,
                    .stride = stride,
                    .offset = 0,
                    .file = move(gpu_files[0]),
                }));
        }

        if (image_ids.is_empty()) {
            warnln("VulkanPresentation: produced zero buffers surface={} buffer_size={} vk_format={} drm_format={} (image_create_errors={})", surface_id, buffer_size, to_underlying(vk_format), drm_format, image_create_error_count);
            return;
        }

        broker.async_register_presentation_buffers(surface_id, move(image_ids), move(image_payloads), buffer_size);
    }

    virtual bool has_pool_image(PaintServer::SurfaceID surface_id, u64 image_id) const override
    {
        auto surface_it = m_surfaces.find(surface_id);
        if (surface_it == m_surfaces.end())
            return false;
        return surface_it->value.pool.contains(image_id);
    }

    virtual Optional<void*> platform_surface_handle_for_image(PaintServer::SurfaceID surface_id, u64 image_id) override
    {
        (void)surface_id;
        (void)image_id;
        return {};
    }

    virtual Optional<LinuxDmaBufPresentationBuffer> clone_linux_dmabuf_presentation_buffer(PaintServer::SurfaceID surface_id, u64 image_id) const override
    {
        auto surface_it = m_surfaces.find(surface_id);
        if (surface_it == m_surfaces.end())
            return {};

        auto it = surface_it->value.pool.find(image_id);
        if (it == surface_it->value.pool.end())
            return {};

        auto const& buffer = it->value;

        u32 drm_format = Gfx::vk_format_to_drm_format(buffer.image->info.format);
        if (drm_format == DRM_FORMAT_INVALID)
            return {};

        if (buffer.image->info.row_pitch > NumericLimits<u32>::max())
            return {};

        auto cloned_or_error = IPC::File::clone_fd(buffer.dmabuf_files[0].fd());
        if (cloned_or_error.is_error())
            return {};

        return LinuxDmaBufPresentationBuffer {
            .drm_format = drm_format,
            .stride = static_cast<u32>(buffer.image->info.row_pitch),
            .offset = 0,
            .fd = cloned_or_error.release_value(),
            .size = surface_it->value.buffer_size,
        };
    }

    virtual RefPtr<Gfx::Bitmap const> bitmap_for_presentation_image(PaintServer::SurfaceID surface_id, u64 image_id) const override
    {
        auto surface_it = m_surfaces.find(surface_id);
        if (surface_it == m_surfaces.end())
            return nullptr;

        auto it = surface_it->value.pool.find(image_id);
        if (it == surface_it->value.pool.end())
            return nullptr;

        auto const& buffer = it->value;
        u32 width = static_cast<u32>(surface_it->value.buffer_size.width());
        u32 height = static_cast<u32>(surface_it->value.buffer_size.height());

        u32 drm_format = Gfx::vk_format_to_drm_format(buffer.image->info.format);
        if (drm_format != DRM_FORMAT_ARGB8888)
            return nullptr;
        if (buffer.image->info.modifier != DRM_FORMAT_MOD_LINEAR)
            return nullptr;
        if (buffer.dmabuf_files.size() != 1)
            return nullptr;
        if (width == 0 || height == 0)
            return nullptr;
        if (buffer.image->info.row_pitch > NumericLimits<u32>::max())
            return nullptr;

        auto bitmap_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { static_cast<int>(width), static_cast<int>(height) });
        if (bitmap_or_error.is_error())
            return nullptr;
        NonnullRefPtr<Gfx::Bitmap> bitmap = bitmap_or_error.release_value();

        u32 stride = static_cast<u32>(buffer.image->info.row_pitch);
        if (stride < bitmap->pitch())
            return nullptr;

        auto cloned_or_error = IPC::File::clone_fd(buffer.dmabuf_files[0].fd());
        if (cloned_or_error.is_error())
            return nullptr;

        size_t mapped_size = static_cast<size_t>(stride) * height;
        auto mapped_file = Core::MappedFile::map_from_fd_and_close(cloned_or_error.release_value().take_fd(), "presentation-buffer"sv);
        if (mapped_file.is_error())
            return nullptr;
        if (mapped_file.value()->bytes().size() < mapped_size)
            return nullptr;

        auto mapped_bytes = mapped_file.value()->bytes();
        for (u32 row = 0; row < height; ++row) {
            size_t source_offset = static_cast<size_t>(row) * stride;
            __builtin_memcpy(bitmap->scanline_u8(static_cast<int>(row)), mapped_bytes.offset(source_offset), bitmap->pitch());
        }

        return bitmap;
    }

    virtual void clear_surface(PaintServer::SurfaceID surface_id) override
    {
        m_surfaces.remove(surface_id);
    }

private:
    struct LinuxPresentationBuffer {
        NonnullRefPtr<Gfx::VulkanImage> image;
        Vector<IPC::File> dmabuf_files;
    };

    struct SurfaceState {
        HashMap<u64, LinuxPresentationBuffer> pool;
        Gfx::IntSize buffer_size;
    };

    HashMap<PaintServer::SurfaceID, SurfaceState> m_surfaces;
    Optional<Gfx::VulkanContext> m_vulkan_context;
};

OwnPtr<Presentation> create_presentation_backend()
{
    return make<VulkanPresentation>();
}

}
