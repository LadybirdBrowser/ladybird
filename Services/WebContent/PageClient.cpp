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
#include <LibCore/Process.h>
#include <LibCore/Timer.h>
#include <LibDevTools/IndexedDBSerialization.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibIPC/TransportHandle.h>
#include <LibJS/Console.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/CSS/StyleSheetList.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/MutationType.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWebView/SiteIsolation.h>
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
static bool s_should_report_session_history_updates_in_test_mode { false };

GC_DEFINE_ALLOCATOR(PageClient);

static String serialize_dom_mutation_target(Web::DOM::Node const& target)
{
    StringBuilder builder;
    auto serializer = MUST(JsonObjectSerializer<>::try_create(builder));
    target.serialize_tree_as_json(serializer);
    MUST(serializer.finish());
    return MUST(builder.to_string());
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

void PageClient::set_should_report_session_history_updates_in_test_mode(bool should_report)
{
    s_should_report_session_history_updates_in_test_mode = should_report;
}

GC::Ref<PageClient> PageClient::create(JS::VM& vm, PageHost& page_host, u64 id)
{
    return vm.heap().allocate<PageClient>(page_host, id);
}

PageClient::PageClient(PageHost& owner, u64 id)
    : m_owner(owner)
    , m_page(Web::Page::create(Web::Bindings::main_thread_vm(), *this))
    , m_id(id)
{
    m_page->set_async_scrolling_enabled(s_async_scrolling_enabled);
    setup_palette();

    m_frame_timer = Core::Timer::create_single_shot(0, [this] {
        m_last_frame_dispatch_time = Web::HighResolutionTime::unsafe_shared_current_time();
        Web::HTML::main_thread_event_loop().queue_task_to_update_the_rendering();
    });
}

PageClient::~PageClient() = default;

void PageClient::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
    visitor.visit(m_top_level_document_console_client);
    for (auto& promise : m_pending_delete_all_cookies_promises)
        visitor.visit(promise.value);
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

    if (auto document = page().top_level_traversable()->active_document()) {
        if (has_focus)
            document->reset_cursor_blink_cycle();
        document->set_cursor_position_needs_repaint();
    }
}

void PageClient::set_window_handle(String window_handle)
{
    page().top_level_traversable()->set_window_handle(move(window_handle));

    if (m_webdriver)
        m_webdriver->page_did_set_window_handle({}, page().top_level_traversable()->window_handle());
}

void PageClient::did_start_webdriver_navigation(URL::URL const& url)
{
    client().async_did_start_webdriver_navigation(m_id, url);
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

Web::NavigationProcessDecision PageClient::decide_navigation_process(URL::URL const& current_url, URL::URL const& target_url, Web::NavigationTarget target, Optional<String> frame_id) const
{
    if (target != Web::NavigationTarget::TopLevel)
        return client().decide_navigation_process(m_id, move(frame_id), current_url, target_url, target);

    return WebView::is_url_suitable_for_same_process_navigation(current_url, target_url, Web::NavigationTarget::TopLevel)
        ? Web::NavigationProcessDecision::Local
        : Web::NavigationProcessDecision::Remote;
}

void PageClient::request_new_process_for_navigation(URL::URL const& url, Variant<Empty, String, Web::HTML::POSTResource> document_resource, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    if (m_webdriver)
        m_webdriver->page_did_start_window_replacement({}, page().top_level_traversable()->window_handle());

    client().async_did_request_new_process_for_navigation(m_id, url, move(document_resource), history_handling);
}

void PageClient::request_new_process_for_child_frame_navigation(String const& frame_id, URL::URL const& url, Variant<Empty, String, Web::HTML::POSTResource> document_resource, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    client().async_did_request_new_process_for_child_frame_navigation(m_id, frame_id, url, move(document_resource), history_handling);
}

void PageClient::page_did_create_child_frame(String const& parent_frame_id, String const& frame_id)
{
    client().async_did_create_child_frame(m_id, parent_frame_id, frame_id);
}

void PageClient::page_did_update_child_frame_viewport(String const& frame_id, Web::CSSPixelRect viewport_rect)
{
    client().async_did_update_child_frame_viewport(m_id, frame_id, page().css_to_device_rect(viewport_rect), page().client().device_pixel_ratio());
}

void PageClient::page_did_commit_child_frame_navigation(String const& frame_id, URL::URL const& url)
{
    client().async_did_commit_child_frame_navigation(m_id, frame_id, url);
}

void PageClient::page_did_destroy_child_frame(String const& frame_id)
{
    m_remote_child_frame_compositor_contexts.remove(frame_id);
    client().async_did_destroy_child_frame(m_id, frame_id);
}

void PageClient::set_remote_child_frame_compositor_context(String frame_id, Optional<Web::Compositor::CompositorContextId> context_id)
{
    if (context_id.has_value())
        m_remote_child_frame_compositor_contexts.set(move(frame_id), *context_id);
    else
        m_remote_child_frame_compositor_contexts.remove(frame_id);
    request_frame();
}

Optional<Web::Compositor::CompositorContextId> PageClient::compositor_context_id_for_remote_child_frame(String const& frame_id) const
{
    return m_remote_child_frame_compositor_contexts.get(frame_id);
}

void PageClient::run_iframe_load_event_steps(String const& frame_id)
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
    if (auto* document = page().top_level_browsing_context().active_document()) {
        document->invalidate_style(Web::DOM::StyleInvalidationReason::SettingsChange);
        document->set_needs_media_query_evaluation();
    }
    request_frame();
}

