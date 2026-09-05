/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObjectSerializer.h>
#include <AK/JsonValue.h>
#include <AK/Math.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16StringBuilder.h>
#include <LibCore/Process.h>
#include <LibCore/Timer.h>
#include <LibDevTools/IndexedDBSerialization.h>
#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibIPC/TransportHandle.h>
#include <LibJS/Console.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/MutationType.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Geolocation/GeolocationCoordinates.h>
#include <LibWeb/Geolocation/GeolocationPositionError.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/NavigationPopulationRequest.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Streams/ReadableStreamDefaultReader.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebView/Debugger.h>
#include <LibWebView/ViewImplementation.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/DevToolsConsoleClient.h>
#include <WebContent/PageClient.h>
#include <WebContent/PageHost.h>
#include <WebContent/WebContentClientEndpoint.h>
#include <WebContent/WebDriverConnection.h>
#include <WebContent/WebUIConnection.h>

namespace WebContent {

static bool s_is_headless { false };
static bool s_async_scrolling_enabled { false };
static constexpr size_t s_max_download_data_ipc_chunk_size = 16 * MiB;

GC_DEFINE_ALLOCATOR(PageClient);

static Web::Geolocation::GeolocationPositionError::ErrorCode geolocation_position_error_code_from_ipc(u16 error_code)
{
    using ErrorCode = Web::Geolocation::GeolocationPositionError::ErrorCode;

    switch (error_code) {
    case to_underlying(ErrorCode::PermissionDenied):
        return ErrorCode::PermissionDenied;
    case to_underlying(ErrorCode::PositionUnavailable):
        return ErrorCode::PositionUnavailable;
    case to_underlying(ErrorCode::Timeout):
        return ErrorCode::Timeout;
    default:
        return ErrorCode::PositionUnavailable;
    }
}

static Optional<Web::Geolocation::CoordinatesData> geolocation_coordinates_from_ipc(WebView::GeolocationPositionData const& position)
{
    if (!position.latitude.has_value() || !position.longitude.has_value() || !position.accuracy.has_value())
        return {};

    return Web::Geolocation::CoordinatesData {
        .accuracy = *position.accuracy,
        .latitude = *position.latitude,
        .longitude = *position.longitude,
        .altitude = position.altitude,
        .altitude_accuracy = position.altitude_accuracy,
        .heading = position.heading,
        .speed = position.speed,
    };
}

static String serialize_dom_mutation_target(Web::DOM::Node const& target)
{
    Utf16StringBuilder builder;
    auto serializer = MUST(JsonObjectSerializer<>::try_create(builder));
    target.serialize_tree_as_json(serializer);
    MUST(serializer.finish());
    auto json = builder.to_string();
    return MUST(json.utf16_view().to_utf8());
}

bool PageClient::is_headless() const
{
    return s_is_headless;
}

void PageClient::set_is_headless(bool is_headless)
{
    s_is_headless = is_headless;
}

void PageClient::set_async_scrolling_enabled(bool enabled)
{
    s_async_scrolling_enabled = enabled;
}

GC::Ref<PageClient> PageClient::create(PageHost& page_host, u64 id, Optional<Web::HTML::CrossProcessId> pending_root_navigable_id)
{
    return GC::Heap::the().allocate<PageClient>(page_host, id, pending_root_navigable_id);
}

PageClient::PageClient(PageHost& owner, u64 id, Optional<Web::HTML::CrossProcessId> pending_root_navigable_id)
    : m_owner(owner)
    , m_page(Web::Page::create(*this))
    , m_id(id)
    , m_pending_root_navigable_id(pending_root_navigable_id)
{
    m_page->set_async_scrolling_enabled(s_async_scrolling_enabled);
    setup_palette();

    m_frame_timer = Core::Timer::create_single_shot(0, [this] { frame_timer_fired(); });
}

PageClient::~PageClient() = default;

void PageClient::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
    visitor.visit(m_top_level_document_console_client);
    for (auto& promise : m_pending_delete_all_cookies_promises)
        visitor.visit(promise.value);
    for (auto& controller : m_download_controllers)
        visitor.visit(controller.value);
    for (auto& reader : m_download_readers)
        visitor.visit(reader.value);
    m_pending_dom_mutations.for_each([&](auto& pending_mutation) {
        visitor.visit(pending_mutation.target);
    });

    if (m_webdriver)
        m_webdriver->visit_edges(visitor);
    if (m_web_ui)
        m_web_ui->visit_edges(visitor);
}

ConnectionFromClient& PageClient::client() const
{
    return m_owner.client();
}

void PageClient::set_has_focus(bool has_focus)
{
    if (m_has_focus == has_focus)
        return;

    m_has_focus = has_focus;

    if (auto document = page().local_root_navigable()->active_document(); document && has_focus)
        document->reset_cursor_blink_cycle();

    // The focus ring, the text caret, and selection highlight colors all depend on the window focus state, so
    // nothing painted before the change can be reused; repaint every document in the traversable.
    Function<void(Web::HTML::LocalNavigable&)> invalidate_cached_paint_recursively = [&](Web::HTML::LocalNavigable& navigable) {
        if (auto navigable_document = navigable.active_document()) {
            // Focus changes can arrive while layout is invalidated. We only need to invalidate cached paint
            // on the current paintable tree here, without requiring layout to be up to date first.
            if (navigable_document->has_committed_viewport_box())
                navigable_document->paint_state().invalidate_all_cached_paint(*navigable_document);
        }
        for (auto& child_navigable : navigable.child_navigables())
            invalidate_cached_paint_recursively(*child_navigable);
    };
    invalidate_cached_paint_recursively(page().local_root_navigable());
}

void PageClient::set_window_handle(Utf16String window_handle)
{
    page().top_level_traversable()->set_window_handle(move(window_handle));
}

void PageClient::setup_palette()
{
    // FIXME: Get the proper palette from our peer somehow
    auto buffer_or_error = Core::AnonymousBuffer::create_with_size(sizeof(Gfx::SystemTheme));
    VERIFY(!buffer_or_error.is_error());
    auto buffer = buffer_or_error.release_value();
    auto* theme = buffer.data<Gfx::SystemTheme>();
    theme->color[to_underlying(Gfx::ColorRole::Window)] = Color(Color::Magenta).value();
    theme->color[to_underlying(Gfx::ColorRole::WindowText)] = Color(Color::Cyan).value();
    m_palette_impl = Gfx::PaletteImpl::create_with_anonymous_buffer(buffer);
}

bool PageClient::is_connection_open() const
{
    return client().is_open();
}

Web::HTML::CrossProcessId PageClient::allocate_cross_process_id()
{
    return m_owner.allocate_cross_process_id();
}

Web::HTML::CrossProcessId PageClient::allocate_navigable_id()
{
    if (m_pending_root_navigable_id.has_value()) {
        auto id = *m_pending_root_navigable_id;
        m_pending_root_navigable_id.clear();
        return id;
    }

    return allocate_cross_process_id();
}

void PageClient::request_navigation_start(Web::HTML::LocalNavigable& navigable, Web::NavigationTarget target, URL::URL const& url, Utf16String navigation_id, Optional<Web::HTML::NavigationStartRequest> start_request)
{
    client().async_did_request_navigation_start(m_id, navigable.id(), target, url, move(navigation_id), move(start_request));
}

void PageClient::request_navigation_population(Web::HTML::LocalNavigable& navigable, Web::NavigationTarget target, Web::HTML::NavigationPopulationRequest request)
{
    client().async_did_request_navigation_population(m_id, navigable.id(), target, move(request));
}

void PageClient::navigation_params_creation_finished(Web::HTML::LocalNavigable& navigable, Web::HTML::NavigationPopulationRequest request, Web::HTML::NavigationPopulationResult result)
{
    client().async_did_finish_navigation_params_creation(m_id, navigable.id(), request.navigation_id, move(result));
}

void PageClient::history_navigation_params_creation_finished(Web::HTML::CrossProcessId operation_id, Web::HTML::HistoryNavigationPopulation population)
{
    client().async_did_finish_history_navigation_params_creation(m_id, operation_id, move(population));
}

void PageClient::navigation_population_failed(Web::HTML::CrossProcessId navigable_id, Utf16String const& navigation_id)
{
    client().async_did_fail_navigation_population(m_id, navigable_id, navigation_id);
}

void PageClient::populate_navigation(Web::HTML::NavigationPopulationRequest request, Web::HTML::NavigationPopulationResult result)
{
    page().top_level_traversable()->continue_navigation_at_population(move(request), move(result));
}

