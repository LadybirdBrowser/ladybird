/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#include <LibMedia/VideoFrame.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Compositor/CompositorClient.h>
#include <LibWeb/Compositor/CompositorThread.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <WebContent/CompositorClientEndpoint.h>
#include <WebContent/CompositorConnection.h>
#include <WebContent/CompositorServerEndpoint.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/WebContentCompositorClientEndpoint.h>
#include <WebContent/WebContentCompositorHost.h>
#include <WebContent/WebContentCompositorServerEndpoint.h>

namespace WebContent {

static bool& should_use_compositor_process()
{
    static bool flag = false;
    return flag;
}

void set_should_use_compositor_process(bool enabled)
{
    should_use_compositor_process() = enabled;
}

class WebContentCompositorActor;

class WebContentCompositorConnectionToServer final
    : public IPC::ConnectionToServer<WebContentCompositorClientEndpoint, WebContentCompositorServerEndpoint>
    , public WebContentCompositorClientEndpoint {
    C_OBJECT(WebContentCompositorConnectionToServer)

public:
    WebContentCompositorConnectionToServer(NonnullOwnPtr<IPC::Transport> transport)
        : IPC::ConnectionToServer<WebContentCompositorClientEndpoint, WebContentCompositorServerEndpoint>(*this, move(transport))
    {
    }

    Web::Compositor::ScreenshotRequestId register_screenshot(NonnullRefPtr<Gfx::PaintingSurface> target_surface, NonnullRefPtr<Gfx::Bitmap> target_bitmap, Function<void()>&& callback)
    {
        auto request_id = Web::Compositor::ScreenshotRequestId { m_next_screenshot_request_id++ };
        m_screenshots.set(request_id, PendingScreenshot { move(target_surface), move(target_bitmap), move(callback) });
        return request_id;
    }

private:
    struct PendingScreenshot {
        NonnullRefPtr<Gfx::PaintingSurface> target_surface;
        NonnullRefPtr<Gfx::Bitmap> target_bitmap;
        Function<void()> callback;
    };

    virtual void die() override
    {
        did_lose_compositor();
    }

    virtual void request_rendering_update() override
    {
        Web::HTML::main_thread_event_loop().queue_task_to_update_the_rendering();
    }

    virtual void did_complete_screenshot(Web::Compositor::ScreenshotRequestId request_id) override
    {
        auto pending_screenshot = take_screenshot(request_id);
        if (!pending_screenshot.has_value())
            return;

        pending_screenshot->target_surface->write_from_bitmap(*pending_screenshot->target_bitmap);
        if (pending_screenshot->callback)
            pending_screenshot->callback();
    }

    virtual void did_fail_screenshot(Web::Compositor::ScreenshotRequestId request_id) override
    {
        auto pending_screenshot = take_screenshot(request_id);
        if (!pending_screenshot.has_value())
            return;

        if (pending_screenshot->callback)
            pending_screenshot->callback();
    }

    virtual void did_lose_compositor() override
    {
        for (auto& entry : m_screenshots) {
            if (entry.value.callback)
                entry.value.callback();
        }
        m_screenshots.clear();
    }

    Optional<PendingScreenshot> take_screenshot(Web::Compositor::ScreenshotRequestId request_id)
    {
        return m_screenshots.take(request_id);
    }

    HashMap<Web::Compositor::ScreenshotRequestId, PendingScreenshot> m_screenshots;
    u64 m_next_screenshot_request_id { 1 };
};

template<typename Self, typename Invokee>
static void invoke_on_event_loop(NonnullRefPtr<Core::WeakEventLoopReference> const& weak_loop, NonnullRefPtr<Self> self, Invokee invokee)
{
    auto event_loop = weak_loop->take();
    if (!event_loop)
        return;
    event_loop->deferred_invoke([self = move(self), invokee = move(invokee)]() mutable {
        invokee();
    });
}

class ActorMainThreadClient final : public Web::Compositor::CompositorMainThreadClient {
public:
    ActorMainThreadClient(NonnullRefPtr<Core::WeakEventLoopReference> event_loop, WeakPtr<WebContentCompositorActor> actor)
        : m_event_loop(move(event_loop))
        , m_actor(move(actor))
    {
    }