void PageClient::set_preferred_color_scheme(Web::CSS::PreferredColorScheme color_scheme)
{
    m_preferred_color_scheme = color_scheme;
    if (auto* document = page().top_level_browsing_context().active_document()) {
        document->invalidate_style(Web::DOM::StyleInvalidationReason::SettingsChange);
        document->set_needs_media_query_evaluation();
    }
}

void PageClient::set_preferred_contrast(Web::CSS::PreferredContrast contrast)
{
    m_preferred_contrast = contrast;
    if (auto* document = page().top_level_browsing_context().active_document()) {
        document->invalidate_style(Web::DOM::StyleInvalidationReason::SettingsChange);
        document->set_needs_media_query_evaluation();
    }
}

void PageClient::set_preferred_motion(Web::CSS::PreferredMotion motion)
{
    m_preferred_motion = motion;
    if (auto* document = page().top_level_browsing_context().active_document()) {
        document->invalidate_style(Web::DOM::StyleInvalidationReason::SettingsChange);
        document->set_needs_media_query_evaluation();
    }
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
}

void PageClient::compositor_process_reconnected()
{
    page().top_level_traversable()->repaint_after_compositor_process_reconnect();
    page().notify_all_canvas_elements_of_lost_backing_storage();
    page().prepare_canvas_contexts_for_compositing();
    page().update_all_media_element_video_sinks();
    Web::HTML::main_thread_event_loop().queue_task_to_update_the_rendering();
}

Queue<Web::QueuedInputEvent>& PageClient::input_event_queue()
{
    return client().input_event_queue();
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

    page().top_level_traversable()->set_viewport_size(page().device_to_css_size(size), invalidate);
}

void PageClient::set_zoom_level(double zoom_level)
{
    m_zoom_level = zoom_level;
    page().top_level_traversable()->set_viewport_size(page().device_to_css_size(m_viewport_size), Web::InvalidateDisplayList::Yes);
}

void PageClient::request_frame()
{
    if (m_frame_timer->is_active())
        return;

    auto delay = 0.0;
    if (m_last_frame_dispatch_time.has_value()) {
        auto now = Web::HighResolutionTime::unsafe_shared_current_time();
        auto minimum_frame_interval = 1000.0 / m_maximum_frames_per_second;
        delay = max(0.0, *m_last_frame_dispatch_time + minimum_frame_interval - now);
    }

    m_frame_timer->restart(static_cast<int>(AK::ceil(delay)));
}

void PageClient::set_maximum_frames_per_second(double maximum_frames_per_second)
{
    m_maximum_frames_per_second = maximum_frames_per_second;
}

void PageClient::page_did_request_cursor_change(Gfx::Cursor const& cursor)
{
    client().async_did_request_cursor_change(m_id, cursor);
}

void PageClient::page_did_change_title(Utf16String const& title)
{
    client().async_did_change_title(m_id, title);
}

void PageClient::page_did_change_url(URL::URL const& url)
{
    client().async_did_change_url(m_id, url);
}

void PageClient::page_did_request_refresh()
{
    client().async_did_request_refresh(m_id);
}

void PageClient::page_did_request_resize_window(Gfx::IntSize size)
{
    client().async_did_request_resize_window(m_id, size);
}

void PageClient::page_did_request_reposition_window(Gfx::IntPoint position)
{
    client().async_did_request_reposition_window(m_id, position);
}

void PageClient::page_did_request_restore_window()
{
    client().async_did_request_restore_window(m_id);
}

