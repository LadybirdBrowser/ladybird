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
#include <LibCore/Timer.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibJS/Console.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/Cookie/ParsedCookie.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/MutationType.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/HTML/Scripting/ClassicScript.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWebView/SiteIsolation.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/DevToolsConsoleClient.h>
#include <WebContent/PageClient.h>
#include <WebContent/PageHost.h>
#include <WebContent/WebContentClientEndpoint.h>
#include <WebContent/WebDriverConnection.h>
#include <WebContent/WebUIConnection.h>

namespace WebContent {

static PageClient::UseSkiaPainter s_use_skia_painter = PageClient::UseSkiaPainter::GPUBackendIfAvailable;
static bool s_is_headless { false };

GC_DEFINE_ALLOCATOR(PageClient);

void PageClient::set_use_skia_painter(UseSkiaPainter use_skia_painter)
{
    s_use_skia_painter = use_skia_painter;
}

bool PageClient::is_headless() const
{
    return s_is_headless;
}

void PageClient::set_is_headless(bool is_headless)
{
    s_is_headless = is_headless;
}

GC::Ref<PageClient> PageClient::create(JS::VM& vm, PageHost& page_host, u64 id)
{
    return vm.heap().allocate<PageClient>(page_host, id);
}

PageClient::PageClient(PageHost& owner, u64 id)
    : m_owner(owner)
    , m_page(Web::Page::create(Web::Bindings::main_thread_vm(), *this))
    , m_id(id)
    , m_backing_store_manager(*this)
{
    setup_palette();

    int refresh_interval = 1000 / 60; // FIXME: Account for the actual refresh rate of the display
    m_paint_refresh_timer = Core::Timer::create_repeating(refresh_interval, [] {
        Web::HTML::main_thread_event_loop().queue_task_to_update_the_rendering();
    });

    m_paint_refresh_timer->start();
}

PageClient::~PageClient() = default;

bool PageClient::is_ready_to_paint() const
{
    return m_paint_state == PaintState::Ready;
}

void PageClient::ready_to_paint()
{
    m_paint_state = PaintState::Ready;
}

void PageClient::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);

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
    m_has_focus = has_focus;
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

bool PageClient::is_url_suitable_for_same_process_navigation(URL::URL const& current_url, URL::URL const& target_url) const
{
    return WebView::is_url_suitable_for_same_process_navigation(current_url, target_url);
}

void PageClient::request_new_process_for_navigation(URL::URL const& url)
{
    client().async_did_request_new_process_for_navigation(m_id, url);
}

Gfx::Palette PageClient::palette() const
{
    return Gfx::Palette(*m_palette_impl);
}

void PageClient::set_palette_impl(Gfx::PaletteImpl& impl)
{
    m_palette_impl = impl;
    if (auto* document = page().top_level_browsing_context().active_document())
        document->invalidate_style(Web::DOM::StyleInvalidationReason::SettingsChange);
}

void PageClient::set_preferred_color_scheme(Web::CSS::PreferredColorScheme color_scheme)
{
    m_preferred_color_scheme = color_scheme;
    if (auto* document = page().top_level_browsing_context().active_document())
        document->invalidate_style(Web::DOM::StyleInvalidationReason::SettingsChange);
}

void PageClient::set_preferred_contrast(Web::CSS::PreferredContrast contrast)
{
    m_preferred_contrast = contrast;
    if (auto* document = page().top_level_browsing_context().active_document())
        document->invalidate_style(Web::DOM::StyleInvalidationReason::SettingsChange);
}

void PageClient::set_preferred_motion(Web::CSS::PreferredMotion motion)
{
    m_preferred_motion = motion;
    if (auto* document = page().top_level_browsing_context().active_document())
        document->invalidate_style(Web::DOM::StyleInvalidationReason::SettingsChange);
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

Web::Layout::Viewport* PageClient::layout_root()
{
    auto* document = page().top_level_browsing_context().active_document();
    if (!document)
        return nullptr;
    return document->layout_node();
}

void PageClient::process_screenshot_requests()
{
    while (!m_screenshot_tasks.is_empty()) {
        auto task = m_screenshot_tasks.dequeue();
        if (task.node_id.has_value()) {
            auto* dom_node = Web::DOM::Node::from_unique_id(*task.node_id);
            if (!dom_node || !dom_node->paintable_box()) {
                client().async_did_take_screenshot(m_id, {});
                continue;
            }
            auto rect = page().enclosing_device_rect(dom_node->paintable_box()->absolute_border_box_rect());
            auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, rect.size().to_type<int>()).release_value_but_fixme_should_propagate_errors();
            auto backing_store = Web::Painting::BitmapBackingStore(*bitmap);
            paint(rect, backing_store, { .paint_overlay = Web::PaintOptions::PaintOverlay::No });
            client().async_did_take_screenshot(m_id, bitmap->to_shareable_bitmap());
        } else {
            Web::DevicePixelRect rect { { 0, 0 }, content_size() };
            auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, rect.size().to_type<int>()).release_value_but_fixme_should_propagate_errors();
            auto backing_store = Web::Painting::BitmapBackingStore(*bitmap);
            paint(rect, backing_store);
            client().async_did_take_screenshot(m_id, bitmap->to_shareable_bitmap());
        }
    }
}