void PageClient::create_navigation_params(Web::HTML::NavigationPopulationRequest request)
{
    auto navigable_id = request.navigable_id;
    auto navigation_id = request.navigation_id;
    auto active_document = page().top_level_traversable()->active_document();
    if (!active_document) {
        client().async_did_finish_navigation_params_creation(m_id, navigable_id, navigation_id, {});
        return;
    }

    for (auto const& navigable : active_document->inclusive_descendant_navigables()) {
        if (navigable->id() != navigable_id)
            continue;
        if (!navigable->resume_navigation_params_creation(navigation_id, move(request)))
            client().async_did_finish_navigation_params_creation(m_id, navigable_id, navigation_id, {});
        return;
    }

    client().async_did_finish_navigation_params_creation(m_id, navigable_id, navigation_id, {});
}

void PageClient::cancel_navigation_params_creation(Web::HTML::CrossProcessId navigable_id, Utf16String const& navigation_id)
{
    auto active_document = page().top_level_traversable()->active_document();
    if (!active_document)
        return;

    for (auto const& navigable : active_document->inclusive_descendant_navigables()) {
        if (navigable->id() != navigable_id)
            continue;
        navigable->resume_navigation_params_creation(navigation_id, {});
        return;
    }
}

void PageClient::run_navigation_unload_check(Web::HTML::CrossProcessId navigable_id, Utf16String const& navigation_id)
{
    auto active_document = page().top_level_traversable()->active_document();
    if (!active_document) {
        client().async_did_fail_navigation_population(m_id, navigable_id, navigation_id);
        return;
    }

    for (auto const& navigable : active_document->inclusive_descendant_navigables()) {
        if (navigable->id() != navigable_id)
            continue;
        navigable->run_navigation_unload_check(navigation_id, GC::create_function(navigable->heap(), [this, navigable = GC::Ref { *navigable }, navigable_id, navigation_id](bool should_continue) {
            // The UI process retained the pending entry at admission; a passed check only needs the signal.
            if (!should_continue) {
                navigable->resume_navigation_params_creation(navigation_id, {});
                client().async_did_fail_navigation_population(m_id, navigable_id, navigation_id);
                return;
            }
            client().async_did_complete_navigation_unload_check(m_id, navigable_id, navigation_id);
        }));
        return;
    }

    client().async_did_fail_navigation_population(m_id, navigable_id, navigation_id);
}

void PageClient::page_did_create_child_frame(Web::HTML::CrossProcessId parent_frame_id, Web::HTML::CrossProcessId frame_id, Web::HTML::ReplicatedNavigableState const& replicated_state)
{
    client().async_did_create_child_frame(m_id, parent_frame_id, frame_id, replicated_state);
}

void PageClient::page_did_update_child_frame_viewport(Web::HTML::CrossProcessId frame_id, Web::CSSPixelRect viewport_rect)
{
    client().async_did_update_child_frame_viewport(m_id, frame_id, page().css_to_device_rect(viewport_rect), page().client().device_pixel_ratio());
}

void PageClient::page_did_destroy_child_frame(Web::HTML::CrossProcessId frame_id)
{
    m_remote_child_frame_compositor_contexts.remove(frame_id);
    client().async_did_destroy_child_frame(m_id, frame_id);
}

void PageClient::set_remote_child_frame_compositor_context(Web::HTML::CrossProcessId frame_id, Optional<Web::Compositor::CompositorContextId> context_id)
{
    if (context_id.has_value())
        m_remote_child_frame_compositor_contexts.set(frame_id, *context_id);
    else
        m_remote_child_frame_compositor_contexts.remove(frame_id);
    request_frame();
}

Optional<Web::Compositor::CompositorContextId> PageClient::compositor_context_id_for_remote_child_frame(Web::HTML::CrossProcessId frame_id) const
{
    return m_remote_child_frame_compositor_contexts.get(frame_id);
}

void PageClient::run_iframe_load_event_steps(Web::HTML::CrossProcessId frame_id)
{
    auto active_document = page().top_level_traversable()->active_document();
    if (!active_document)
        return;

    for (auto const& navigable : active_document->inclusive_descendant_navigables()) {
        if (navigable->id() != frame_id)
            continue;

        auto container = GC::make_root(navigable->container());
        if (!container || !is<Web::HTML::HTMLIFrameElement>(*container))
            return;

        container->queue_an_element_task(Web::HTML::Task::Source::DOMManipulation, [container] {
            Web::HTML::run_iframe_load_event_steps(as<Web::HTML::HTMLIFrameElement>(*container));
        });
        container->document().schedule_html_parser_end_check();
        return;
    }
}

Gfx::Palette PageClient::palette() const
{
    return Gfx::Palette(*m_palette_impl);
}

void PageClient::set_palette_impl(Gfx::PaletteImpl& impl)
{
    m_palette_impl = impl;
    page().invalidate_style_for_preference_change();
    request_frame();
}

void PageClient::set_preferred_color_scheme(Web::CSS::PreferredColorScheme color_scheme)
{
    m_preferred_color_scheme = color_scheme;
    page().invalidate_style_for_preference_change();
}

void PageClient::set_preferred_contrast(Web::CSS::PreferredContrast contrast)
{
    m_preferred_contrast = contrast;
    page().invalidate_style_for_preference_change();
}

void PageClient::set_preferred_motion(Web::CSS::PreferredMotion motion)
{
    m_preferred_motion = motion;
    page().invalidate_style_for_preference_change();
}

void PageClient::set_is_scripting_enabled(bool is_scripting_enabled)
{
    page().set_is_scripting_enabled(is_scripting_enabled);
}

void PageClient::set_window_position(Web::DevicePixelPoint position)
{
    page().set_window_position(position);
}

void PageClient::set_window_size(Web::DevicePixelSize size)
{
    page().set_window_size(size);
}

void PageClient::compositor_process_lost()
{
    page().notify_all_webgl_contexts_lost();
    page().detach_all_media_element_video_sinks_after_compositor_lost();

    m_compositor_rendering_opportunity_outstanding = false;
    m_compositor_watchdog_deadline = 0;
    m_frame_timer->stop();
    m_frame_timer_purpose = FrameTimerPurpose::Inactive;
    request_rendering_opportunity_if_needed();
}

void PageClient::compositor_process_reconnected()
{
    // Drop canvas commands recorded for the previous Compositor process: the
    // new process allocates canvas ids from scratch, so flushing stale
    // segments could target the wrong canvas.
    if (auto* compositor_host = m_owner.compositor_host())
        compositor_host->discard_canvas_2d_stream();

    page().local_root_navigable()->repaint_after_compositor_process_reconnect();
    page().notify_all_canvas_elements_of_lost_backing_storage();
    page().prepare_canvas_contexts_for_compositing();
    page().restore_all_media_element_video_sinks();
    m_compositor_rendering_opportunity_outstanding = false;
    m_compositor_watchdog_deadline = 0;
    m_frame_timer->stop();
    m_frame_timer_purpose = FrameTimerPurpose::Inactive;
    request_frame();
}

Queue<Web::QueuedInputEvent>& PageClient::input_event_queue()
{
    return client().input_event_queue();
}

void PageClient::did_handle_input_event(u64 page_id, Web::InputEvent const& event)
{
    auto should_update_input_method_state = event.visit(
        [](Web::KeyEvent const&) {
            return true;
        },
        [](Web::MouseEvent const& mouse_event) {
            switch (mouse_event.type) {
            case Web::MouseEvent::Type::MouseDown:
            case Web::MouseEvent::Type::MouseUp:
                return true;
            case Web::MouseEvent::Type::MouseMove:
                return mouse_event.buttons != Web::UIEvents::MouseButton::None;
            case Web::MouseEvent::Type::MouseLeave:
            case Web::MouseEvent::Type::MouseWheel:
                return false;
            }
            VERIFY_NOT_REACHED();
        },
        [](auto const&) {
            return false;
        });

    if (should_update_input_method_state)
        client().update_input_method_state(page_id);
}

void PageClient::report_finished_handling_input_event(u64 page_id, Web::EventResult event_was_handled)
{
    client().async_did_finish_handling_input_event(page_id, event_was_handled);
}

Web::Compositor::CompositorContextId PageClient::allocate_compositor_context_id(Web::Compositor::PagePresentationRegistration page_presentation_registration)
{
    return client().allocate_compositor_context_id(m_id, page_presentation_registration);
}

void PageClient::set_viewport(Web::DevicePixelSize const& size, double device_pixel_ratio)
{
    auto invalidate = m_device_pixel_ratio != device_pixel_ratio
        ? Web::InvalidateDisplayList::Yes
        : Web::InvalidateDisplayList::No;

    m_viewport_size = size;
    m_device_pixel_ratio = device_pixel_ratio;

    page().local_root_navigable()->set_viewport_size(page().device_to_css_size(size), invalidate);
}