void PageClient::page_did_request_maximize_window()
{
    client().async_did_request_maximize_window(m_id);
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

void PageClient::page_did_start_loading(URL::URL const& url, Variant<Empty, String, Web::HTML::POSTResource> document_resource, bool is_redirect, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    if (m_webdriver)
        m_webdriver->page_did_start_loading({}, url);

    client().async_did_start_loading(m_id, url, move(document_resource), is_redirect, history_handling);
}

void PageClient::page_did_cancel_loading(URL::URL const& url)
{
    if (m_webdriver)
        m_webdriver->page_did_cancel_loading({}, url);

    client().async_did_cancel_loading(m_id, url);
}

void PageClient::page_did_create_new_document(Web::DOM::Document& document)
{
    initialize_js_console(document);
}

void PageClient::page_did_change_active_document_in_top_level_browsing_context(Web::DOM::Document& document)
{
    auto& realm = document.realm();

    clear_pending_dom_mutations();
    m_web_ui.clear();

    if (auto console_client = document.console_client()) {
        auto& web_content_console_client = as<WebContentConsoleClient>(*console_client);
        m_top_level_document_console_client = web_content_console_client;

        auto console_object = realm.intrinsics().console_object();
        console_object->console().set_client(*console_client);
    }
}

void PageClient::page_did_finish_loading(URL::URL const& url)
{
    client().async_did_finish_loading(m_id, url);
}

void PageClient::wait_for_webdriver_navigation_completion(Optional<u64> page_load_timeout, Function<void(Web::WebDriver::Response)> on_complete)
{
    auto request_id = m_next_webdriver_navigation_completion_request_id++;
    m_pending_webdriver_navigation_completion_requests.set(request_id, move(on_complete));
    client().async_did_request_webdriver_navigation_completion(m_id, request_id, page_load_timeout);
}

void PageClient::did_complete_webdriver_navigation_completion(u64 request_id, Web::WebDriver::Response response)
{
    auto maybe_callback = m_pending_webdriver_navigation_completion_requests.take(request_id);
    if (!maybe_callback.has_value())
        return;

    maybe_callback.value()(move(response));
}

void PageClient::page_did_finish_test(String const& text)
{
    client().async_did_finish_test(m_id, text);
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
    auto traversable = page().top_level_traversable();
    traversable->set_pending_set_browser_zoom_request(true);
    client().async_did_set_browser_zoom(m_id, factor);
    auto& event_loop = Web::HTML::main_thread_event_loop();
    event_loop.spin_until(GC::create_function(event_loop.heap(), [this, traversable]() {
        return !traversable->pending_set_browser_zoom_request() || !is_connection_open();
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

void PageClient::page_did_request_alert(String const& message)
{
    client().async_did_request_alert(m_id, message);

    if (m_webdriver)
        m_webdriver->page_did_open_dialog({});
}

void PageClient::alert_closed()
{
    page().alert_closed();
}

void PageClient::page_did_request_confirm(String const& message)
{
    client().async_did_request_confirm(m_id, message);

    if (m_webdriver)
        m_webdriver->page_did_open_dialog({});
}

void PageClient::confirm_closed(bool accepted)
{
    page().confirm_closed(accepted);
}

void PageClient::page_did_request_prompt(String const& message, String const& default_)
{
    client().async_did_request_prompt(m_id, message, default_);

    if (m_webdriver)
        m_webdriver->page_did_open_dialog({});
}

void PageClient::page_did_request_set_prompt_text(String const& text)
{
    client().async_did_request_set_prompt_text(m_id, text);
}

void PageClient::prompt_closed(Optional<String> response)
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
    page().set_user_style(source);
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

Optional<String> PageClient::page_did_request_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key, String const& bottle_key)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidRequestStorageItem>(storage_endpoint, storage_key, bottle_key);
    if (!response) {
        dbgln("WebContent client disconnected during DidRequestStorageItem. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
    return response->take_value();
}

WebView::StorageSetResult PageClient::page_did_set_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key, String const& bottle_key, String const& value)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidSetStorageItem>(storage_endpoint, storage_key, bottle_key, value);
    if (!response) {
        dbgln("WebContent client disconnected during DidSetStorageItem. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
    return response->result();
}

void PageClient::page_did_remove_storage_item(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key, String const& bottle_key)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidRemoveStorageItem>(storage_endpoint, storage_key, bottle_key);
    if (!response) {
        dbgln("WebContent client disconnected during DidRemoveStorageItem. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
}

Vector<String> PageClient::page_did_request_storage_keys(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidRequestStorageKeys>(storage_endpoint, storage_key);
    if (!response) {
        dbgln("WebContent client disconnected during DidRequestStorageKeys. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
    return response->take_keys();
}

void PageClient::page_did_clear_storage(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& storage_key)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidClearStorage>(storage_endpoint, storage_key);
    if (!response) {
        dbgln("WebContent client disconnected during DidClearStorage. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }
}

void PageClient::page_did_broadcast_storage_change(Web::StorageAPI::StorageEndpointType storage_endpoint, String const& url, Optional<String> const& key, Optional<String> const& old_value, Optional<String> const& new_value)
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

PageClient::NewWebViewResult PageClient::page_did_request_new_web_view(Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints, Web::HTML::TokenizedFeature::NoOpener no_opener)
{
    if (no_opener == Web::HTML::TokenizedFeature::NoOpener::Yes) {
        // FIXME: Create an abstraction to let this WebContent process know about a new process we create?
        // FIXME: For now, just create a new page in the same process anyway
    }

    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidRequestNewWebView>(m_id, activate_tab, hints);
    if (!response) {
        dbgln("WebContent client disconnected during DidRequestNewWebView. Exiting peacefully.");
        Core::Process::terminate_immediately(0);
    }

    auto& new_client = m_owner.create_page(response->new_page_id());
    return { &new_client.page(), response->take_handle() };
}

void PageClient::page_did_request_activate_tab()
{
    client().async_did_request_activate_tab(m_id);
}

void PageClient::page_did_close_top_level_traversable()
{
    page().top_level_traversable()->compositor_context().stop_presenting_to_client();

    if (m_webdriver)
        m_webdriver->page_did_close_window({}, page().top_level_traversable()->window_handle());

    // FIXME: Rename this IPC call
    client().async_did_close_browsing_context(m_id);

    // NOTE: This only removes the strong reference the PageHost has for this PageClient.
    //       It will be GC'd 'later'.
    m_owner.remove_page({}, m_id);
}

void PageClient::page_did_change_needs_beforeunload_check(bool needs_beforeunload_check)
{
    client().async_did_change_needs_beforeunload_check(m_id, needs_beforeunload_check);
}

void PageClient::send_current_needs_beforeunload_check()
{
    client().async_did_change_needs_beforeunload_check(m_id, page().needs_beforeunload_check());
}

void PageClient::page_did_update_navigation_buttons_state(bool back_enabled, bool forward_enabled)
{
    client().async_did_update_navigation_buttons_state(m_id, back_enabled, forward_enabled);
}

bool PageClient::should_report_session_history_updates() const
{
    return !Web::HTML::Window::in_test_mode() || s_should_report_session_history_updates_in_test_mode;
}

void PageClient::page_did_update_session_history(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& entries, Vector<i32> const& used_steps, size_t current_used_step_index)
{
    client().async_did_update_session_history(m_id, entries, used_steps, current_used_step_index);
}

String PageClient::page_did_request_ui_process_session_history_for_testing()
{
    return client().did_request_ui_process_session_history_for_testing(m_id);
}

String PageClient::dump_site_isolation_process_tree_for_testing()
{
    return client().did_request_site_isolation_process_tree_for_testing(m_id);
}

String PageClient::page_did_update_session_history_and_request_ui_process_session_history_for_testing(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& entries, Vector<i32> const& used_steps, size_t current_used_step_index)
{
    return client().did_update_session_history_and_request_ui_process_session_history_for_testing(m_id, entries, used_steps, current_used_step_index);
}

bool PageClient::page_did_request_traverse_the_history_by_delta(int delta, Web::HistoryTraversalPrecheck history_traversal_precheck)
{
    return client().did_request_traverse_the_history_by_delta(m_id, delta, history_traversal_precheck);
}

void PageClient::request_webdriver_history_traversal(int delta, Function<void(WebDriverHistoryTraversalResult)> on_complete)
{
    auto request_id = m_next_webdriver_history_traversal_request_id++;
    m_pending_webdriver_history_traversal_requests.set(request_id, move(on_complete));
    client().async_did_request_webdriver_history_traversal(m_id, request_id, delta);
}

void PageClient::did_complete_webdriver_history_traversal(u64 request_id, bool accepted, bool will_replace_web_content_process, bool will_change_top_level_entry)
{
    auto maybe_callback = m_pending_webdriver_history_traversal_requests.take(request_id);
    if (!maybe_callback.has_value())
        return;

    maybe_callback.value()(WebDriverHistoryTraversalResult {
        .accepted = accepted,
        .will_replace_web_content_process = will_replace_web_content_process,
        .will_change_top_level_entry = will_change_top_level_entry,
    });
}

Web::WebDriver::Response PageClient::request_webdriver_load_url_from_ui(URL::URL const& url)
{
    return client().did_request_webdriver_load_url_from_ui(m_id, url);
}

Web::WebDriver::Response PageClient::request_webdriver_traverse_history_from_ui(int delta)
{
    return client().did_request_webdriver_traverse_history_from_ui(m_id, delta);
}

Web::WebDriver::Response PageClient::request_webdriver_mark_web_content_session_history_stale()
{
    return client().did_request_webdriver_mark_web_content_session_history_stale(m_id);
}

Web::WebDriver::Response PageClient::request_webdriver_session_history()
{
    return client().did_request_webdriver_session_history(m_id);
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

void PageClient::page_did_insert_clipboard_entry(Web::Clipboard::SystemClipboardRepresentation const& entry, StringView presentation_style)
{
    client().async_did_insert_clipboard_entry(m_id, entry, presentation_style);
}

void PageClient::page_did_request_clipboard_entries(u64 request_id)
{
    client().async_did_request_clipboard_entries(m_id, request_id);
}

void PageClient::page_did_request_primary_paste()
{
    client().async_did_request_primary_paste(m_id);
}

void PageClient::page_did_update_primary_selection(String const& text)
{
    client().async_did_update_primary_selection(m_id, text);
}

void PageClient::page_did_change_audio_play_state(Web::HTML::AudioPlayState play_state)
{
    client().async_did_change_audio_play_state(m_id, play_state);
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

void PageClient::page_did_mutate_dom(FlyString const& type, Web::DOM::Node const& target, Web::DOM::NodeList& added_nodes, Web::DOM::NodeList& removed_nodes, GC::Ptr<Web::DOM::Node>, GC::Ptr<Web::DOM::Node>, Optional<String> const& attribute_name)
{
    Optional<WebView::Mutation::Type> mutation;

    if (type == Web::DOM::MutationType::attributes) {
        VERIFY(attribute_name.has_value());

        auto const& element = as<Web::DOM::Element>(target);
        mutation = WebView::AttributeMutation { *attribute_name, element.attribute(*attribute_name) };
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

    auto mutation_message = WebView::Mutation { type.to_string(), target.unique_id(), {}, mutation.release_value() };
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

ErrorOr<void> PageClient::connect_to_webdriver(ByteString const& webdriver_endpoint)
{
    VERIFY(!m_webdriver);
    m_webdriver = TRY(WebDriverConnection::connect(*this, webdriver_endpoint));

    return {};
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

void PageClient::received_message_from_web_ui(String const& name, JS::Value data)
{
    if (m_web_ui)
        m_web_ui->received_message_from_web_ui(name, data);
}

void PageClient::page_did_start_network_request(u64 request_id, URL::URL const& url, ByteString const& method, Vector<HTTP::Header> const& request_headers, ReadonlyBytes request_body, Optional<String> initiator_type)
{
    client().async_did_start_network_request(m_id, request_id, url, method, request_headers, request_body, move(initiator_type));
}

void PageClient::page_did_receive_network_response_headers(u64 request_id, u32 status_code, Optional<String> reason_phrase, Vector<HTTP::Header> const& response_headers)
{
    client().async_did_receive_network_response_headers(m_id, request_id, status_code, move(reason_phrase), response_headers);
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

    for (auto& navigable : Web::HTML::all_navigables()) {
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

    for (auto& navigable : Web::HTML::all_navigables()) {
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

    auto& realm = document.realm();
    auto console_object = realm.intrinsics().console_object();

    auto console_client = DevToolsConsoleClient::create(document.realm(), console_object->console(), *this);
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
    auto script = Web::HTML::ClassicScript::create("(client connection run_javascript)", js_source, settings, move(base_url));

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
                .url = import_rule->href(),
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

    Web::CSS::StyleScope::for_each_user_agent_stylesheet(document && document->in_quirks_mode(), [&](auto&, auto const& identifier) {
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

Optional<Web::HTML::ScriptRegistry::Content> PageClient::devtools_source_content(Web::HTML::ScriptRegistry::Identifier const& source_id) const
{
    auto* node = Web::DOM::Node::from_unique_id(source_id.document_id);
    auto* document = as_if<Web::DOM::Document>(node);
    if (!document)
        return {};

    return document->script_registry().script_content(source_id.script_id, document->source());
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
    page().top_level_traversable()->queue_screenshot_task(node_id);
}

}
