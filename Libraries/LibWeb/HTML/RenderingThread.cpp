/*
 * Copyright (c) 2025-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibThreading/Thread.h>
#include <LibWeb/HTML/RenderingThread.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/ExternalContentSource.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <AK/Array.h>
#    include <LibGfx/VulkanImage.h>
#    include <libdrm/drm_fourcc.h>
#endif

#include <core/SkCanvas.h>
#include <core/SkColor.h>

#include <LibCore/Platform/ScopedAutoreleasePool.h>

namespace Web::HTML {

struct BackingStoreState {
    RefPtr<Gfx::PaintingSurface> front_store;
    RefPtr<Gfx::PaintingSurface> back_store;
    i32 front_bitmap_id { -1 };
    i32 back_bitmap_id { -1 };

    void swap()
    {
        AK::swap(front_store, back_store);
        AK::swap(front_bitmap_id, back_bitmap_id);
    }

    bool is_valid() const { return front_store && back_store; }
};

struct UpdateDisplayListCommand {
    NonnullRefPtr<Painting::DisplayList> display_list;
    Painting::ScrollStateSnapshot scroll_state_snapshot;
};

struct UpdateBackingStoresCommand {
    Gfx::IntSize size;
    i32 front_bitmap_id;
    i32 back_bitmap_id;
    Function<void(i32, Gfx::SharedImage, i32, Gfx::SharedImage)> allocation_callback;
};

struct ScreenshotCommand {
    NonnullRefPtr<Gfx::PaintingSurface> target_surface;
    Function<void()> callback;
};

using CompositorCommand = Variant<UpdateDisplayListCommand, UpdateBackingStoresCommand, ScreenshotCommand>;

struct BackingStorePair {
    RefPtr<Gfx::PaintingSurface> front;
    RefPtr<Gfx::PaintingSurface> back;
};

#ifdef USE_VULKAN
static NonnullRefPtr<Gfx::PaintingSurface> create_gpu_painting_surface_with_bitmap_flush(Gfx::IntSize size, Gfx::SharedImageBuffer& buffer, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context);
    auto bitmap = buffer.bitmap();
    surface->on_flush = [bitmap = move(bitmap)](auto& surface) {
        surface.read_into_bitmap(*bitmap);
    };
    return surface;
}
#endif

static BackingStorePair create_shareable_bitmap_backing_stores([[maybe_unused]] Gfx::IntSize size, Gfx::SharedImageBuffer& front_buffer, Gfx::SharedImageBuffer& back_buffer, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
#ifdef AK_OS_MACOS
    if (skia_backend_context) {
        return {
            .front = Gfx::PaintingSurface::create_from_shared_image_buffer(front_buffer, *skia_backend_context),
            .back = Gfx::PaintingSurface::create_from_shared_image_buffer(back_buffer, *skia_backend_context),
        };
    }
#else
#    ifdef USE_VULKAN
    if (skia_backend_context) {
        return {
            .front = create_gpu_painting_surface_with_bitmap_flush(size, front_buffer, skia_backend_context),
            .back = create_gpu_painting_surface_with_bitmap_flush(size, back_buffer, skia_backend_context),
        };
    }
#    else
    (void)skia_backend_context;
#    endif
#endif

    return {
        .front = Gfx::PaintingSurface::wrap_bitmap(*front_buffer.bitmap()),
        .back = Gfx::PaintingSurface::wrap_bitmap(*back_buffer.bitmap()),
    };
}

#ifdef USE_VULKAN_DMABUF_IMAGES
struct DMABufBackingStorePair {
    RefPtr<Gfx::PaintingSurface> front;
    RefPtr<Gfx::PaintingSurface> back;
    Gfx::SharedImage front_shared_image;
    Gfx::SharedImage back_shared_image;
};

static ErrorOr<DMABufBackingStorePair> create_linear_dmabuf_backing_stores(Gfx::IntSize size, Gfx::SkiaBackendContext& skia_backend_context)
{
    auto const& vulkan_context = skia_backend_context.vulkan_context();
    static constexpr Array<uint64_t, 1> linear_modifiers = { DRM_FORMAT_MOD_LINEAR };
    auto front_image = TRY(Gfx::create_shared_vulkan_image(vulkan_context, size.width(), size.height(), VK_FORMAT_B8G8R8A8_UNORM, linear_modifiers.span()));
    auto back_image = TRY(Gfx::create_shared_vulkan_image(vulkan_context, size.width(), size.height(), VK_FORMAT_B8G8R8A8_UNORM, linear_modifiers.span()));
    auto front_shared_image = Gfx::duplicate_shared_image(*front_image);
    auto back_shared_image = Gfx::duplicate_shared_image(*back_image);

    return DMABufBackingStorePair {
        .front = Gfx::PaintingSurface::create_from_vkimage(skia_backend_context, move(front_image), Gfx::PaintingSurface::Origin::TopLeft),
        .back = Gfx::PaintingSurface::create_from_vkimage(skia_backend_context, move(back_image), Gfx::PaintingSurface::Origin::TopLeft),
        .front_shared_image = move(front_shared_image),
        .back_shared_image = move(back_shared_image),
    };
}
#endif

class RenderingThread::ThreadData final : public AtomicRefCounted<ThreadData> {
public:
    ThreadData(NonnullRefPtr<Core::WeakEventLoopReference>&& main_thread_event_loop, RenderingThread::PresentationCallback presentation_callback)
        : m_main_thread_event_loop(move(main_thread_event_loop))
        , m_presentation_callback(move(presentation_callback))
    {
    }

    ~ThreadData() = default;

    void set_presentation_mode(RenderingThread::PresentationMode mode)
    {
        Threading::MutexLocker const locker { m_mutex };
        m_presentation_mode = move(mode);
    }

    void exit()
    {
        Threading::MutexLocker const locker { m_mutex };
        m_exit = true;
        m_command_ready.signal();
        m_ready_to_paint.signal();
        m_frame_completed.broadcast();
    }

    void enqueue_command(CompositorCommand&& command)
    {
        Threading::MutexLocker const locker { m_mutex };
        m_command_queue.enqueue(move(command));
        m_command_ready.signal();
    }

    u64 set_needs_present(Gfx::IntRect viewport_rect)
    {
        Threading::MutexLocker const locker { m_mutex };
        m_needs_present = true;
        m_pending_viewport_rect = viewport_rect;
        m_submitted_frame_id++;
        m_command_ready.signal();
        return m_submitted_frame_id;
    }

    void mark_frame_complete(u64 frame_id)
    {
        Threading::MutexLocker const locker { m_mutex };
        m_completed_frame_id = frame_id;
        m_frame_completed.broadcast();
    }

    void wait_for_frame(u64 frame_id)
    {
        Threading::MutexLocker const locker { m_mutex };
        while (m_completed_frame_id < frame_id && !m_exit)
            m_frame_completed.wait();
    }

    void compositor_loop(DisplayListPlayerType display_list_player_type)
    {
        initialize_skia_player(display_list_player_type);

        while (true) {
            {
                Threading::MutexLocker const locker { m_mutex };
                while (m_command_queue.is_empty() && !m_needs_present && !m_exit) {
                    m_command_ready.wait();
                }
                if (m_exit)
                    break;
            }

            // Drain autoreleased Objective-C objects created by Metal/Skia each iteration,
            // since this background thread has no autorelease pool.
            Core::ScopedAutoreleasePool autorelease_pool;

            while (true) {
                auto command = [this]() -> Optional<CompositorCommand> {
                    Threading::MutexLocker const locker { m_mutex };
                    if (m_command_queue.is_empty())
                        return {};
                    return m_command_queue.dequeue();
                }();

                if (!command.has_value())
                    break;

                command->visit(
                    [this](UpdateDisplayListCommand& cmd) {
                        m_cached_display_list = move(cmd.display_list);
                        m_cached_scroll_state_snapshot = move(cmd.scroll_state_snapshot);
                    },
                    [this](UpdateBackingStoresCommand& cmd) {
                        allocate_backing_stores(cmd);
                        m_backing_stores.front_bitmap_id = cmd.front_bitmap_id;
                        m_backing_stores.back_bitmap_id = cmd.back_bitmap_id;
                    },
                    [this](ScreenshotCommand& cmd) {
                        if (!m_cached_display_list)
                            return;
                        m_skia_player->execute(*m_cached_display_list, m_cached_scroll_state_snapshot, *cmd.target_surface);
                        if (cmd.callback) {
                            invoke_on_main_thread([callback = move(cmd.callback)]() mutable {
                                callback();
                            });
                        }
                    });

                if (m_exit)
                    break;
            }

            if (m_exit)
                break;

            bool should_present = false;
            Gfx::IntRect viewport_rect;
            u64 presenting_frame_id = 0;
            {
                Threading::MutexLocker const locker { m_mutex };
                if (m_needs_present) {
                    should_present = true;
                    viewport_rect = m_pending_viewport_rect;
                    presenting_frame_id = m_submitted_frame_id;
                    m_needs_present = false;
                }
            }

            if (should_present) {
                // Block if we already have a frame queued (back pressure)
                {
                    Threading::MutexLocker const locker { m_mutex };
                    while (m_queued_rasterization_tasks > 1 && !m_exit) {
                        m_ready_to_paint.wait();
                    }
                    if (m_exit)
                        break;
                }

                auto presentation_mode = [this] {
                    Threading::MutexLocker const locker { m_mutex };
                    return m_presentation_mode;
                }();

                if (m_cached_display_list && m_backing_stores.is_valid()) {
                    auto should_clear_back_store = presentation_mode.visit(
                        [](RenderingThread::PresentToUI) { return false; },
                        [](RenderingThread::PublishToExternalContent const&) { return true; });
                    if (should_clear_back_store) {
                        // Embedded navigables leave their PaintConfig canvas unfilled, so double-buffered back stores
                        // must be cleared before repainting.
                        m_backing_stores.back_store->canvas().clear(SK_ColorTRANSPARENT);
                    }
                    m_skia_player->execute(*m_cached_display_list, m_cached_scroll_state_snapshot, *m_backing_stores.back_store);
                    i32 rendered_bitmap_id = m_backing_stores.back_bitmap_id;
                    m_backing_stores.swap();

                    presentation_mode.visit(
                        [this, viewport_rect, rendered_bitmap_id](RenderingThread::PresentToUI) {
                            m_queued_rasterization_tasks++;
                            invoke_on_main_thread([this, viewport_rect, rendered_bitmap_id]() {
                                m_presentation_callback(viewport_rect, rendered_bitmap_id);
                            });
                        },
                        [this](RenderingThread::PublishToExternalContent const& mode) {
                            auto snapshot = Gfx::ImmutableBitmap::create_snapshot_from_painting_surface(*m_backing_stores.front_store);
                            mode.source->update(move(snapshot));
                        });
                }
                mark_frame_complete(presenting_frame_id);
            }
        }
    }

private:
    void initialize_skia_player(DisplayListPlayerType display_list_player_type)
    {
        switch (display_list_player_type) {
        case DisplayListPlayerType::SkiaGPUIfAvailable:
            m_skia_backend_context = Gfx::SkiaBackendContext::create_independent_gpu_backend();
            break;
        case DisplayListPlayerType::SkiaCPU:
            break;
        }
        m_skia_player = make<Painting::DisplayListPlayerSkia>(m_skia_backend_context);
    }

    void publish_backing_store_pair(UpdateBackingStoresCommand& cmd, Gfx::SharedImage front_shared_image, Gfx::SharedImage back_shared_image)
    {
        if (!cmd.allocation_callback)
            return;
        invoke_on_main_thread([callback = move(cmd.allocation_callback), front_bitmap_id = cmd.front_bitmap_id, front_shared_image = move(front_shared_image), back_bitmap_id = cmd.back_bitmap_id, back_shared_image = move(back_shared_image)]() mutable {
            callback(front_bitmap_id, move(front_shared_image), back_bitmap_id, move(back_shared_image));
        });
    }

    void allocate_backing_stores(UpdateBackingStoresCommand& cmd)
    {
#ifdef USE_VULKAN_DMABUF_IMAGES
        if (m_skia_backend_context && cmd.allocation_callback) {
            auto backing_stores = create_linear_dmabuf_backing_stores(cmd.size, *m_skia_backend_context);
            if (!backing_stores.is_error()) {
                auto backing_store_pair = backing_stores.release_value();
                m_backing_stores.front_store = move(backing_store_pair.front);
                m_backing_stores.back_store = move(backing_store_pair.back);
                publish_backing_store_pair(cmd, move(backing_store_pair.front_shared_image), move(backing_store_pair.back_shared_image));
                return;
            }
        }
#endif

        auto front_buffer = Gfx::SharedImageBuffer::create(cmd.size);
        auto back_buffer = Gfx::SharedImageBuffer::create(cmd.size);
        auto front_shared_image = front_buffer.export_shared_image();
        auto back_shared_image = back_buffer.export_shared_image();
        auto backing_store_pair = create_shareable_bitmap_backing_stores(cmd.size, front_buffer, back_buffer, m_skia_backend_context);
        m_backing_stores.front_store = move(backing_store_pair.front);
        m_backing_stores.back_store = move(backing_store_pair.back);
        publish_backing_store_pair(cmd, move(front_shared_image), move(back_shared_image));
    }

    template<typename Invokee>
    void invoke_on_main_thread(Invokee invokee)
    {
        if (m_exit)
            return;
        auto event_loop = m_main_thread_event_loop->take();
        if (!event_loop)
            return;
        event_loop->deferred_invoke([self = NonnullRefPtr(*this), invokee = move(invokee)]() mutable {
            invokee();
        });
    }

    NonnullRefPtr<Core::WeakEventLoopReference> m_main_thread_event_loop;
    RenderingThread::PresentationCallback m_presentation_callback;

    mutable Threading::Mutex m_mutex;
    mutable Threading::ConditionVariable m_command_ready { m_mutex };
    Atomic<bool> m_exit { false };

    Queue<CompositorCommand> m_command_queue;

    OwnPtr<Painting::DisplayListPlayerSkia> m_skia_player;
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    RefPtr<Painting::DisplayList> m_cached_display_list;
    Painting::ScrollStateSnapshot m_cached_scroll_state_snapshot;
    BackingStoreState m_backing_stores;
    RenderingThread::PresentationMode m_presentation_mode { RenderingThread::PresentToUI {} };

    Atomic<i32> m_queued_rasterization_tasks { 0 };
    mutable Threading::ConditionVariable m_ready_to_paint { m_mutex };

    bool m_needs_present { false };
    Gfx::IntRect m_pending_viewport_rect;

    u64 m_submitted_frame_id { 0 };
    u64 m_completed_frame_id { 0 };
    mutable Threading::ConditionVariable m_frame_completed { m_mutex };

public:
    void decrement_queued_tasks()
    {
        Threading::MutexLocker const locker { m_mutex };
        VERIFY(m_queued_rasterization_tasks >= 1 && m_queued_rasterization_tasks <= 2);
        m_queued_rasterization_tasks--;
        m_ready_to_paint.signal();
    }
};

RenderingThread::RenderingThread(PresentationCallback presentation_callback)
    : m_thread_data(adopt_ref(*new ThreadData(Core::EventLoop::current_weak(), move(presentation_callback))))
{
}

RenderingThread::~RenderingThread()
{
    m_thread_data->exit();
}

void RenderingThread::start(DisplayListPlayerType display_list_player_type)
{
    m_thread = Threading::Thread::construct("Renderer"sv, [thread_data = m_thread_data, display_list_player_type] {
        thread_data->compositor_loop(display_list_player_type);
        return static_cast<intptr_t>(0);
    });
    m_thread->start();
    m_thread->detach();
}

void RenderingThread::set_presentation_mode(PresentationMode mode)
{
    m_thread_data->set_presentation_mode(move(mode));
}

void RenderingThread::update_display_list(NonnullRefPtr<Painting::DisplayList> display_list, Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    m_thread_data->enqueue_command(UpdateDisplayListCommand { move(display_list), move(scroll_state_snapshot) });
}

void RenderingThread::update_backing_stores(Gfx::IntSize size, i32 front_id, i32 back_id, Function<void(i32, Gfx::SharedImage, i32, Gfx::SharedImage)>&& allocation_callback)
{
    m_thread_data->enqueue_command(UpdateBackingStoresCommand { size, front_id, back_id, move(allocation_callback) });
}

u64 RenderingThread::present_frame(Gfx::IntRect viewport_rect)
{
    return m_thread_data->set_needs_present(viewport_rect);
}

void RenderingThread::wait_for_frame(u64 frame_id)
{
    m_thread_data->wait_for_frame(frame_id);
}

void RenderingThread::request_screenshot(NonnullRefPtr<Gfx::PaintingSurface> target_surface, Function<void()>&& callback)
{
    m_thread_data->enqueue_command(ScreenshotCommand { move(target_surface), move(callback) });
}

void RenderingThread::ready_to_paint()
{
    m_thread_data->decrement_queued_tasks();
}

}