void PageClient::set_zoom_level(double zoom_level)
{
    m_zoom_level = zoom_level;
    page().local_root_navigable()->set_viewport_size(page().device_to_css_size(m_viewport_size), Web::InvalidateDisplayList::Yes);
}

void PageClient::request_frame()
{
    Web::HTML::main_thread_event_loop().request_rendering_update();

    if (m_rendering_opportunity_granted)
        return;

    if (m_rendering_update_requested) {
        request_rendering_opportunity_if_needed();
        return;
    }

    m_rendering_update_requested = true;
    request_rendering_opportunity_if_needed();
}

void PageClient::request_rendering_opportunity_if_needed()
{
    if (!m_rendering_update_requested)
        return;
    if (m_manual_rendering_opportunities)
        return;
    if (m_compositor_rendering_opportunity_outstanding || m_frame_timer->is_active())
        return;
    if (Web::HTML::main_thread_event_loop().rendering_task_queued_or_running())
        return;

    if (page().has_local_root_navigable()) {
        auto& local_root_navigable = *page().local_root_navigable();
        if (local_root_navigable.has_compositor_context() && local_root_navigable.compositor_context().request_rendering_opportunity(m_maximum_frames_per_second)) {
            m_compositor_rendering_opportunity_outstanding = true;
            schedule_compositor_watchdog();
            return;
        }
    }

    schedule_local_rendering_opportunity();
}

void PageClient::schedule_local_rendering_opportunity()
{
    VERIFY(!m_frame_timer->is_active());

    auto delay = 0.0;
    auto now = Web::HighResolutionTime::unsafe_shared_current_time();
    if (m_last_scheduled_frame_dispatch_time.has_value()) {
        auto minimum_frame_interval = 1000.0 / m_maximum_frames_per_second;
        auto next_frame_dispatch_time = *m_last_scheduled_frame_dispatch_time + minimum_frame_interval;
        m_last_scheduled_frame_dispatch_time = max(now, next_frame_dispatch_time);
        delay = *m_last_scheduled_frame_dispatch_time - now;
    } else {
        m_last_scheduled_frame_dispatch_time = now;
    }

    m_frame_timer_purpose = FrameTimerPurpose::LocalFallback;
    m_frame_timer->restart(static_cast<int>(AK::ceil(delay)));
}

void PageClient::schedule_compositor_watchdog()
{
    VERIFY(!m_frame_timer->is_active());
    auto delay = 4.0 * m_last_rendering_opportunity_frame_interval;
    m_compositor_watchdog_deadline = Web::HighResolutionTime::unsafe_shared_current_time() + delay;
    m_frame_timer_purpose = FrameTimerPurpose::CompositorWatchdog;
    m_frame_timer->restart(static_cast<int>(AK::ceil(delay)));
}

void PageClient::frame_timer_fired()
{
    auto purpose = m_frame_timer_purpose;
    m_frame_timer_purpose = FrameTimerPurpose::Inactive;

    auto document = page().has_local_root_navigable() ? page().local_root_navigable()->active_document() : nullptr;
    // NB: The Compositor keeps its pending request while the context is hidden. Forget our copy once its watchdog
    //     fires so becoming visible can arm a new watchdog for the retained request.
    if (document && document->hidden()) {
        if (purpose == FrameTimerPurpose::CompositorWatchdog) {
            m_compositor_rendering_opportunity_outstanding = false;
            m_compositor_watchdog_deadline = 0;
        }
        return;
    }

    auto now = Web::HighResolutionTime::unsafe_shared_current_time();
    if (purpose == FrameTimerPurpose::CompositorWatchdog) {
        if (!m_compositor_rendering_opportunity_outstanding)
            return;
        // A timer event that became runnable before stop() may survive a subsequent restart. Do not let an event from
        // an earlier request satisfy the current request before its own watchdog deadline.
        if (now < m_compositor_watchdog_deadline) {
            m_frame_timer_purpose = FrameTimerPurpose::CompositorWatchdog;
            m_frame_timer->restart(static_cast<int>(AK::ceil(m_compositor_watchdog_deadline - now)));
            return;
        }
        // An opportunity IPC may already be waiting behind a long main-thread task when this timer becomes runnable.
        // Defer the fallback once so an arrived reply is handled first, while a genuinely lost reply still falls back
        // on the next event-loop dispatch.
        auto watchdog_deadline = m_compositor_watchdog_deadline;
        Web::Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [page_client = GC::Ref { *this }, watchdog_deadline] {
            if (!page_client->m_compositor_rendering_opportunity_outstanding)
                return;
            if (page_client->m_compositor_watchdog_deadline != watchdog_deadline)
                return;
            auto document = page_client->page().has_local_root_navigable() ? page_client->page().local_root_navigable()->active_document() : nullptr;
            if (document && document->hidden()) {
                page_client->m_compositor_rendering_opportunity_outstanding = false;
                page_client->m_compositor_watchdog_deadline = 0;
                return;
            }
            page_client->m_compositor_rendering_opportunity_outstanding = false;
            page_client->m_compositor_watchdog_deadline = 0;
            page_client->grant_rendering_opportunity(Web::HighResolutionTime::unsafe_shared_current_time(), Web::HTML::EventLoop::RenderingOpportunitySource::Watchdog);
        }));
        return;
    }

    if (purpose == FrameTimerPurpose::LocalFallback && m_rendering_update_requested)
        grant_rendering_opportunity(now, Web::HTML::EventLoop::RenderingOpportunitySource::LocalTimer);
}

void PageClient::rendering_opportunity(i64 frame_time_nanoseconds, double frame_interval_milliseconds)
{
    if (!m_compositor_rendering_opportunity_outstanding)
        return;

    m_compositor_rendering_opportunity_outstanding = false;
    m_compositor_watchdog_deadline = 0;
    m_frame_timer->stop();
    m_frame_timer_purpose = FrameTimerPurpose::Inactive;
    if (isfinite(frame_interval_milliseconds) && frame_interval_milliseconds > 0)
        m_last_rendering_opportunity_frame_interval = frame_interval_milliseconds;
    grant_rendering_opportunity(static_cast<double>(frame_time_nanoseconds) / 1'000'000.0, Web::HTML::EventLoop::RenderingOpportunitySource::Compositor);
}

void PageClient::grant_rendering_opportunity(double frame_time, Web::HTML::EventLoop::RenderingOpportunitySource source)
{
    m_rendering_update_requested = false;
    m_rendering_opportunity_granted = true;
    m_granted_rendering_opportunity_time = frame_time;
    m_granted_rendering_opportunity_source = source;

    if (!Web::HTML::main_thread_event_loop().running_rendering_task())
        deliver_granted_rendering_opportunity();
}

void PageClient::deliver_granted_rendering_opportunity()
{
    VERIFY(m_rendering_opportunity_granted);
    VERIFY(m_granted_rendering_opportunity_time.has_value());
    if (!Web::HTML::main_thread_event_loop().rendering_opportunity(*m_granted_rendering_opportunity_time, m_granted_rendering_opportunity_source)) {
        m_rendering_opportunity_granted = false;
        m_granted_rendering_opportunity_time.clear();
        m_rendering_update_requested = true;
    }
}

void PageClient::will_begin_rendering_update()
{
    m_rendering_opportunity_for_current_update = m_rendering_opportunity_granted;
    m_rendering_opportunity_granted = false;
    m_granted_rendering_opportunity_time.clear();
}

bool PageClient::has_rendering_opportunity() const
{
    return m_rendering_opportunity_granted || m_rendering_opportunity_for_current_update;
}

void PageClient::did_finish_rendering_update()
{
    m_rendering_opportunity_for_current_update = false;
    if (m_rendering_opportunity_granted) {
        deliver_granted_rendering_opportunity();
        return;
    }
    request_rendering_opportunity_if_needed();
}

void PageClient::set_manual_rendering_opportunities(bool enabled)
{
    if (m_manual_rendering_opportunities == enabled)
        return;

    m_manual_rendering_opportunities = enabled;
    if (enabled) {
        m_frame_timer->stop();
        m_frame_timer_purpose = FrameTimerPurpose::Inactive;
        m_compositor_rendering_opportunity_outstanding = false;
        if (m_rendering_opportunity_granted)
            m_rendering_update_requested = true;
        m_rendering_opportunity_granted = false;
        m_granted_rendering_opportunity_time.clear();
        return;
    }

    request_rendering_opportunity_if_needed();
}

void PageClient::inject_rendering_opportunity(double frame_time)
{
    VERIFY(m_manual_rendering_opportunities);
    if (!m_rendering_update_requested || m_rendering_opportunity_granted)
        return;

    auto document = page().has_local_root_navigable() ? page().local_root_navigable()->active_document() : nullptr;
    if (document && document->hidden())
        return;

    grant_rendering_opportunity(frame_time, Web::HTML::EventLoop::RenderingOpportunitySource::Manual);
}