    virtual void request_rendering_update() override;
    virtual void did_complete_screenshot(Web::Compositor::ScreenshotRequestId) override;
    virtual void did_fail_screenshot(Web::Compositor::ScreenshotRequestId) override;
    virtual void did_lose_compositor() override;

private:
    template<typename Invokee>
    void dispatch_to_actor(Invokee invokee)
    {
        invoke_on_event_loop(m_event_loop, NonnullRefPtr(*this), [this, invokee = move(invokee)]() mutable {
            if (auto actor = m_actor.strong_ref())
                invokee(*actor);
        });
    }

    NonnullRefPtr<Core::WeakEventLoopReference> m_event_loop;
    WeakPtr<WebContentCompositorActor> m_actor;
};

class CompositorConnectionFromClient final
    : public IPC::ConnectionFromClient<CompositorClientEndpoint, CompositorServerEndpoint> {
    C_OBJECT(CompositorConnectionFromClient)

public:
    CompositorConnectionFromClient(WebContentCompositorActor& actor, NonnullOwnPtr<IPC::Transport> transport)
        : IPC::ConnectionFromClient<CompositorClientEndpoint, CompositorServerEndpoint>(*this, move(transport), 1)
        , m_actor(actor)
    {
    }

    virtual void die() override;

private:
    virtual Messages::CompositorServer::AsyncScrollByResponse async_scroll_by(u64 page_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels) override;
    virtual Messages::CompositorServer::MouseEventResponse mouse_event(u64 page_id, Web::MouseEvent event) override;
    virtual void ready_to_paint(u64 page_id, i32 bitmap_id) override;

    WebContentCompositorActor& m_actor;
};

class ActorUIPresentationClient final : public Web::Compositor::CompositorUIPresentationClient {
public:
    ActorUIPresentationClient(NonnullRefPtr<Core::WeakEventLoopReference> event_loop, NonnullRefPtr<CompositorConnectionFromClient> connection)
        : m_event_loop(move(event_loop))
        , m_connection(move(connection))
    {
    }

    virtual void publish_backing_stores(u64 page_id, i32 front_bitmap_id, Gfx::SharedImage&& front_backing_store, i32 back_bitmap_id, Gfx::SharedImage&& back_backing_store) override
    {
        invoke_on_event_loop(m_event_loop, NonnullRefPtr(*this), [this, page_id, front_bitmap_id, front_backing_store = move(front_backing_store), back_bitmap_id, back_backing_store = move(back_backing_store)]() mutable {
            m_connection->async_did_allocate_backing_stores(page_id, front_bitmap_id, move(front_backing_store), back_bitmap_id, move(back_backing_store));
        });
    }