void PageClient::paint_next_frame()
{
    auto back_store = m_backing_store_manager.back_store();
    if (!back_store)
        return;

    auto viewport_rect = page().css_to_device_rect(page().top_level_traversable()->viewport_rect());
    paint(viewport_rect, *back_store);

    m_backing_store_manager.swap_back_and_front();

    m_paint_state = PaintState::WaitingForClient;
    client().async_did_paint(m_id, viewport_rect.to_type<int>(), m_backing_store_manager.front_id());
}

void PageClient::paint(Web::DevicePixelRect const& content_rect, Web::Painting::BackingStore& target, Web::PaintOptions paint_options)
{
    paint_options.should_show_line_box_borders = m_should_show_line_box_borders;
    paint_options.has_focus = m_has_focus;
    page().top_level_traversable()->paint(content_rect, target, paint_options);
}

Queue<Web::QueuedInputEvent>& PageClient::input_event_queue()
{
    return client().input_event_queue();
}

void PageClient::report_finished_handling_input_event(u64 page_id, Web::EventResult event_was_handled)
{
    client().async_did_finish_handling_input_event(page_id, event_was_handled);
}

void PageClient::set_viewport_size(Web::DevicePixelSize const& size)
{
    page().top_level_traversable()->set_viewport_size(page().device_to_css_size(size));

    m_backing_store_manager.restart_resize_timer();
    m_backing_store_manager.resize_backing_stores_if_needed(BackingStoreManager::WindowResizingInProgress::Yes);
    m_pending_set_browser_zoom_request = false;
}

void PageClient::page_did_request_cursor_change(Gfx::Cursor const& cursor)
{
    client().async_did_request_cursor_change(m_id, cursor);
}

void PageClient::page_did_layout()
{
    auto* layout_root = this->layout_root();
    VERIFY(layout_root);

    if (layout_root->paintable_box()->has_scrollable_overflow())
        m_content_size = page().enclosing_device_rect(layout_root->paintable_box()->scrollable_overflow_rect().value()).size();
    else
        m_content_size = page().enclosing_device_rect(layout_root->paintable_box()->absolute_rect()).size();
}

void PageClient::page_did_change_title(ByteString const& title)
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

void PageClient::page_did_start_loading(URL::URL const& url, bool is_redirect)
{
    client().async_did_start_loading(m_id, url, is_redirect);
}

void PageClient::page_did_create_new_document(Web::DOM::Document& document)
{
    initialize_js_console(document);
}