void PageClient::set_maximum_frames_per_second(double maximum_frames_per_second)
{
    if (!isfinite(maximum_frames_per_second) || maximum_frames_per_second <= 0)
        return;
    m_maximum_frames_per_second = maximum_frames_per_second;
    m_last_rendering_opportunity_frame_interval = 1000.0 / maximum_frames_per_second;
}

void PageClient::page_did_request_cursor_change(Gfx::Cursor const& cursor)
{
    client().async_did_request_cursor_change(m_id, cursor);
}

void PageClient::page_did_change_title(Utf16String const& title)
{
    client().async_did_change_title(m_id, title);
}

void PageClient::page_did_update_editing_history_state(bool can_undo, bool can_redo)
{
    client().async_did_update_editing_history_state(m_id, can_undo, can_redo);
}

void PageClient::page_did_request_refresh()
{
    client().async_did_request_refresh(m_id);
}

void PageClient::page_did_request_resize_window(Gfx::IntSize size, u64 completion_id)
{
    client().async_did_request_resize_window(m_id, size, completion_id);
}

void PageClient::page_did_request_reposition_window(Gfx::IntPoint position, u64 completion_id)
{
    client().async_did_request_reposition_window(m_id, position, completion_id);
}

void PageClient::page_did_request_restore_window()
{
    client().async_did_request_restore_window(m_id);
}

void PageClient::page_did_request_maximize_window(u64 completion_id)
{
    client().async_did_request_maximize_window(m_id, completion_id);
}

void PageClient::page_did_request_minimize_window()
{
    client().async_did_request_minimize_window(m_id);
}

void PageClient::page_did_request_fullscreen_window()
{
    client().async_did_request_fullscreen_window(m_id);
}

void PageClient::page_did_request_exit_fullscreen()
{
    client().async_did_request_exit_fullscreen(m_id);
}

void PageClient::page_did_request_tooltip_override(Web::CSSPixelPoint position, ByteString const& title)
{
    auto device_position = page().css_to_device_point(position);
    client().async_did_request_tooltip_override(m_id, { device_position.x(), device_position.y() }, title);
}

void PageClient::page_did_stop_tooltip_override()
{
    client().async_did_leave_tooltip_area(m_id);
}

void PageClient::page_did_enter_tooltip_area(ByteString const& title)
{
    client().async_did_enter_tooltip_area(m_id, title);
}

void PageClient::page_did_leave_tooltip_area()
{
    client().async_did_leave_tooltip_area(m_id);
}

void PageClient::page_did_hover_link(URL::URL const& url)
{
    client().async_did_hover_link(m_id, url);
}

void PageClient::page_did_unhover_link()
{
    client().async_did_unhover_link(m_id);
}

void PageClient::page_did_click_link(URL::URL const& url, ByteString const& target, unsigned modifiers)
{
    client().async_did_click_link(m_id, url, target, modifiers);
}

void PageClient::page_did_middle_click_link(URL::URL const& url, ByteString const& target, unsigned modifiers)
{
    client().async_did_middle_click_link(m_id, url, target, modifiers);
}

void PageClient::page_did_request_external_url(URL::URL const& url, URL::Origin const& initiator_origin, bool has_transient_activation)
{
    client().async_did_request_external_url(m_id, url, initiator_origin, has_transient_activation);
}

void PageClient::page_did_create_new_document(Web::DOM::Document& document)
{
    initialize_js_console(document);
    apply_pending_geolocation_emulated_position();
}

void PageClient::page_did_change_active_document_in_top_level_browsing_context(Web::DOM::Document& document)
{
    auto& realm = document.relevant_settings_object().realm();

    clear_pending_dom_mutations();

    if (m_web_ui && &m_web_ui->document() != &document)
        m_web_ui.clear();

    if (auto console_client = document.console_client()) {
        auto& web_content_console_client = as<WebContentConsoleClient>(*console_client);
        m_top_level_document_console_client = web_content_console_client;

        auto console_object = realm.intrinsics().console_object();
        console_object->console().set_client(*console_client);
    }
}

void PageClient::page_did_finish_loading(Optional<Utf16String> const& navigation_id, URL::URL const& url)
{
    client().async_did_finish_loading(m_id, navigation_id, url);
}

Optional<u64> PageClient::page_did_start_download(Web::HTML::CrossProcessId navigable_id, Optional<Utf16String> const& navigation_id, URL::URL const& url, ByteString const& suggested_filename, Optional<u64> total_size, int request_server_client_id, u64 request_server_request_id, ByteBuffer initial_data)
{
    auto response = client().send_sync<Messages::WebContentClient::DidStartDownload>(m_id, navigable_id, navigation_id, url, suggested_filename, total_size, request_server_client_id, request_server_request_id, move(initial_data));
    return response->download_id();
}

Optional<u64> PageClient::page_did_start_download(URL::URL const& url, ByteString const& suggested_filename, Optional<u64> total_size)
{
    auto response = client().send_sync<Messages::WebContentClient::DidStartDownloadWithoutRequest>(m_id, url, suggested_filename, total_size);
    return response->download_id();
}

void PageClient::page_did_receive_download_data(u64 download_id, ByteBuffer data)
{
    if (m_canceled_downloads.contains(download_id))
        return;

    auto bytes = data.bytes();
    for (size_t offset = 0; offset < bytes.size(); offset += s_max_download_data_ipc_chunk_size) {
        auto chunk_size = min(bytes.size() - offset, s_max_download_data_ipc_chunk_size);
        client().async_did_receive_download_data(m_id, download_id, bytes.slice(offset, chunk_size));
    }
}

void PageClient::page_did_finish_download(u64 download_id)
{
    page_did_unregister_download(download_id);
    client().async_did_finish_download(m_id, download_id);
}

void PageClient::page_did_fail_download(u64 download_id, String const& error)
{
    page_did_unregister_download(download_id);
    client().async_did_fail_download(m_id, download_id, error);
}

void PageClient::page_did_register_download_controller(u64 download_id, GC::Ref<Web::Fetch::Infrastructure::FetchController> controller)
{
    m_download_controllers.set(download_id, controller);
}

void PageClient::page_did_register_download_reader(u64 download_id, GC::Ref<Web::Streams::ReadableStreamDefaultReader> reader)
{
    m_download_readers.set(download_id, reader);
}

void PageClient::page_did_unregister_download(u64 download_id)
{
    m_download_controllers.remove(download_id);
    m_download_readers.remove(download_id);
    m_canceled_downloads.remove(download_id);
}

bool PageClient::page_is_download_canceled(u64 download_id) const
{
    return m_canceled_downloads.contains(download_id);
}

void PageClient::cancel_download(u64 download_id)
{
    auto controller = m_download_controllers.take(download_id);
    auto reader = m_download_readers.take(download_id);
    if (!controller.has_value() && !reader.has_value())
        return;

    m_canceled_downloads.set(download_id);

    if (controller.has_value())
        controller.value()->stop_fetch();

    if (reader.has_value())
        Web::Fetch::Infrastructure::cancel_incremental_read(*reader.value());
}

void PageClient::page_did_finish_test(Utf16String const& text)
{
    client().async_did_finish_test(m_id, text.to_utf8());
}

void PageClient::page_did_set_test_timeout(double milliseconds)
{
    client().async_did_set_test_timeout(m_id, milliseconds);
}

void PageClient::page_did_receive_reference_test_metadata(JsonValue metadata)
{
    client().async_did_receive_reference_test_metadata(m_id, metadata);
}

void PageClient::page_did_set_browser_zoom(double factor)
{
    auto local_root_navigable = page().local_root_navigable();
    local_root_navigable->set_pending_set_browser_zoom_request(true);
    client().async_did_set_browser_zoom(m_id, factor);
    auto& event_loop = Web::HTML::main_thread_event_loop();
    event_loop.spin_until(GC::create_function(GC::Heap::the(), [this, local_root_navigable]() {
        return !local_root_navigable->pending_set_browser_zoom_request() || !is_connection_open();
    }));
}

void PageClient::page_did_set_device_pixel_ratio_for_testing(double ratio)
{
    set_viewport(m_viewport_size, ratio);
}

void PageClient::page_did_request_context_menu(Web::CSSPixelPoint content_position, Web::ContextMenuForInputEventsTarget for_input_events_target)
{
    client().async_did_request_context_menu(m_id, page().css_to_device_point(content_position).to_type<int>(), for_input_events_target);
}

void PageClient::page_did_request_link_context_menu(Web::CSSPixelPoint content_position, URL::URL const& url, ByteString const& target, unsigned modifiers)
{
    client().async_did_request_link_context_menu(m_id, page().css_to_device_point(content_position).to_type<int>(), url, target, modifiers);
}