    virtual void present_frame_to_ui(u64 page_id, Gfx::IntRect const& viewport_rect, i32 bitmap_id) override
    {
        invoke_on_event_loop(m_event_loop, NonnullRefPtr(*this), [this, page_id, viewport_rect, bitmap_id] {
            m_connection->async_did_paint(page_id, viewport_rect, bitmap_id);
        });
    }

private:
    NonnullRefPtr<Core::WeakEventLoopReference> m_event_loop;
    NonnullRefPtr<CompositorConnectionFromClient> m_connection;
};

class WebContentCompositorActor final
    : public IPC::ConnectionFromClient<WebContentCompositorClientEndpoint, WebContentCompositorServerEndpoint> {
    C_OBJECT(WebContentCompositorActor)

public:
    WebContentCompositorActor(NonnullOwnPtr<IPC::Transport> transport, Web::DisplayListPlayerType display_list_player_type)
        : IPC::ConnectionFromClient<WebContentCompositorClientEndpoint, WebContentCompositorServerEndpoint>(*this, move(transport), 1)
        , m_main_thread_client(adopt_ref(*new ActorMainThreadClient(Core::EventLoop::current_weak(), make_weak_ptr<WebContentCompositorActor>())))
        , m_compositor_thread(make<Web::Compositor::CompositorThread>(m_main_thread_client))
    {
        m_compositor_thread->start(display_list_player_type);
    }

    virtual void die() override
    {
        _exit(0);
    }

    bool async_scroll_by_from_ui(u64 page_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels)
    {
        return m_compositor_thread->async_scroll_by(page_id, position, delta_in_device_pixels);
    }

    bool handle_mouse_event_from_ui(u64 page_id, Web::MouseEvent const& event)
    {
        return m_compositor_thread->handle_mouse_event(page_id, event);
    }

    void presented_bitmap_ready_to_paint(u64 page_id, i32 bitmap_id)
    {
        m_compositor_thread->presented_bitmap_ready_to_paint(page_id, bitmap_id);
    }

    void detach_ui_client(CompositorConnectionFromClient& connection)
    {
        if (m_ui_connection.ptr() != &connection)
            return;
        m_compositor_thread->clear_ui_presentation_client();
        m_ui_connection = nullptr;
    }

private:
    virtual void create_context(Web::Compositor::CompositorContextId context_id, Optional<u64> page_id, Web::Compositor::PagePresentationRegistration page_presentation_registration) override
    {
        m_compositor_thread->register_context(context_id, page_id, page_presentation_registration);
    }

    virtual void destroy_context(Web::Compositor::CompositorContextId context_id) override
    {
        m_compositor_thread->destroy_context(context_id);
    }

    virtual void set_presentation_mode(Web::Compositor::CompositorContextId context_id, Web::Compositor::PresentationMode presentation_mode) override
    {
        m_compositor_thread->set_presentation_mode(context_id, move(presentation_mode));
    }

    virtual void stop_presenting_to_client(Web::Compositor::CompositorContextId context_id) override
    {
        m_compositor_thread->stop_presenting_to_client(context_id);
    }

    virtual void update_display_list(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Web::Painting::DisplayList> display_list, Web::Painting::DisplayListResourceTransaction resource_transaction, Web::Painting::ScrollStateSnapshot scroll_state_snapshot) override
    {
        m_compositor_thread->update_display_list(context_id, move(display_list), move(resource_transaction), move(scroll_state_snapshot));
    }

    virtual void update_scroll_state(Web::Compositor::CompositorContextId context_id, Web::Painting::ScrollStateSnapshot scroll_state_snapshot) override
    {
        m_compositor_thread->update_scroll_state(context_id, move(scroll_state_snapshot));
    }

    virtual void update_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id, NonnullRefPtr<Media::VideoFrame const> frame) override
    {
        m_compositor_thread->update_video_frame(context_id, frame_id, move(frame));
    }