void PageClient::page_did_change_active_document_in_top_level_browsing_context(Web::DOM::Document& document)
{
    auto& realm = document.realm();

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

void PageClient::page_did_finish_test(String const& text)
{
    client().async_did_finish_test(m_id, text);
}

void PageClient::page_did_set_test_timeout(double milliseconds)
{
    client().async_did_set_test_timeout(m_id, milliseconds);
}

void PageClient::page_did_set_browser_zoom(double factor)
{
    m_pending_set_browser_zoom_request = true;
    client().async_did_set_browser_zoom(m_id, factor);

    auto& event_loop = Web::HTML::main_thread_event_loop();
    event_loop.spin_until(GC::create_function(event_loop.heap(), [&]() {
        return !m_pending_set_browser_zoom_request || !is_connection_open();
    }));
}

void PageClient::page_did_request_context_menu(Web::CSSPixelPoint content_position)
{
    client().async_did_request_context_menu(m_id, page().css_to_device_point(content_position).to_type<int>());
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

Web::WebIDL::ExceptionOr<void> PageClient::toggle_media_play_state()
{
    return page().toggle_media_play_state();
}

void PageClient::toggle_media_mute_state()
{
    page().toggle_media_mute_state();
}

Web::WebIDL::ExceptionOr<void> PageClient::toggle_media_loop_state()
{
    return page().toggle_media_loop_state();
}

Web::WebIDL::ExceptionOr<void> PageClient::toggle_media_controls_state()
{
    return page().toggle_media_controls_state();
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

Vector<Web::Cookie::Cookie> PageClient::page_did_request_all_cookies(URL::URL const& url)
{
    return client().did_request_all_cookies(url);
}

Optional<Web::Cookie::Cookie> PageClient::page_did_request_named_cookie(URL::URL const& url, String const& name)
{
    return client().did_request_named_cookie(url, name);
}

String PageClient::page_did_request_cookie(URL::URL const& url, Web::Cookie::Source source)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidRequestCookie>(url, source);
    if (!response) {
        dbgln("WebContent client disconnected during DidRequestCookie. Exiting peacefully.");
        exit(0);
    }
    return response->take_cookie();
}

void PageClient::page_did_set_cookie(URL::URL const& url, Web::Cookie::ParsedCookie const& cookie, Web::Cookie::Source source)
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidSetCookie>(url, cookie, source);
    if (!response) {
        dbgln("WebContent client disconnected during DidSetCookie. Exiting peacefully.");
        exit(0);
    }
}

void PageClient::page_did_update_cookie(Web::Cookie::Cookie const& cookie)
{
    client().async_did_update_cookie(cookie);
}

void PageClient::page_did_expire_cookies_with_time_offset(AK::Duration offset)
{
    client().async_did_expire_cookies_with_time_offset(offset);
}

void PageClient::page_did_update_resource_count(i32 count_waiting)
{
    client().async_did_update_resource_count(m_id, count_waiting);
}

PageClient::NewWebViewResult PageClient::page_did_request_new_web_view(Web::HTML::ActivateTab activate_tab, Web::HTML::WebViewHints hints, Web::HTML::TokenizedFeature::NoOpener no_opener)
{
    auto& new_client = m_owner.create_page();

    Optional<u64> page_id;
    if (no_opener == Web::HTML::TokenizedFeature::NoOpener::Yes) {
        // FIXME: Create an abstraction to let this WebContent process know about a new process we create?
        // FIXME: For now, just create a new page in the same process anyway
    }

    page_id = new_client.m_id;

    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::DidRequestNewWebView>(m_id, activate_tab, hints, page_id);
    if (!response) {
        dbgln("WebContent client disconnected during DidRequestNewWebView. Exiting peacefully.");
        exit(0);
    }

    return { &new_client.page(), response->take_handle() };
}

void PageClient::page_did_request_activate_tab()
{
    client().async_did_request_activate_tab(m_id);
}

void PageClient::page_did_close_top_level_traversable()
{
    // FIXME: Rename this IPC call
    client().async_did_close_browsing_context(m_id);

    // NOTE: This only removes the strong reference the PageHost has for this PageClient.
    //       It will be GC'd 'later'.
    m_owner.remove_page({}, m_id);
}

void PageClient::page_did_update_navigation_buttons_state(bool back_enabled, bool forward_enabled)
{
    client().async_did_update_navigation_buttons_state(m_id, back_enabled, forward_enabled);
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

void PageClient::page_did_insert_clipboard_entry(StringView data, StringView presentation_style, StringView mime_type)
{
    client().async_did_insert_clipboard_entry(m_id, data, presentation_style, mime_type);
}

void PageClient::page_did_change_audio_play_state(Web::HTML::AudioPlayState play_state)
{
    client().async_did_change_audio_play_state(m_id, play_state);
}

void PageClient::page_did_allocate_backing_stores(i32 front_bitmap_id, Gfx::ShareableBitmap front_bitmap, i32 back_bitmap_id, Gfx::ShareableBitmap back_bitmap)
{
    client().async_did_allocate_backing_stores(m_id, front_bitmap_id, front_bitmap, back_bitmap_id, back_bitmap);
}

IPC::File PageClient::request_worker_agent()
{
    auto response = client().send_sync_but_allow_failure<Messages::WebContentClient::RequestWorkerAgent>(m_id);
    if (!response) {
        dbgln("WebContent client disconnected during RequestWorkerAgent. Exiting peacefully.");
        exit(0);
    }

    return response->take_socket();
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
        mutation = WebView::CharacterDataMutation { character_data.data() };
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

    StringBuilder builder;
    auto serializer = MUST(JsonObjectSerializer<>::try_create(builder));
    target.serialize_tree_as_json(serializer);
    MUST(serializer.finish());
    auto serialized_target = MUST(builder.to_string());

    client().async_did_mutate_dom(m_id, { type.to_string(), target.unique_id(), move(serialized_target), mutation.release_value() });
}

ErrorOr<void> PageClient::connect_to_webdriver(ByteString const& webdriver_ipc_path)
{
    VERIFY(!m_webdriver);
    m_webdriver = TRY(WebDriverConnection::connect(*this, webdriver_ipc_path));

    return {};
}

ErrorOr<void> PageClient::connect_to_web_ui(IPC::File web_ui_socket)
{
    auto* active_document = page().top_level_browsing_context().active_document();
    if (!active_document || !active_document->window())
        return {};

    VERIFY(!m_web_ui);
    m_web_ui = TRY(WebUIConnection::connect(move(web_ui_socket), *active_document));

    return {};
}

void PageClient::received_message_from_web_ui(String const& name, JS::Value data)
{
    if (m_web_ui)
        m_web_ui->received_message_from_web_ui(name, data);
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

    // Let script be the result of creating a classic script given scriptSource, setting's realm, baseURL, and the default classic script fetch options.
    // FIXME: This doesn't pass in "default classic script fetch options"
    // FIXME: What should the filename be here?
    auto script = Web::HTML::ClassicScript::create("(client connection run_javascript)", js_source, settings.realm(), move(base_url));

    // Let evaluationStatus be the result of running the classic script script.
    auto evaluation_status = script->run();

    if (evaluation_status.is_error())
        dbgln("Exception :(");
}

void PageClient::js_console_request_messages(i32 start_index)
{
    if (m_top_level_document_console_client)
        m_top_level_document_console_client->send_messages(start_index);
}

void PageClient::did_output_js_console_message(i32 message_index)
{
    client().async_did_output_js_console_message(m_id, message_index);
}

void PageClient::console_peer_did_misbehave(char const* reason)
{
    client().did_misbehave(reason);
}

void PageClient::did_get_js_console_messages(i32 start_index, ReadonlySpan<WebView::ConsoleOutput> console_output)
{
    client().async_did_get_js_console_messages(m_id, start_index, console_output);
}

static void gather_style_sheets(Vector<Web::CSS::StyleSheetIdentifier>& results, Web::CSS::CSSStyleSheet& sheet)
{
    Web::CSS::StyleSheetIdentifier identifier {};

    bool valid = true;

    if (sheet.owner_rule()) {
        identifier.type = Web::CSS::StyleSheetIdentifier::Type::ImportRule;
    } else if (auto* node = sheet.owner_node()) {
        if (node->is_html_style_element() || node->is_svg_style_element()) {
            identifier.type = Web::CSS::StyleSheetIdentifier::Type::StyleElement;
        } else if (is<Web::HTML::HTMLLinkElement>(node)) {
            identifier.type = Web::CSS::StyleSheetIdentifier::Type::LinkElement;
        } else {
            dbgln("Can't identify where style sheet came from; owner node is {}", node->debug_description());
            identifier.type = Web::CSS::StyleSheetIdentifier::Type::StyleElement;
        }
        identifier.dom_element_unique_id = node->unique_id();
    } else {
        dbgln("Style sheet has no owner rule or owner node; skipping");
        valid = false;
    }

    if (valid) {
        if (auto location = sheet.location(); location.has_value())
            identifier.url = location.release_value();

        identifier.rule_count = sheet.rules().length();
        results.append(move(identifier));
    }

    for (auto& import_rule : sheet.import_rules()) {
        if (import_rule->loaded_style_sheet()) {
            gather_style_sheets(results, *import_rule->loaded_style_sheet());
        } else {
            // We can gather this anyway, and hope it loads later
            results.append({
                .type = Web::CSS::StyleSheetIdentifier::Type::ImportRule,
                .url = import_rule->url().to_string(),
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

    // User-agent
    results.append({
        .type = Web::CSS::StyleSheetIdentifier::Type::UserAgent,
        .url = "CSS/Default.css"_string,
    });
    if (document && document->in_quirks_mode()) {
        results.append({
            .type = Web::CSS::StyleSheetIdentifier::Type::UserAgent,
            .url = "CSS/QuirksMode.css"_string,
        });
    }
    results.append({
        .type = Web::CSS::StyleSheetIdentifier::Type::UserAgent,
        .url = "MathML/Default.css"_string,
    });
    results.append({
        .type = Web::CSS::StyleSheetIdentifier::Type::UserAgent,
        .url = "SVG/Default.css"_string,
    });

    return results;
}

Web::DisplayListPlayerType PageClient::display_list_player_type() const
{
    switch (s_use_skia_painter) {
    case UseSkiaPainter::GPUBackendIfAvailable:
        return Web::DisplayListPlayerType::SkiaGPUIfAvailable;
    case UseSkiaPainter::CPUBackend:
        return Web::DisplayListPlayerType::SkiaCPU;
    default:
        VERIFY_NOT_REACHED();
    }
}

void PageClient::queue_screenshot_task(Optional<Web::UniqueNodeID> node_id)
{
    m_screenshot_tasks.enqueue({ node_id });
    page().top_level_traversable()->set_needs_repaint();
}

}