void PageClient::page_did_request_image_context_menu(Web::CSSPixelPoint content_position, URL::URL const& url, ByteString const& target, unsigned modifiers, Optional<Gfx::Bitmap const*> bitmap_pointer)
{
    Optional<Gfx::ShareableBitmap> bitmap;
    if (bitmap_pointer.has_value() && bitmap_pointer.value())
        bitmap = bitmap_pointer.value()->to_shareable_bitmap();

    client().async_did_request_image_context_menu(m_id, page().css_to_device_point(content_position).to_type<int>(), url, target, modifiers, bitmap);
}

void PageClient::page_did_request_media_context_menu(Web::CSSPixelPoint content_position, ByteString const& target, unsigned modifiers, Web::Page::MediaContextMenu const& menu)
{
    client().async_did_request_media_context_menu(m_id, page().css_to_device_point(content_position).to_type<int>(), target, modifiers, menu);
}

void PageClient::set_geolocation_emulated_position(WebView::GeolocationPositionData const& position, Optional<u16> error_code)
{
    m_pending_geolocation_emulated_position = PendingGeolocationEmulatedPosition {
        .position = position,
        .error_code = error_code,
    };
    apply_pending_geolocation_emulated_position();
}

void PageClient::apply_pending_geolocation_emulated_position()
{
    if (!m_pending_geolocation_emulated_position.has_value() || !page().top_level_traversable_is_initialized())
        return;

    auto const& pending = *m_pending_geolocation_emulated_position;
    auto const& position = pending.position;
    auto traversable = page().top_level_traversable();

    if (pending.error_code.has_value())
        traversable->set_emulated_position_data(geolocation_position_error_code_from_ipc(*pending.error_code));
    else if (auto coordinates = geolocation_coordinates_from_ipc(position); coordinates.has_value())
        traversable->set_emulated_position_data(*coordinates);
    else
        traversable->set_emulated_position_data(Empty {});
}

void PageClient::geolocation_position_response(u64 request_id, WebView::GeolocationPositionData const& position, Optional<u16> error_code)
{
    if (error_code.has_value())
        page().receive_geolocation_position(request_id, geolocation_position_error_code_from_ipc(*error_code));
    else if (auto coordinates = geolocation_coordinates_from_ipc(position); coordinates.has_value())
        page().receive_geolocation_position(request_id, *coordinates);
    else
        page().receive_geolocation_position(request_id, Web::Geolocation::GeolocationPositionError::ErrorCode::PositionUnavailable);
}

void PageClient::page_did_request_geolocation_position(u64 request_id)
{
    client().async_did_request_geolocation_position(m_id, request_id);
}

void PageClient::page_did_cancel_geolocation_position_request(u64 request_id)
{
    client().async_did_cancel_geolocation_position_request(m_id, request_id);
}

void PageClient::page_did_start_geolocation_position_watch(u64 request_id)
{
    client().async_did_start_geolocation_position_watch(m_id, request_id);
}

void PageClient::page_did_stop_geolocation_position_watch(u64 request_id)
{
    client().async_did_stop_geolocation_position_watch(m_id, request_id);
}

void PageClient::page_did_request_alert(Utf16String const& message)
{
    client().async_did_request_alert(m_id, message);

    if (m_webdriver)
        m_webdriver->page_did_open_dialog({});
}

void PageClient::alert_closed()
{
    page().alert_closed();
}

void PageClient::page_did_request_confirm(Utf16String const& message)
{
    client().async_did_request_confirm(m_id, message);

    if (m_webdriver)
        m_webdriver->page_did_open_dialog({});
}

void PageClient::confirm_closed(bool accepted)
{
    page().confirm_closed(accepted);
}

void PageClient::page_did_request_prompt(Utf16String const& message, Utf16String const& default_)
{
    client().async_did_request_prompt(m_id, message, default_);

    if (m_webdriver)
        m_webdriver->page_did_open_dialog({});
}

void PageClient::page_did_request_set_prompt_text(Utf16String const& text)
{
    client().async_did_request_set_prompt_text(m_id, text);
}

void PageClient::prompt_closed(Optional<Utf16String> response)
{
    page().prompt_closed(move(response));
}

void PageClient::color_picker_update(Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state)
{
    page().color_picker_update(picked_color, state);
}

void PageClient::select_dropdown_closed(Optional<u32> const& selected_item_id)
{
    page().select_dropdown_closed(selected_item_id);
}

void PageClient::toggle_media_play_state()
{
    page().toggle_media_play_state();
}

void PageClient::toggle_media_mute_state()
{
    page().toggle_media_mute_state();
}

void PageClient::toggle_media_loop_state()
{
    page().toggle_media_loop_state();
}

void PageClient::toggle_media_controls_state()
{
    page().toggle_media_controls_state();
}

void PageClient::set_user_style(String source)
{
    page().set_user_style(Utf16String::from_utf8(source));
}

void PageClient::page_did_request_accept_dialog()
{
    client().async_did_request_accept_dialog(m_id);
}

void PageClient::page_did_request_dismiss_dialog()
{
    client().async_did_request_dismiss_dialog(m_id);
}

void PageClient::page_did_change_favicon(Gfx::Bitmap const& favicon)
{
    client().async_did_change_favicon(m_id, favicon.to_shareable_bitmap());
}

Optional<Core::SharedVersion> PageClient::page_did_request_document_cookie_version(Core::SharedVersionIndex document_index)
{
    return Core::get_shared_version(m_document_cookie_version_buffer, document_index);
}

void PageClient::page_did_receive_document_cookie_version_buffer(Core::AnonymousBuffer document_cookie_version_buffer)
{
    m_document_cookie_version_buffer = move(document_cookie_version_buffer);
}

void PageClient::page_did_request_document_cookie_version_index(Web::UniqueNodeID document_id, String const& domain)
{
    // FIXME: Support transferring DistinctNumeric over IPC.
    client().async_did_request_document_cookie_version_index(m_id, document_id.value(), domain);
}

void PageClient::page_did_receive_document_cookie_version_index(Web::UniqueNodeID document_id, Core::SharedVersionIndex document_index)
{
    if (auto* document = as_if<Web::DOM::Document>(Web::DOM::Node::from_unique_id(document_id)))
        document->set_cookie_version_index(document_index);
}

Vector<HTTP::Cookie::Cookie> PageClient::page_did_request_all_cookies_webdriver(URL::URL const& url)
{
    return client().did_request_all_cookies_webdriver(url);
}

Vector<HTTP::Cookie::Cookie> PageClient::page_did_request_all_cookies_cookiestore(URL::URL const& url)
{
    return client().did_request_all_cookies_cookiestore(url);
}

Optional<HTTP::Cookie::Cookie> PageClient::page_did_request_named_cookie(URL::URL const& url, String const& name)
{
    return client().did_request_named_cookie(url, name);
}