    virtual void clear_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id) override
    {
        m_compositor_thread->clear_video_frame(context_id, frame_id);
    }

    virtual void update_compositor_surface(Web::Compositor::CompositorContextId context_id, Web::Painting::CompositorSurfaceId surface_id, Gfx::SharedImage shared_image) override
    {
        m_compositor_thread->update_compositor_surface(context_id, surface_id, move(shared_image));
    }

    virtual void clear_compositor_surface(Web::Compositor::CompositorContextId context_id, Web::Painting::CompositorSurfaceId surface_id) override
    {
        m_compositor_thread->clear_compositor_surface(context_id, surface_id);
    }

    virtual void invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId context_id, u64 generation) override
    {
        m_compositor_thread->invalidate_wheel_event_listener_state(context_id, generation);
    }

    virtual Messages::WebContentCompositorServer::AsyncScrollByResponse async_scroll_by(Web::Compositor::CompositorContextId context_id, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking operation_tracking) override
    {
        return m_compositor_thread->async_scroll_by(context_id, document_id, position, delta, viewport_rect, operation_tracking);
    }

    virtual Messages::WebContentCompositorServer::ShouldDeferAsyncScrollOffsetAdoptionResponse should_defer_async_scroll_offset_adoption(Web::Compositor::CompositorContextId context_id) override
    {
        return m_compositor_thread->should_defer_async_scroll_offset_adoption(context_id);
    }

    virtual Messages::WebContentCompositorServer::ShouldDeferMainThreadPresentForAsyncScrollResponse should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId context_id) override
    {
        return m_compositor_thread->should_defer_main_thread_present_for_async_scroll(context_id);
    }

    virtual Messages::WebContentCompositorServer::TakePendingAsyncScrollUpdatesResponse take_pending_async_scroll_updates(Web::Compositor::CompositorContextId context_id) override
    {
        return m_compositor_thread->take_pending_async_scroll_updates(context_id);
    }

    virtual void viewport_size_updated(Web::Compositor::CompositorContextId context_id, Gfx::IntSize viewport_size, bool is_top_level, Web::Compositor::WindowResizingInProgress window_resize_in_progress) override
    {
        m_compositor_thread->viewport_size_updated(context_id, viewport_size, is_top_level, window_resize_in_progress);
    }

    virtual void present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect viewport_rect) override
    {
        m_compositor_thread->present_frame(context_id, viewport_rect);
    }

    virtual void request_screenshot(Web::Compositor::CompositorContextId context_id, Web::Compositor::ScreenshotRequestId request_id, Gfx::ShareableBitmap target_bitmap) override
    {
        if (!target_bitmap.is_valid() || !target_bitmap.bitmap()) {
            async_did_fail_screenshot(request_id);
            return;
        }
        auto target_surface = Gfx::PaintingSurface::wrap_bitmap(*target_bitmap.bitmap());
        m_compositor_thread->request_screenshot(context_id, target_surface, request_id);
    }

    virtual void attach_ui_client(IPC::TransportHandle handle) override
    {
        auto transport = MUST(handle.create_transport());
        auto connection = CompositorConnectionFromClient::construct(*this, move(transport));
        m_ui_connection = connection;
        m_compositor_thread->set_ui_presentation_client(adopt_ref(*new ActorUIPresentationClient(Core::EventLoop::current_weak(), connection)));
    }

    NonnullRefPtr<ActorMainThreadClient> m_main_thread_client;
    OwnPtr<Web::Compositor::CompositorThread> m_compositor_thread;
    RefPtr<CompositorConnectionFromClient> m_ui_connection;
};

void ActorMainThreadClient::request_rendering_update()
{
    dispatch_to_actor([](auto& actor) {
        actor.async_request_rendering_update();
    });
}

void ActorMainThreadClient::did_complete_screenshot(Web::Compositor::ScreenshotRequestId request_id)
{
    dispatch_to_actor([request_id](auto& actor) {
        actor.async_did_complete_screenshot(request_id);
    });
}

void ActorMainThreadClient::did_fail_screenshot(Web::Compositor::ScreenshotRequestId request_id)
{
    dispatch_to_actor([request_id](auto& actor) {
        actor.async_did_fail_screenshot(request_id);
    });
}

void ActorMainThreadClient::did_lose_compositor()
{
    dispatch_to_actor([](auto& actor) {
        actor.async_did_lose_compositor();
    });
}

void CompositorConnectionFromClient::die()
{
    NonnullRefPtr protect { *this };
    m_actor.detach_ui_client(*this);
}

Messages::CompositorServer::AsyncScrollByResponse CompositorConnectionFromClient::async_scroll_by(u64 page_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels)
{
    return m_actor.async_scroll_by_from_ui(page_id, position, delta_in_device_pixels);
}

Messages::CompositorServer::MouseEventResponse CompositorConnectionFromClient::mouse_event(u64 page_id, Web::MouseEvent event)
{
    return m_actor.handle_mouse_event_from_ui(page_id, event);
}

void CompositorConnectionFromClient::ready_to_paint(u64 page_id, i32 bitmap_id)
{
    m_actor.presented_bitmap_ready_to_paint(page_id, bitmap_id);
}

class LocalWebContentCompositorHost final : public Web::Compositor::CompositorHost {
public:
    virtual void start(Web::DisplayListPlayerType display_list_player_type) override
    {
        if (m_connection)
            return;

        auto paired_transport = MUST(IPC::Transport::create_paired());

        m_connection = WebContentCompositorConnectionToServer::construct(move(paired_transport.local));
        m_actor_thread = Threading::Thread::construct("WebContentCompositor"sv, [handle = move(paired_transport.remote_handle), display_list_player_type] mutable {
            Core::EventLoop event_loop;
            auto transport = MUST(handle.create_transport());
            auto actor = WebContentCompositorActor::construct(move(transport), display_list_player_type);
            return event_loop.exec();
        });
        m_actor_thread->start();
        m_actor_thread->detach();
    }

    virtual void attach_ui_client(IPC::TransportHandle handle) override
    {
        connection().async_attach_ui_client(move(handle));
    }

private:
    virtual void register_context(Web::Compositor::CompositorContextId context_id, Optional<u64> page_id, Web::Compositor::PagePresentationRegistration page_presentation_registration) override
    {
        connection().create_context(context_id, page_id, page_presentation_registration);
    }

    virtual void destroy_context(Web::Compositor::CompositorContextId context_id) override
    {
        connection().async_destroy_context(context_id);
    }

    virtual void stop_presenting_to_client(Web::Compositor::CompositorContextId context_id) override
    {
        connection().async_stop_presenting_to_client(context_id);
    }

    virtual void set_presentation_mode(Web::Compositor::CompositorContextId context_id, Web::Compositor::PresentationMode mode) override
    {
        connection().async_set_presentation_mode(context_id, move(mode));
    }

    virtual void update_display_list(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Web::Painting::DisplayList> display_list, Web::Painting::DisplayListResourceTransaction&& resource_transaction, Web::Painting::ScrollStateSnapshot&& scroll_state_snapshot) override
    {
        auto encoded_message = MUST(Messages::WebContentCompositorServer::UpdateDisplayList::static_encode(context_id, move(display_list), move(resource_transaction), move(scroll_state_snapshot)));
        MUST(connection().post_message(encoded_message));
    }