HTTP::Cookie::VersionedCookie PageClient::page_did_request_cookie(URL::URL const& url, HTTP::Cookie::Source source)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidRequestCookie>(m_id, url, source);
    if (!response) {
        dbgln("WebContent client disconnected during DidRequestCookie. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
    return response->take_cookie();
}

void PageClient::page_did_set_cookie(URL::URL const& url, HTTP::Cookie::ParsedCookie const& cookie, HTTP::Cookie::Source source)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidSetCookie>(url, cookie, source);
    if (!response) {
        dbgln("WebContent client disconnected during DidSetCookie. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
}

void PageClient::page_did_update_cookie(HTTP::Cookie::Cookie const& cookie)
{
    client().async_did_update_cookie(cookie);

    // Since the above (test-only) IPC is async, we reset the document cookie version now to avoid a stale cache.
    if (auto* document = page().top_level_browsing_context().active_document())
        document->reset_cookie_version();
}

void PageClient::page_did_expire_cookies_with_time_offset(AK::Duration offset)
{
    client().async_did_expire_cookies_with_time_offset(offset);

    // Since the above (test-only) IPC is async, we reset the document cookie version now to avoid a stale cache.
    if (auto* document = page().top_level_browsing_context().active_document())
        document->reset_cookie_version();
}

void PageClient::page_did_delete_all_cookies(URL::URL const& url, GC::Ref<Web::WebIDL::Promise> promise)
{
    auto request_id = m_next_delete_all_cookies_request_id++;
    m_pending_delete_all_cookies_promises.set(request_id, promise);
    client().async_did_request_delete_all_cookies(m_id, request_id, url);

    if (auto* document = page().top_level_browsing_context().active_document())
        document->reset_cookie_version();
}

void PageClient::did_delete_all_cookies(u64 request_id)
{
    auto maybe_promise = m_pending_delete_all_cookies_promises.take(request_id);
    if (!maybe_promise.has_value())
        return;

    auto promise = maybe_promise.release_value();
    auto& realm = promise->promise()->shape().realm();
    Web::HTML::TemporaryExecutionContext execution_context { realm, Web::HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    Web::WebIDL::resolve_promise(realm, promise);
}

void PageClient::page_did_lose_request_server_connection()
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidLoseRequestServerConnection>();
    if (!response)
        return;

    auto handle = response->take_handle();
    if (!handle.has_value())
        return;

    if (client().on_request_server_connection)
        client().on_request_server_connection(*handle);
}

void PageClient::page_did_simulate_worker_request_server_connection_loss()
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidSimulateWorkerRequestServerConnectionLoss>(m_id);
    if (!response) {
        dbgln("WebContent client disconnected during DidSimulateWorkerRequestServerConnectionLoss. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
}

void PageClient::page_did_store_hsts_policy(String const& domain, HTTP::HSTS::ParsedHSTSPolicy const& policy)
{
    client().async_did_store_hsts_policy(domain, policy);
}

bool PageClient::page_did_is_known_hsts_host(String const& domain)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidIsKnownHstsHost>(domain);
    if (!response) {
        dbgln("WebContent client disconnected during DidIsKnownHstsHost. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
    return response->result();
}

Optional<Utf16String> PageClient::page_did_request_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key, Utf16String const& bottle_key)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidRequestStorageItem>(m_id, storage_endpoint, storage_key, bottle_key);
    if (!response) {
        dbgln("WebContent client disconnected during DidRequestStorageItem. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
    return response->take_value();
}

WebView::StorageSetResult PageClient::page_did_set_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key, Utf16String const& bottle_key, Utf16String const& value)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidSetStorageItem>(m_id, storage_endpoint, storage_key, bottle_key, value);
    if (!response) {
        dbgln("WebContent client disconnected during DidSetStorageItem. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
    return response->result();
}

void PageClient::page_did_remove_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key, Utf16String const& bottle_key)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidRemoveStorageItem>(m_id, storage_endpoint, storage_key, bottle_key);
    if (!response) {
        dbgln("WebContent client disconnected during DidRemoveStorageItem. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
}

Vector<Utf16String> PageClient::page_did_request_storage_keys(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidRequestStorageKeys>(m_id, storage_endpoint, storage_key);
    if (!response) {
        dbgln("WebContent client disconnected during DidRequestStorageKeys. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
    return response->take_keys();
}

u64 PageClient::page_did_request_storage_usage(String const& storage_key)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidRequestStorageUsage>(m_id, storage_key);
    if (!response) {
        dbgln("WebContent client disconnected during DidRequestStorageUsage. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
    return response->usage();
}

void PageClient::page_did_clear_storage(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidClearStorage>(m_id, storage_endpoint, storage_key);
    if (!response) {
        dbgln("WebContent client disconnected during DidClearStorage. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
}

void PageClient::page_did_broadcast_storage_change(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& url, Optional<Utf16String> const& key, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value)
{
    client().async_did_change_storage_item(m_id, storage_endpoint, url, key, old_value, new_value);
}

void PageClient::page_did_update_indexed_database(String const& url, Web::IndexedDB::TransactionChanges const& changes)
{
    if (!has_devtools_client())
        return;

    auto update = DevTools::IndexedDB::serialize_update(url, changes);
    if (update.is_empty())
        return;

    client().async_did_update_indexed_database(m_id, update.serialized());
}

void PageClient::page_did_post_broadcast_channel_message(Web::HTML::BroadcastChannelMessage const& message)
{
    client().async_did_post_broadcast_channel_message(m_id, message);
}

void PageClient::page_did_update_resource_count(i32 count_waiting)
{
    client().async_did_update_resource_count(m_id, count_waiting);
}

PageClient::NewWebViewResult PageClient::page_did_request_new_web_view(Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints)
{
    // FIXME: Create an abstraction to let this WebContent process know about a new process we create?
    // FIXME: For now, just create a new page in the same process anyway
    // FIXME: Proper agent-cluster separation must also cover same-process
    // COOP/noopener popups before they receive distinct main-world cells.
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidRequestNewWebView>(m_id, activate_tab, hints);
    if (!response) {
        dbgln("WebContent client disconnected during DidRequestNewWebView. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }

    if (!response->new_page_id().has_value())
        return {};
    VERIFY(response->root_navigable_id().has_value());

    auto& new_client = m_owner.create_page(*response->new_page_id(), *response->root_navigable_id());
    return { &new_client.page(), response->system_visibility_state(), response->take_handle() };
}

void PageClient::page_did_request_activate_tab()
{
    client().async_did_request_activate_tab(m_id);
}

void PageClient::page_did_close_top_level_traversable()
{
    page().local_root_navigable()->compositor_context().stop_presenting_to_client();

    // FIXME: Rename this IPC call
    client().async_did_close_browsing_context(m_id);

    // NOTE: This only removes the strong reference the PageHost has for this PageClient.
    //       It will be GC'd 'later'.
    m_owner.remove_page({}, m_id);
}

void PageClient::page_did_create_top_level_traversable(Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryDescriptor const& initial_history_entry, Optional<Web::HTML::CrossProcessId> opener_navigable_id)
{
    client().async_did_create_top_level_traversable(m_id, navigable_id, initial_history_entry, opener_navigable_id);
}

void PageClient::page_did_change_needs_beforeunload_check(bool needs_beforeunload_check)
{
    client().async_did_change_needs_beforeunload_check(m_id, needs_beforeunload_check);
}

void PageClient::send_current_needs_beforeunload_check()
{
    client().async_did_change_needs_beforeunload_check(m_id, page().needs_beforeunload_check());
}

void PageClient::page_did_update_session_history_entry_navigation_api_state(Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity const& entry_identity, Web::HTML::StorageSerializationRecord const& navigation_api_state)
{
    client().async_did_update_session_history_entry_navigation_api_state(m_id, navigable_id, entry_identity, navigation_api_state);
}

void PageClient::page_did_update_session_history_entry_scroll_restoration_mode(Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity const& entry_identity, Web::HTML::ScrollRestorationMode scroll_restoration_mode)
{
    client().async_did_update_session_history_entry_scroll_restoration_mode(m_id, navigable_id, entry_identity, scroll_restoration_mode);
}

void PageClient::page_did_update_session_history_entry_document_state_navigable_target_name(Web::HTML::CrossProcessId navigable_id, Web::HTML::SessionHistoryEntryIdentity const& entry_identity, Utf16String const& navigable_target_name)
{
    client().async_did_update_session_history_entry_document_state_navigable_target_name(m_id, navigable_id, entry_identity, navigable_target_name);
}

void PageClient::page_did_set_session_history_entry_document_state_reload_pending(Web::HTML::CrossProcessId navigable_id, Utf16String const& navigation_api_key, bool reload_pending)
{
    client().async_did_set_session_history_entry_document_state_reload_pending(m_id, navigable_id, navigation_api_key, reload_pending);
}

void PageClient::page_did_request_set_system_visibility_state(Web::HTML::VisibilityState visibility_state)
{
    client().async_did_request_set_system_visibility_state(m_id, visibility_state);
}

void PageClient::page_did_request_history_operation(Web::HTML::CrossProcessId operation_id, Web::HistoryOperationParameters parameters)
{
    client().async_request_history_operation(m_id, operation_id, move(parameters));
}

void PageClient::page_did_request_child_navigable_unload(Web::HTML::CrossProcessId navigable_id)
{
    client().async_request_child_navigable_unload(m_id, navigable_id);
}

String PageClient::page_did_request_ui_process_session_history_for_testing()
{
    return client().did_request_ui_process_session_history_for_testing(m_id);
}

String PageClient::dump_site_isolation_process_tree_for_testing()
{
    return client().did_request_site_isolation_process_tree_for_testing(m_id);
}

void PageClient::crash_remote_frame_processes_for_testing()
{
    client().async_did_request_crash_of_remote_frame_processes_for_testing(m_id);
}

bool PageClient::page_did_request_capture_session_history_snapshot_for_testing()
{
    return client().did_request_capture_session_history_snapshot_for_testing(m_id);
}

bool PageClient::page_did_request_restore_session_history_snapshot_for_testing()
{
    return client().did_request_restore_session_history_snapshot_for_testing(m_id);
}

bool PageClient::page_did_request_register_session_store_tab_for_testing()
{
    return client().did_request_register_session_store_tab_for_testing(m_id);
}

String PageClient::page_did_request_session_store_tab_state_for_testing()
{
    return client().did_request_session_store_tab_state_for_testing(m_id);
}

void PageClient::run_webdriver_user_prompt_handling(u64 request_id)
{
    Web::WebDriver::handle_any_user_prompts(page(),
        GC::create_function(heap(), [this, request_id](Optional<Web::WebDriver::Error> error) {
            auto response = error.has_value()
                ? Web::WebDriver::Response { error.release_value() }
                : Web::WebDriver::Response { JsonValue {} };
            client().async_webdriver_user_prompt_handling_complete(m_id, request_id, move(response));
        }));
}

void PageClient::request_file(Web::FileRequest file_request)
{
    client().request_file(m_id, move(file_request));
}

void PageClient::page_did_request_color_picker(Color current_color)
{
    client().async_did_request_color_picker(m_id, current_color);
}

void PageClient::page_did_request_file_picker(Web::HTML::FileFilter const& accepted_file_types, Web::HTML::AllowMultipleFiles allow_multiple_files)
{
    client().async_did_request_file_picker(m_id, accepted_file_types, allow_multiple_files);
}

void PageClient::page_did_request_select_dropdown(Web::CSSPixelPoint content_position, Web::CSSPixels minimum_width, Vector<Web::HTML::SelectItem> items)
{
    client().async_did_request_select_dropdown(m_id, page().css_to_device_point(content_position).to_type<int>(), minimum_width * device_pixels_per_css_pixel(), items);
}

void PageClient::page_did_change_theme_color(Gfx::Color color)
{
    client().async_did_change_theme_color(m_id, color);
}

void PageClient::page_did_change_background_color(Gfx::Color color)
{
    client().async_did_change_background_color(m_id, color);
}

void PageClient::page_did_insert_clipboard_item(Web::Clipboard::SystemClipboardItem const& item, StringView presentation_style)
{
    client().async_did_insert_clipboard_item(m_id, item, presentation_style);
}

void PageClient::page_did_request_clipboard_entries(u64 request_id)
{
    client().async_did_request_clipboard_entries(m_id, request_id);
}

void PageClient::page_did_request_primary_paste()
{
    client().async_did_request_primary_paste(m_id);
}

void PageClient::page_did_complete_paste_action()
{
    client().update_input_method_state(m_id);
}

void PageClient::page_did_update_primary_selection(Utf16String const& text)
{
    client().async_did_update_primary_selection(m_id, text.to_utf8());
}

void PageClient::page_did_change_audio_play_state(Web::HTML::AudioPlayState play_state)
{
    client().async_did_change_audio_play_state(m_id, play_state);
}

void PageClient::page_did_change_screen_wake_lock_state(Web::ScreenWakeLockState wake_lock_state)
{
    client().async_did_change_screen_wake_lock_state(m_id, wake_lock_state);
}

Web::HTML::WorkerAgentId PageClient::start_worker_agent(Web::HTML::WorkerAgentStartRequest&& request)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::StartWorkerAgent>(m_id, move(request));
    if (!response) {
        dbgln("WebContent client disconnected during StartWorkerAgent. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }

    return response->agent_id();
}

void PageClient::close_worker_agent(Web::HTML::WorkerAgentId agent_id, Web::HTML::WorkerAgentOwnerToken owner_token)
{
    client().async_close_worker_agent(m_id, agent_id, owner_token);
}

void PageClient::page_did_mutate_dom(Utf16FlyString const& type, Web::DOM::Node const& target, Web::DOM::NodeList& added_nodes, Web::DOM::NodeList& removed_nodes, GC::Ptr<Web::DOM::Node>, GC::Ptr<Web::DOM::Node>, Optional<Utf16FlyString> const& attribute_name)
{
    Optional<WebView::Mutation::Type> mutation;

    if (type == Web::DOM::MutationType::attributes) {
        VERIFY(attribute_name.has_value());

        auto const& element = as<Web::DOM::Element>(target);
        mutation = WebView::AttributeMutation {
            *attribute_name,
            element.attribute(*attribute_name)
        };
    } else if (type == Web::DOM::MutationType::characterData) {
        auto const& character_data = as<Web::DOM::CharacterData>(target);
        mutation = WebView::CharacterDataMutation { character_data.data().to_utf8_but_should_be_ported_to_utf16() };
    } else if (type == Web::DOM::MutationType::childList) {
        Vector<Web::UniqueNodeID> added;
        added.ensure_capacity(added_nodes.length());

        Vector<Web::UniqueNodeID> removed;
        removed.ensure_capacity(removed_nodes.length());

        for (auto i = 0u; i < added_nodes.length(); ++i)
            added.unchecked_append(added_nodes.item(i)->unique_id());
        for (auto i = 0u; i < removed_nodes.length(); ++i)
            removed.unchecked_append(removed_nodes.item(i)->unique_id());

        mutation = WebView::ChildListMutation { move(added), move(removed), target.child_count() };
    } else {
        VERIFY_NOT_REACHED();
    }

    auto mutation_message = WebView::Mutation { type.to_utf16_string().to_utf8(), target.unique_id(), {}, mutation.release_value() };
    if (m_pending_dom_mutations.is_empty() && target.document().layout_is_up_to_date()) {
        send_dom_mutation(target, move(mutation_message));
        return;
    }

    m_pending_dom_mutations.enqueue({ const_cast<Web::DOM::Node&>(target), move(mutation_message) });
}

void PageClient::flush_pending_dom_mutations()
{
    if (!page().listen_for_dom_mutations()) {
        clear_pending_dom_mutations();
        return;
    }

    while (!m_pending_dom_mutations.is_empty()) {
        if (!m_pending_dom_mutations.head().target->document().layout_is_up_to_date())
            break;

        auto pending_mutation = m_pending_dom_mutations.dequeue();
        send_dom_mutation(*pending_mutation.target, move(pending_mutation.mutation));
    }
}

void PageClient::clear_pending_dom_mutations()
{
    m_pending_dom_mutations.clear();
}

void PageClient::send_dom_mutation(Web::DOM::Node const& target, WebView::Mutation mutation)
{
    mutation.serialized_target = serialize_dom_mutation_target(target);
    client().async_did_mutate_dom(m_id, move(mutation));
}

void PageClient::page_did_take_screenshot(Gfx::ShareableBitmap const& screenshot)
{
    client().async_did_take_screenshot(m_id, screenshot);
}

WebDriverConnection& PageClient::ensure_webdriver_session()
{
    if (!m_webdriver)
        m_webdriver = WebDriverConnection::create(*this);
    return *m_webdriver;
}

void PageClient::run_webdriver_command(u64 command_id, String const& name, JsonValue payload, Vector<String> arguments)
{
    ensure_webdriver_session().run_command(command_id, name, move(payload), move(arguments));
}

void PageClient::webdriver_command_complete(u64 command_id, Web::WebDriver::Response response)
{
    client().async_webdriver_command_complete(m_id, command_id, move(response));
}

void PageClient::set_webdriver_session_config(Web::WebDriver::UserPromptHandler user_prompt_handler, Web::WebDriver::PageLoadStrategy page_load_strategy, bool strict_file_interactability, JsonValue const& timeouts)
{
    Web::WebDriver::set_user_prompt_handler(move(user_prompt_handler));
    ensure_webdriver_session().set_session_config(page_load_strategy, strict_file_interactability, timeouts);
}

ErrorOr<void> PageClient::connect_to_web_ui(IPC::TransportHandle handle)
{
    auto* active_document = page().top_level_browsing_context().active_document();
    if (!active_document || !active_document->window())
        return {};

    VERIFY(!m_web_ui);
    m_web_ui = TRY(WebUIConnection::connect(move(handle), *active_document));

    return {};
}

void PageClient::received_message_from_web_ui(Utf16String const& name, JS::Value data)
{
    if (m_web_ui)
        m_web_ui->received_message_from_web_ui(name, data);
}

void PageClient::page_did_start_network_request(u64 request_id, URL::URL const& url, ByteString const& method, Vector<HTTP::Header> const& request_headers, ReadonlyBytes request_body, Optional<String> initiator_type, String const& referrer_policy, bool is_navigation_request, Web::Fetch::Infrastructure::Request::Priority priority)
{
    client().async_did_start_network_request(m_id, request_id, url, method, request_headers, request_body, move(initiator_type), referrer_policy, is_navigation_request, priority);
}

void PageClient::page_did_receive_network_response_headers(u64 request_id, u32 status_code, Optional<String> reason_phrase, Vector<HTTP::Header> const& response_headers, Requests::CameFromCache came_from_cache)
{
    client().async_did_receive_network_response_headers(m_id, request_id, status_code, move(reason_phrase), response_headers, came_from_cache);
}

void PageClient::page_did_receive_network_response_body(u64 request_id, ReadonlyBytes data)
{
    if (!has_devtools_client())
        return;
    client().async_did_receive_network_response_body(m_id, request_id, data);
}

void PageClient::did_connect_devtools_client()
{
    auto was_first_devtools_client = !has_devtools_client();
    ++m_devtools_client_count;

    if (!was_first_devtools_client)
        return;

    for (auto& navigable : Web::HTML::all_local_navigables()) {
        if (&navigable->page() != &page())
            continue;
        if (auto active_document = navigable->active_document())
            active_document->update_layout(Web::DOM::UpdateLayoutReason::InspectDevToolsLayoutData);
    }
}

void PageClient::did_disconnect_devtools_client()
{
    VERIFY(m_devtools_client_count > 0);
    --m_devtools_client_count;

    if (has_devtools_client())
        return;

    for (auto& navigable : Web::HTML::all_local_navigables()) {
        if (&navigable->page() != &page())
            continue;
        if (auto active_document = navigable->active_document())
            active_document->clear_devtools_layout_inspection_data();
    }
}

void PageClient::page_did_finish_network_request(u64 request_id, u64 body_size, Requests::RequestTimingInfo const& timing_info, Optional<Requests::NetworkError> const& network_error)
{
    client().async_did_finish_network_request(m_id, request_id, body_size, timing_info, network_error);
}

void PageClient::initialize_js_console(Web::DOM::Document& document)
{
    if (document.is_temporary_document_for_fragment_parsing())
        return;

    auto& realm = document.relevant_settings_object().realm();
    auto console_object = realm.intrinsics().console_object();

    auto console_client = DevToolsConsoleClient::create(realm, console_object->console(), *this);
    document.set_console_client(console_client);
}

void PageClient::did_execute_js_console_input(JsonValue const& result)
{
    client().async_did_execute_js_console_input(m_id, result);
}

void PageClient::js_console_input(StringView js_source)
{
    if (m_top_level_document_console_client)
        m_top_level_document_console_client->handle_input(js_source);
}

void PageClient::run_javascript(StringView js_source)
{
    auto* active_document = page().top_level_browsing_context().active_document();

    if (!active_document)
        return;

    // This is partially based on "execute a javascript: URL request" https://html.spec.whatwg.org/multipage/browsing-the-web.html#javascript-protocol

    // Let settings be browsingContext's active document's relevant settings object.
    auto& settings = active_document->relevant_settings_object();

    // Let baseURL be settings's API base URL.
    auto base_url = settings.api_base_url();

    // Let script be the result of creating a classic script given scriptSource, settings, baseURL, and the default classic script fetch options.
    // FIXME: This doesn't pass in "default classic script fetch options"
    // FIXME: What should the filename be here?
    auto script_source = Utf16String::from_utf8(js_source);
    auto script = Web::HTML::ClassicScript::create("(client connection run_javascript)", script_source, settings, move(base_url));

    // Let evaluationStatus be the result of running the classic script script.
    auto evaluation_status = script->run();

    if (evaluation_status.is_error())
        dbgln("Exception :(");
}

void PageClient::did_output_js_console_message(WebView::ConsoleOutput console_output)
{
    client().async_did_output_js_console_message(m_id, move(console_output));
}

void PageClient::console_peer_did_misbehave(char const* reason)
{
    client().did_misbehave(reason);
}

static void gather_style_sheets(Vector<Web::CSS::StyleSheetIdentifier>& results, Web::CSS::CSSStyleSheet& sheet)
{
    if (auto identifier = Web::CSS::style_sheet_identifier_for(sheet); identifier.has_value())
        results.append(identifier.release_value());

    for (auto& import_rule : sheet.import_rules()) {
        if (import_rule->loaded_style_sheet()) {
            gather_style_sheets(results, *import_rule->loaded_style_sheet());
        } else {
            // We can gather this anyway, and hope it loads later
            results.append({
                .type = Web::CSS::StyleSheetIdentifier::Type::ImportRule,
                .url = import_rule->href_for_bindings(),
            });
        }
    }
}

Vector<Web::CSS::StyleSheetIdentifier> PageClient::list_style_sheets() const
{
    Vector<Web::CSS::StyleSheetIdentifier> results;

    auto const* document = page().top_level_browsing_context().active_document();
    if (document) {
        for (auto& sheet : document->style_sheets().sheets()) {
            gather_style_sheets(results, sheet);
        }
    }

    // User style
    if (page().user_style().has_value()) {
        results.append({
            .type = Web::CSS::StyleSheetIdentifier::Type::UserStyle,
        });
    }

    Web::CSS::StyleScope::for_each_user_agent_stylesheet(document && document->in_quirks_mode(), true, [&](auto&, auto const& identifier) {
        results.append(identifier);
    });

    return results;
}

static Web::HTML::ScriptRegistry::Description exported_devtools_source_description(Web::DOM::Document const& document, Web::HTML::ScriptRegistry::Description description)
{
    description.id.document_id = document.unique_id();
    return description;
}

static void append_devtools_sources_for_document(Vector<Web::HTML::ScriptRegistry::Description>& results, Web::DOM::Document const& document)
{
    for (auto const& source : document.script_registry().scripts()) {
        auto description = exported_devtools_source_description(document, source.value.description);
        results.append(move(description));
    }

    for (auto const& navigable : document.descendant_navigables()) {
        auto content_document = navigable->active_document();
        if (!content_document)
            continue;
        append_devtools_sources_for_document(results, *content_document);
    }
}

Vector<Web::HTML::ScriptRegistry::Description> PageClient::list_devtools_sources() const
{
    Vector<Web::HTML::ScriptRegistry::Description> results;

    auto const* document = page().top_level_browsing_context().active_document();
    if (document)
        append_devtools_sources_for_document(results, *document);

    return results;
}

static Optional<Web::HTML::ScriptRegistry::Description> find_devtools_source_description(Web::DOM::Document const& document, JS::SourceCode const& source_code)
{
    if (auto script = document.script_registry().script_for_source_code(source_code); script.has_value())
        return exported_devtools_source_description(document, script->description);

    for (auto const& navigable : document.descendant_navigables()) {
        auto content_document = navigable->active_document();
        if (!content_document)
            continue;
        if (auto description = find_devtools_source_description(*content_document, source_code); description.has_value())
            return description;
    }
    return {};
}

Optional<Web::HTML::ScriptRegistry::Description> PageClient::devtools_source_description(JS::SourceCode const& source_code) const
{
    auto const* document = page().top_level_browsing_context().active_document();
    if (!document)
        return {};
    return find_devtools_source_description(*document, source_code);
}

static Web::DOM::Document const* document_for_devtools_source(PageClient const& page_client, Web::HTML::ScriptRegistry::Identifier const& source_id)
{
    auto* node = Web::DOM::Node::from_unique_id(source_id.document_id);
    auto* document = as_if<Web::DOM::Document>(node);
    if (!document)
        return nullptr;

    auto navigable = document->navigable();
    if (!navigable || &navigable->page() != &page_client.page())
        return nullptr;

    return document;
}

Optional<NonnullRefPtr<JS::SourceCode const>> PageClient::devtools_source_code(Web::HTML::ScriptRegistry::Identifier const& source_id) const
{
    auto* document = document_for_devtools_source(*this, source_id);
    if (!document)
        return {};

    return document->script_registry().source_code(source_id.script_id);
}

Optional<Web::HTML::ScriptRegistry::Content> PageClient::devtools_source_content(Web::HTML::ScriptRegistry::Identifier const& source_id) const
{
    auto* document = document_for_devtools_source(*this, source_id);
    if (!document)
        return {};

    return document->script_registry().script_content(source_id.script_id, document->source().utf16_view());
}

Vector<WebView::DebuggerSourcePosition> PageClient::devtools_source_breakpoint_positions(Web::HTML::ScriptRegistry::Identifier const& source_id) const
{
    auto* document = document_for_devtools_source(*this, source_id);
    if (!document)
        return {};

    Vector<WebView::DebuggerSourcePosition> positions;
    for (auto const& position : document->script_registry().breakpoint_positions(source_id.script_id)) {
        positions.append({
            .line = position.line,
            .column = position.column > 0 ? position.column - 1 : 0,
        });
    }
    return positions;
}

void PageClient::page_did_register_javascript_source(Web::DOM::Document& document, Web::HTML::ScriptRegistry::Description const& source)
{
    if (!has_devtools_client())
        return;

    client().async_did_add_devtools_source(m_id, exported_devtools_source_description(document, source));
}

void PageClient::ensure_compositor_host()
{
    m_owner.ensure_compositor_host();
}

Web::Compositor::CompositorHost* PageClient::compositor_host()
{
    return m_owner.compositor_host();
}

Web::Compositor::CompositorHost const* PageClient::compositor_host() const
{
    return m_owner.compositor_host();
}

void PageClient::queue_screenshot_task(Optional<Web::UniqueNodeID> node_id)
{
    page().queue_screenshot_task(node_id);
}

}