    virtual void update_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id, NonnullRefPtr<Media::VideoFrame const> frame) override
    {
        auto encoded_message = MUST(Messages::WebContentCompositorServer::UpdateVideoFrame::static_encode(context_id, frame_id, move(frame)));
        MUST(connection().post_message(encoded_message));
    }

    virtual void clear_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id) override
    {
        connection().async_clear_video_frame(context_id, frame_id);
    }

    virtual void update_compositor_surface(Web::Compositor::CompositorContextId context_id, Web::Painting::CompositorSurfaceId surface_id, Gfx::SharedImage&& shared_image) override
    {
        connection().async_update_compositor_surface(context_id, surface_id, move(shared_image));
    }

    virtual void clear_compositor_surface(Web::Compositor::CompositorContextId context_id, Web::Painting::CompositorSurfaceId surface_id) override
    {
        connection().async_clear_compositor_surface(context_id, surface_id);
    }

    virtual void update_scroll_state(Web::Compositor::CompositorContextId context_id, Web::Painting::ScrollStateSnapshot&& scroll_state_snapshot) override
    {
        connection().async_update_scroll_state(context_id, move(scroll_state_snapshot));
    }

    virtual void invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId context_id, u64 generation) override
    {
        connection().async_invalidate_wheel_event_listener_state(context_id, generation);
    }

    virtual Web::Compositor::AsyncScrollEnqueueResult async_scroll_by(Web::Compositor::CompositorContextId context_id, Web::UniqueNodeID expected_document_id, Gfx::FloatPoint position,
        Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking operation_tracking) override
    {
        auto response = connection().send_sync<Messages::WebContentCompositorServer::AsyncScrollBy>(context_id, expected_document_id, position, delta_in_device_pixels, viewport_rect, operation_tracking);
        return response->take_result();
    }

    virtual bool should_defer_async_scroll_offset_adoption(Web::Compositor::CompositorContextId context_id) const override
    {
        auto response = connection().send_sync<Messages::WebContentCompositorServer::ShouldDeferAsyncScrollOffsetAdoption>(context_id);
        return response->should_defer();
    }

    virtual bool should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId context_id) const override
    {
        auto response = connection().send_sync<Messages::WebContentCompositorServer::ShouldDeferMainThreadPresentForAsyncScroll>(context_id);
        return response->should_defer();
    }

    virtual Web::Compositor::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::Compositor::CompositorContextId context_id) override
    {
        auto response = connection().send_sync<Messages::WebContentCompositorServer::TakePendingAsyncScrollUpdates>(context_id);
        return response->take_updates();
    }

    virtual void viewport_size_updated(Web::Compositor::CompositorContextId context_id, Gfx::IntSize viewport_size, bool is_top_level_traversable, Web::Compositor::WindowResizingInProgress window_resize_in_progress) override
    {
        connection().async_viewport_size_updated(context_id, viewport_size, is_top_level_traversable, window_resize_in_progress);
    }

    virtual void present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect viewport_rect) override
    {
        connection().async_present_frame(context_id, viewport_rect);
    }

    virtual void request_screenshot(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Gfx::PaintingSurface> target_surface, Function<void()>&& callback) override
    {
        auto& conn = connection();
        auto target_bitmap = MUST(Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, target_surface->size()));
        auto shareable_bitmap = Gfx::ShareableBitmap { target_bitmap, Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
        auto request_id = conn.register_screenshot(target_surface, target_bitmap, move(callback));
        conn.async_request_screenshot(context_id, request_id, move(shareable_bitmap));
    }

    WebContentCompositorConnectionToServer& connection() const
    {
        VERIFY(m_connection);
        return *m_connection;
    }

    RefPtr<WebContentCompositorConnectionToServer> m_connection;
    RefPtr<Threading::Thread> m_actor_thread;
};

class ProcessWebContentCompositorHost final : public Web::Compositor::CompositorHost {
public:
    explicit ProcessWebContentCompositorHost(ConnectionFromClient& client)
        : m_client(client)
    {
    }

    virtual void start(Web::DisplayListPlayerType) override
    {
    }

private:
    virtual void register_context(Web::Compositor::CompositorContextId context_id, Optional<u64> page_id, Web::Compositor::PagePresentationRegistration page_presentation_registration) override
    {
        if (page_presentation_registration == Web::Compositor::PagePresentationRegistration::Yes) {
            VERIFY(page_id.has_value());
        } else {
            VERIFY(!page_id.has_value());
            VERIFY(!Web::Compositor::is_page_presenting_compositor_context_id(context_id));
        }
    }

    virtual void destroy_context(Web::Compositor::CompositorContextId context_id) override
    {
        if (auto* connection = compositor_connection())
            connection->destroy_context(context_id);
        m_client.did_destroy_compositor_context(context_id);
    }

    virtual void stop_presenting_to_client(Web::Compositor::CompositorContextId context_id) override
    {
        if (auto* connection = compositor_connection())
            connection->stop_presenting_to_client(context_id);
    }

    virtual void set_presentation_mode(Web::Compositor::CompositorContextId context_id, Web::Compositor::PresentationMode mode) override
    {
        if (auto* connection = compositor_connection())
            connection->set_presentation_mode(context_id, mode);
    }

    virtual void update_display_list(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Web::Painting::DisplayList> display_list, Web::Painting::DisplayListResourceTransaction&& resource_transaction, Web::Painting::ScrollStateSnapshot&& scroll_state_snapshot) override
    {
        if (auto* connection = compositor_connection())
            connection->update_display_list(context_id, display_list, resource_transaction, scroll_state_snapshot);
    }

    virtual void update_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id, NonnullRefPtr<Media::VideoFrame const> frame) override
    {
        if (auto* connection = compositor_connection())
            connection->update_video_frame(context_id, frame_id, frame);
    }

    virtual void clear_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id) override
    {
        if (auto* connection = compositor_connection())
            connection->clear_video_frame(context_id, frame_id);
    }

    virtual void update_compositor_surface(Web::Compositor::CompositorContextId context_id, Web::Painting::CompositorSurfaceId surface_id, Gfx::SharedImage&& shared_image) override
    {
        if (auto* connection = compositor_connection())
            connection->update_compositor_surface(context_id, surface_id, shared_image);
    }

    virtual void clear_compositor_surface(Web::Compositor::CompositorContextId context_id, Web::Painting::CompositorSurfaceId surface_id) override
    {
        if (auto* connection = compositor_connection())
            connection->clear_compositor_surface(context_id, surface_id);
    }

    virtual void update_scroll_state(Web::Compositor::CompositorContextId context_id, Web::Painting::ScrollStateSnapshot&& scroll_state_snapshot) override
    {
        if (auto* connection = compositor_connection())
            connection->update_scroll_state(context_id, scroll_state_snapshot);
    }

    virtual void invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId context_id, u64 generation) override
    {
        if (auto* connection = compositor_connection())
            connection->invalidate_wheel_event_listener_state(context_id, generation);
    }

    virtual Web::Compositor::AsyncScrollEnqueueResult async_scroll_by(Web::Compositor::CompositorContextId context_id, Web::UniqueNodeID expected_document_id, Gfx::FloatPoint position,
        Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking operation_tracking) override
    {
        if (auto* connection = compositor_connection())
            return connection->async_scroll_by(context_id, expected_document_id, position, delta_in_device_pixels, viewport_rect, operation_tracking);
        return {};
    }

    virtual bool should_defer_async_scroll_offset_adoption(Web::Compositor::CompositorContextId) const override
    {
        return false;
    }

    virtual bool should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId context_id) const override
    {
        if (auto* connection = compositor_connection())
            return connection->should_defer_main_thread_present_for_async_scroll(context_id);
        return false;
    }

    virtual Web::Compositor::PendingAsyncScrollUpdates take_pending_async_scroll_updates(Web::Compositor::CompositorContextId context_id) override
    {
        if (auto* connection = compositor_connection())
            return connection->take_pending_async_scroll_updates(context_id);
        return {};
    }

    virtual void viewport_size_updated(Web::Compositor::CompositorContextId context_id, Gfx::IntSize viewport_size, bool is_top_level_traversable, Web::Compositor::WindowResizingInProgress window_resize_in_progress) override
    {
        if (auto* connection = compositor_connection())
            connection->viewport_size_updated(context_id, viewport_size, is_top_level_traversable, window_resize_in_progress);
    }

    virtual void present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect viewport_rect) override
    {
        if (auto* connection = compositor_connection())
            connection->present_frame(context_id, viewport_rect);
    }

    virtual void request_screenshot(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Gfx::PaintingSurface> target_surface, Function<void()>&& callback) override
    {
        if (auto* connection = compositor_connection()) {
            connection->request_screenshot(context_id, move(target_surface), move(callback));
            return;
        }
        if (callback)
            callback();
    }

    CompositorConnection* compositor_connection() const
    {
        return m_client.compositor_process_connection();
    }

    ConnectionFromClient& m_client;
};

NonnullOwnPtr<Web::Compositor::CompositorHost> create_web_content_compositor_host(ConnectionFromClient& client)
{
    if (should_use_compositor_process())
        return make<ProcessWebContentCompositorHost>(client);
    return make<LocalWebContentCompositorHost>();
}

}
