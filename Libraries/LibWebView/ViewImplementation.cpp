/*
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/String.h>
#include <AK/TemporaryChange.h>
#include <AK/Time.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibURL/Parser.h>
#include <LibWeb/Clipboard/SystemClipboard.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWebView/Application.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/Menu.h>
#include <LibWebView/SiteIsolation.h>
#include <LibWebView/URL.h>
#include <LibWebView/UserAgent.h>
#include <LibWebView/ViewImplementation.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#    include <LibCore/MachPort.h>
#endif

namespace WebView {

static HashMap<u64, ViewImplementation*> s_all_views;
static u64 s_view_count = 1; // This has to start at 1 for Firefox DevTools.

void ViewImplementation::for_each_view(Function<IterationDecision(ViewImplementation&)> callback)
{
    for (auto& view : s_all_views) {
        if (callback(*view.value) == IterationDecision::Break)
            break;
    }
}

Optional<ViewImplementation&> ViewImplementation::find_view_by_id(u64 id)
{
    if (auto view = s_all_views.get(id); view.has_value())
        return *view.value();
    return {};
}

ViewImplementation::ViewImplementation()
    : m_view_id(s_view_count++)
{
    s_all_views.set(m_view_id, this);

    initialize_context_menus();

    m_repeated_crash_timer = Core::Timer::create_single_shot(1000, [this] {
        // Reset the "crashing a lot" counter after 1 second in case we just
        // happen to be visiting crashy websites a lot.
        this->m_crash_count = 0;
    });

    on_request_file = [this](auto const& path, auto request_id) {
        auto file = Core::File::open(path, Core::File::OpenMode::Read);

        if (file.is_error())
            client().async_handle_file_return(page_id(), file.error().code(), {}, request_id);
        else
            client().async_handle_file_return(page_id(), 0, IPC::File::adopt_file(file.release_value()), request_id);
    };
}

ViewImplementation::~ViewImplementation()
{
    s_all_views.remove(m_view_id);

    if (m_client_state.client)
        m_client_state.client->unregister_view(m_client_state.page_index);
}

WebContentClient& ViewImplementation::client()
{
    VERIFY(m_client_state.client);
    return *m_client_state.client;
}

WebContentClient const& ViewImplementation::client() const
{
    VERIFY(m_client_state.client);
    return *m_client_state.client;
}

u64 ViewImplementation::page_id() const
{
    VERIFY(m_client_state.client);
    return m_client_state.page_index;
}

void ViewImplementation::create_new_process_for_cross_site_navigation(URL::URL const& url)
{
    if (m_client_state.client)
        client().async_close_server();

    initialize_client();
    VERIFY(m_client_state.client);

    // Don't keep a stale backup bitmap around.
    m_backup_bitmap = nullptr;
    handle_resize();

    load(url);
}

void ViewImplementation::server_did_paint(Badge<WebContentClient>, i32 bitmap_id, Gfx::IntSize size)
{
    if (m_client_state.back_bitmap.id == bitmap_id) {
        m_client_state.has_usable_bitmap = true;
        m_client_state.back_bitmap.last_painted_size = size.to_type<Web::DevicePixels>();
        swap(m_client_state.back_bitmap, m_client_state.front_bitmap);
        m_backup_bitmap = nullptr;
        if (on_ready_to_paint)
            on_ready_to_paint();
    }

    client().async_ready_to_paint(page_id());
}

void ViewImplementation::set_window_position(Gfx::IntPoint position)
{
    client().async_set_window_position(m_client_state.page_index, position.to_type<Web::DevicePixels>());
}

void ViewImplementation::set_window_size(Gfx::IntSize size)
{
    client().async_set_window_size(m_client_state.page_index, size.to_type<Web::DevicePixels>());
}

void ViewImplementation::did_update_window_rect()
{
    client().async_did_update_window_rect(m_client_state.page_index);
}

void ViewImplementation::set_system_visibility_state(Web::HTML::VisibilityState visibility_state)
{
    m_system_visibility_state = visibility_state;
    client().async_set_system_visibility_state(m_client_state.page_index, m_system_visibility_state);
}

void ViewImplementation::load(URL::URL const& url)
{
    m_url = url;
    client().async_load_url(page_id(), url);
}

void ViewImplementation::load_html(StringView html)
{
    client().async_load_html(page_id(), html);
}

void ViewImplementation::reload()
{
    client().async_reload(page_id());
}

void ViewImplementation::traverse_the_history_by_delta(int delta)
{
    client().async_traverse_the_history_by_delta(page_id(), delta);
}

void ViewImplementation::zoom_in()
{
    if (m_zoom_level >= ZOOM_MAX_LEVEL)
        return;
    m_zoom_level = round_to<int>((m_zoom_level + ZOOM_STEP) * 100) / 100.0f;
    update_zoom();
}

void ViewImplementation::zoom_out()
{
    if (m_zoom_level <= ZOOM_MIN_LEVEL)
        return;
    m_zoom_level = round_to<int>((m_zoom_level - ZOOM_STEP) * 100) / 100.0f;
    update_zoom();
}

void ViewImplementation::set_zoom(double zoom_level)
{
    m_zoom_level = max(ZOOM_MIN_LEVEL, min(zoom_level, ZOOM_MAX_LEVEL));
    update_zoom();
}

void ViewImplementation::reset_zoom()
{
    m_zoom_level = 1.0f;
    update_zoom();
}

void ViewImplementation::enqueue_input_event(Web::InputEvent event)
{
    // Send the next event over to the WebContent to be handled by JS. We'll later get a message to say whether JS
    // prevented the default event behavior, at which point we either discard or handle that event, and then try to
    // process the next one.
    m_pending_input_events.enqueue(move(event));

    m_pending_input_events.tail().visit(
        [this](Web::KeyEvent const& event) {
            client().async_key_event(m_client_state.page_index, event.clone_without_browser_data());
        },
        [this](Web::MouseEvent const& event) {
            client().async_mouse_event(m_client_state.page_index, event.clone_without_browser_data());
        },
        [this](Web::DragEvent& event) {
            auto cloned_event = event.clone_without_browser_data();
            cloned_event.files = move(event.files);

            client().async_drag_event(m_client_state.page_index, cloned_event);
        });
}

void ViewImplementation::did_finish_handling_input_event(Badge<WebContentClient>, Web::EventResult event_result)
{
    auto event = m_pending_input_events.dequeue();

    if (event_result == Web::EventResult::Handled)
        return;

    // Here we handle events that were not consumed or cancelled by the WebContent. Propagate the event back
    // to the concrete view implementation.
    event.visit(
        [this](Web::KeyEvent const& event) {
            if (on_finish_handling_key_event)
                on_finish_handling_key_event(event);
        },
        [this](Web::DragEvent const& event) {
            if (on_finish_handling_drag_event)
                on_finish_handling_drag_event(event);
        },
        [](auto const&) {});
}

void ViewImplementation::set_preferred_color_scheme(Web::CSS::PreferredColorScheme color_scheme)
{
    client().async_set_preferred_color_scheme(page_id(), color_scheme);
}

void ViewImplementation::set_preferred_contrast(Web::CSS::PreferredContrast contrast)
{
    client().async_set_preferred_contrast(page_id(), contrast);
}

void ViewImplementation::set_preferred_motion(Web::CSS::PreferredMotion motion)
{
    client().async_set_preferred_motion(page_id(), motion);
}

ByteString ViewImplementation::selected_text()
{
    return client().get_selected_text(page_id());
}

Optional<String> ViewImplementation::selected_text_with_whitespace_collapsed()
{
    auto selected_text = MUST(Web::Infra::strip_and_collapse_whitespace(this->selected_text()));
    if (selected_text.is_empty())
        return OptionalNone {};
    return selected_text;
}

void ViewImplementation::select_all()
{
    client().async_select_all(page_id());
}

void ViewImplementation::find_in_page(String const& query, CaseSensitivity case_sensitivity)
{
    client().async_find_in_page(page_id(), query, case_sensitivity);
}

void ViewImplementation::find_in_page_next_match()
{
    client().async_find_in_page_next_match(page_id());
}

void ViewImplementation::find_in_page_previous_match()
{
    client().async_find_in_page_previous_match(page_id());
}

void ViewImplementation::get_source()
{
    client().async_get_source(page_id());
}

void ViewImplementation::inspect_dom_tree()
{
    client().async_inspect_dom_tree(page_id());
}

void ViewImplementation::inspect_accessibility_tree()
{
    client().async_inspect_accessibility_tree(page_id());
}

void ViewImplementation::get_hovered_node_id()
{
    client().async_get_hovered_node_id(page_id());
}

void ViewImplementation::inspect_dom_node(Web::UniqueNodeID node_id, DOMNodeProperties::Type property_type, Optional<Web::CSS::PseudoElement> pseudo_element)
{
    client().async_inspect_dom_node(page_id(), property_type, node_id, pseudo_element);
}

void ViewImplementation::clear_inspected_dom_node()
{
    client().async_clear_inspected_dom_node(page_id());
}

void ViewImplementation::highlight_dom_node(Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element)
{
    client().async_highlight_dom_node(page_id(), node_id, pseudo_element);
}

void ViewImplementation::clear_highlighted_dom_node()
{
    highlight_dom_node(0, {});
}

void ViewImplementation::set_listen_for_dom_mutations(bool listen_for_dom_mutations)
{
    client().async_set_listen_for_dom_mutations(page_id(), listen_for_dom_mutations);
}

void ViewImplementation::get_dom_node_inner_html(Web::UniqueNodeID node_id)
{
    client().async_get_dom_node_inner_html(page_id(), node_id);
}

void ViewImplementation::get_dom_node_outer_html(Web::UniqueNodeID node_id)
{
    client().async_get_dom_node_outer_html(page_id(), node_id);
}

void ViewImplementation::set_dom_node_outer_html(Web::UniqueNodeID node_id, String const& html)
{
    client().async_set_dom_node_outer_html(page_id(), node_id, html);
}

void ViewImplementation::set_dom_node_text(Web::UniqueNodeID node_id, String const& text)
{
    client().async_set_dom_node_text(page_id(), node_id, text);
}

void ViewImplementation::set_dom_node_tag(Web::UniqueNodeID node_id, String const& name)
{
    client().async_set_dom_node_tag(page_id(), node_id, name);
}

void ViewImplementation::add_dom_node_attributes(Web::UniqueNodeID node_id, ReadonlySpan<Attribute> attributes)
{
    client().async_add_dom_node_attributes(page_id(), node_id, attributes);
}

void ViewImplementation::replace_dom_node_attribute(Web::UniqueNodeID node_id, String const& name, ReadonlySpan<Attribute> replacement_attributes)
{
    client().async_replace_dom_node_attribute(page_id(), node_id, name, replacement_attributes);
}

void ViewImplementation::create_child_element(Web::UniqueNodeID node_id)
{
    client().async_create_child_element(page_id(), node_id);
}

void ViewImplementation::create_child_text_node(Web::UniqueNodeID node_id)
{
    client().async_create_child_text_node(page_id(), node_id);
}

void ViewImplementation::insert_dom_node_before(Web::UniqueNodeID node_id, Web::UniqueNodeID parent_node_id, Optional<Web::UniqueNodeID> sibling_node_id)
{
    client().async_insert_dom_node_before(page_id(), node_id, parent_node_id, sibling_node_id);
}

void ViewImplementation::clone_dom_node(Web::UniqueNodeID node_id)
{
    client().async_clone_dom_node(page_id(), node_id);
}

void ViewImplementation::remove_dom_node(Web::UniqueNodeID node_id)
{
    client().async_remove_dom_node(page_id(), node_id);
}

void ViewImplementation::list_style_sheets()
{
    client().async_list_style_sheets(page_id());
}

void ViewImplementation::request_style_sheet_source(Web::CSS::StyleSheetIdentifier const& identifier)
{
    client().async_request_style_sheet_source(page_id(), identifier);
}

void ViewImplementation::debug_request(ByteString const& request, ByteString const& argument)
{
    client().async_debug_request(page_id(), request, argument);
}

void ViewImplementation::run_javascript(String const& js_source)
{
    client().async_run_javascript(page_id(), js_source);
}

void ViewImplementation::js_console_input(String const& js_source)
{
    client().async_js_console_input(page_id(), js_source);
}

void ViewImplementation::js_console_request_messages(i32 start_index)
{
    client().async_js_console_request_messages(page_id(), start_index);
}

void ViewImplementation::alert_closed()
{
    client().async_alert_closed(page_id());
}

void ViewImplementation::confirm_closed(bool accepted)
{
    client().async_confirm_closed(page_id(), accepted);
}

void ViewImplementation::prompt_closed(Optional<String> const& response)
{
    client().async_prompt_closed(page_id(), response);
}

void ViewImplementation::color_picker_update(Optional<Color> picked_color, Web::HTML::ColorPickerUpdateState state)
{
    client().async_color_picker_update(page_id(), picked_color, state);
}

void ViewImplementation::file_picker_closed(Vector<Web::HTML::SelectedFile> selected_files)
{
    client().async_file_picker_closed(page_id(), move(selected_files));
}

void ViewImplementation::select_dropdown_closed(Optional<u32> const& selected_item_id)
{
    client().async_select_dropdown_closed(page_id(), selected_item_id);
}

void ViewImplementation::insert_text_into_clipboard(ByteString text) const
{
    if (on_insert_clipboard_entry)
        on_insert_clipboard_entry({ move(text), "text/plain"_string }, {});
}

void ViewImplementation::paste_text_from_clipboard()
{
    if (on_request_clipboard_text)
        client().async_paste(page_id(), on_request_clipboard_text());
}

void ViewImplementation::retrieved_clipboard_entries(u64 request_id, ReadonlySpan<Web::Clipboard::SystemClipboardItem> items)
{
    client().async_retrieved_clipboard_entries(page_id(), request_id, items);
}

void ViewImplementation::toggle_page_mute_state()
{
    m_mute_state = Web::HTML::invert_mute_state(m_mute_state);
    client().async_toggle_page_mute_state(page_id());
}

void ViewImplementation::did_change_audio_play_state(Badge<WebContentClient>, Web::HTML::AudioPlayState play_state)
{
    bool state_changed = false;

    switch (play_state) {
    case Web::HTML::AudioPlayState::Paused:
        if (--m_number_of_elements_playing_audio == 0) {
            m_audio_play_state = play_state;
            state_changed = true;
        }
        break;

    case Web::HTML::AudioPlayState::Playing:
        if (m_number_of_elements_playing_audio++ == 0) {
            m_audio_play_state = play_state;
            state_changed = true;
        }
        break;
    }

    if (state_changed && on_audio_play_state_changed)
        on_audio_play_state_changed(m_audio_play_state);
}

void ViewImplementation::did_update_navigation_buttons_state(Badge<WebContentClient>, bool back_enabled, bool forward_enabled) const
{
    m_navigate_back_action->set_enabled(back_enabled);
    m_navigate_forward_action->set_enabled(forward_enabled);
}

void ViewImplementation::did_allocate_backing_stores(Badge<WebContentClient>, i32 front_bitmap_id, Gfx::ShareableBitmap const& front_bitmap, i32 back_bitmap_id, Gfx::ShareableBitmap const& back_bitmap)
{
    if (m_client_state.has_usable_bitmap) {
        // NOTE: We keep the outgoing front bitmap as a backup so we have something to paint until we get a new one.
        m_backup_bitmap = m_client_state.front_bitmap.bitmap;
        m_backup_bitmap_size = m_client_state.front_bitmap.last_painted_size;
    }
    m_client_state.has_usable_bitmap = false;

    m_client_state.front_bitmap.bitmap = front_bitmap.bitmap();
    m_client_state.front_bitmap.id = front_bitmap_id;
    m_client_state.back_bitmap.bitmap = back_bitmap.bitmap();
    m_client_state.back_bitmap.id = back_bitmap_id;
}

#ifdef AK_OS_MACOS
void ViewImplementation::did_allocate_iosurface_backing_stores(i32 front_id, Core::MachPort&& front_port, i32 back_id, Core::MachPort&& back_port)
{
    if (m_client_state.has_usable_bitmap) {
        // NOTE: We keep the outgoing front bitmap as a backup so we have something to paint until we get a new one.
        m_backup_bitmap = m_client_state.front_bitmap.bitmap;
        m_backup_bitmap_size = m_client_state.front_bitmap.last_painted_size;
    }
    m_client_state.has_usable_bitmap = false;

    auto front_iosurface = Core::IOSurfaceHandle::from_mach_port(move(front_port));
    auto back_iosurface = Core::IOSurfaceHandle::from_mach_port(move(back_port));

    auto front_size = Gfx::IntSize { front_iosurface.width(), front_iosurface.height() };
    auto back_size = Gfx::IntSize { back_iosurface.width(), back_iosurface.height() };

    auto bytes_per_row = front_iosurface.bytes_per_row();

    auto front_bitmap = Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, front_size, bytes_per_row, front_iosurface.data(), [handle = move(front_iosurface)] { });
    auto back_bitmap = Gfx::Bitmap::create_wrapper(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, back_size, bytes_per_row, back_iosurface.data(), [handle = move(back_iosurface)] { });

    m_client_state.front_bitmap.bitmap = front_bitmap.release_value_but_fixme_should_propagate_errors();
    m_client_state.front_bitmap.id = front_id;
    m_client_state.back_bitmap.bitmap = back_bitmap.release_value_but_fixme_should_propagate_errors();
    m_client_state.back_bitmap.id = back_id;
}
#endif

void ViewImplementation::update_zoom()
{
    if (m_zoom_level != 1.0f) {
        m_reset_zoom_action->set_text(MUST(String::formatted("{}%", round_to<int>(m_zoom_level * 100))));
        m_reset_zoom_action->set_visible(true);
    } else {
        m_reset_zoom_action->set_visible(false);
    }

    client().async_set_device_pixels_per_css_pixel(m_client_state.page_index, m_device_pixel_ratio * m_zoom_level);
}

void ViewImplementation::handle_resize()
{
    client().async_set_viewport_size(page_id(), this->viewport_size());
}

void ViewImplementation::initialize_client(CreateNewClient create_new_client)
{
    if (create_new_client == CreateNewClient::Yes) {
        m_client_state = {};

        // FIXME: Fail to open the tab, rather than crashing the whole application if this fails.
        m_client_state.client = Application::the().launch_web_content_process(*this).release_value_but_fixme_should_propagate_errors();
    } else {
        m_client_state.client->register_view(m_client_state.page_index, *this);
    }

    m_client_state.client->on_web_content_process_crash = [this] {
        Core::deferred_invoke([this] {
            handle_web_content_process_crash();

            if (on_web_content_crashed)
                on_web_content_crashed();
        });
    };

    m_client_state.client_handle = MUST(Web::Crypto::generate_random_uuid());
    client().async_set_window_handle(m_client_state.page_index, m_client_state.client_handle);

    client().async_set_device_pixels_per_css_pixel(m_client_state.page_index, m_device_pixel_ratio);
    client().async_set_maximum_frames_per_second(m_client_state.page_index, m_maximum_frames_per_second);
    client().async_set_system_visibility_state(m_client_state.page_index, m_system_visibility_state);

    if (auto webdriver_content_ipc_path = Application::browser_options().webdriver_content_ipc_path; webdriver_content_ipc_path.has_value())
        client().async_connect_to_webdriver(m_client_state.page_index, *webdriver_content_ipc_path);

    Application::the().apply_view_options({}, *this);

    default_zoom_level_factor_changed();
    languages_changed();
    autoplay_settings_changed();
    global_privacy_control_changed();
}

void ViewImplementation::handle_web_content_process_crash(LoadErrorPage load_error_page)
{
    dbgln("\033[31;1mWebContent process crashed!\033[0m Last page loaded: {}", m_url);
    dbgln("Consider raising an issue at https://github.com/LadybirdBrowser/ladybird/issues/new/choose");

    ++m_crash_count;
    constexpr size_t max_reasonable_crash_count = 5U;
    if (m_crash_count >= max_reasonable_crash_count) {
        dbgln("WebContent has crashed {} times in quick succession! Not restarting...", m_crash_count);
        m_repeated_crash_timer->stop();
        return;
    }
    m_repeated_crash_timer->restart();

    initialize_client();
    VERIFY(m_client_state.client);

    // Don't keep a stale backup bitmap around.
    m_backup_bitmap = nullptr;

    handle_resize();

    if (load_error_page == LoadErrorPage::Yes) {
        StringBuilder builder;
        builder.append("<!DOCTYPE html>"sv);
        builder.append("<html lang=\"en\"><head><meta charset=\"UTF-8\"><title>Error!</title><style>"
                       ":root { color-scheme: light dark; font-family: system-ui, sans-serif; }"
                       "body { display: flex; flex-direction: column; align-items: center; justify-content: center; min-height: 100vh; box-sizing: border-box; margin: 0; padding: 1rem; text-align: center; }"
                       "header { display: flex; flex-direction: column; align-items: center; gap: 2rem; margin-bottom: 1rem; }"
                       "svg { height: 64px; width: auto; stroke: currentColor; fill: none; stroke-width: 1.5; stroke-linecap: round; stroke-linejoin: round; }"
                       "h1 { margin: 0; font-size: 1.5rem; }"
                       "p { font-size: 1rem; color: #555; }"
                       "</style></head><body>"sv);
        builder.append("<header>"sv);
        builder.append("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 17.5 21.5\">"sv);
        builder.append("<path class=\"b\" d=\"M11.75.75h-9c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2v-13l-5-5z\"/>"sv);
        builder.append("<path class=\"b\" d=\"M10.75.75v4c0 1.1.9 2 2 2h4M4.75 9.75l2 2M10.75 9.75l2 2M12.75 9.75l-2 2M6.75 9.75l-2 2M5.75 16.75c1-2.67 5-2.67 6 0\"/></svg>"sv);
        auto escaped_url = escape_html_entities(m_url.to_byte_string());
        builder.append("<h1>Ladybird flew off-course!</h1>"sv);
        builder.appendff("<p>The web page <a href=\"{}\">{}</a> has crashed.<br><br>You can reload the page to try again.</p>", escaped_url, escaped_url);
        builder.append("</body></html>"sv);
        load_html(builder.to_byte_string());
    }
}

void ViewImplementation::default_zoom_level_factor_changed()
{
    auto const default_zoom_level_factor = Application::settings().default_zoom_level_factor();
    set_zoom(default_zoom_level_factor);
}

void ViewImplementation::languages_changed()
{
    auto const& languages = Application::settings().languages();
    client().async_set_preferred_languages(page_id(), languages);
}

void ViewImplementation::autoplay_settings_changed()
{
    auto const& autoplay_settings = Application::settings().autoplay_settings();
    auto const& web_content_options = Application::web_content_options();

    if (autoplay_settings.enabled_globally || web_content_options.enable_autoplay == EnableAutoplay::Yes)
        client().async_set_autoplay_allowed_on_all_websites(page_id());
    else
        client().async_set_autoplay_allowlist(page_id(), autoplay_settings.site_filters.values());
}

void ViewImplementation::global_privacy_control_changed()
{
    auto global_privacy_control = Application::settings().global_privacy_control();
    client().async_set_enable_global_privacy_control(page_id(), global_privacy_control == GlobalPrivacyControl::Yes);
}

static ErrorOr<LexicalPath> save_screenshot(Gfx::Bitmap const* bitmap)
{
    if (!bitmap)
        return Error::from_string_literal("Failed to take a screenshot");

    auto file = AK::UnixDateTime::now().to_byte_string("screenshot-%Y-%m-%d-%H-%M-%S.png"sv);
    auto path = TRY(Application::the().path_for_downloaded_file(file));

    auto encoded = TRY(Gfx::PNGWriter::encode(*bitmap));

    auto dump_file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Write));
    TRY(dump_file->write_until_depleted(encoded));

    return path;
}

NonnullRefPtr<Core::Promise<LexicalPath>> ViewImplementation::take_screenshot(ScreenshotType type)
{
    auto promise = Core::Promise<LexicalPath>::construct();

    if (m_pending_screenshot) {
        // For simplicity, only allow taking one screenshot at a time for now. Revisit if we need
        // to allow spamming screenshot requests for some reason.
        promise->reject(Error::from_string_literal("A screenshot request is already in progress"));
        return promise;
    }

    switch (type) {
    case ScreenshotType::Visible:
        if (auto* visible_bitmap = m_client_state.has_usable_bitmap ? m_client_state.front_bitmap.bitmap.ptr() : m_backup_bitmap.ptr()) {
            if (auto result = save_screenshot(visible_bitmap); result.is_error())
                promise->reject(result.release_error());
            else
                promise->resolve(result.release_value());
        }
        break;

    case ScreenshotType::Full:
        m_pending_screenshot = promise;
        client().async_take_document_screenshot(page_id());
        break;
    }

    return promise;
}

NonnullRefPtr<Core::Promise<LexicalPath>> ViewImplementation::take_dom_node_screenshot(Web::UniqueNodeID node_id)
{
    auto promise = Core::Promise<LexicalPath>::construct();

    if (m_pending_screenshot) {
        // For simplicity, only allow taking one screenshot at a time for now. Revisit if we need
        // to allow spamming screenshot requests for some reason.
        promise->reject(Error::from_string_literal("A screenshot request is already in progress"));
        return promise;
    }

    m_pending_screenshot = promise;
    client().async_take_dom_node_screenshot(page_id(), node_id);

    return promise;
}

void ViewImplementation::did_receive_screenshot(Badge<WebContentClient>, Gfx::ShareableBitmap const& screenshot)
{
    VERIFY(m_pending_screenshot);

    if (auto result = save_screenshot(screenshot.bitmap()); result.is_error())
        m_pending_screenshot->reject(result.release_error());
    else
        m_pending_screenshot->resolve(result.release_value());

    m_pending_screenshot = nullptr;
}

NonnullRefPtr<Core::Promise<String>> ViewImplementation::request_internal_page_info(PageInfoType type)
{
    auto promise = Core::Promise<String>::construct();

    if (m_pending_info_request) {
        // For simplicity, only allow one info request at a time for now.
        promise->reject(Error::from_string_literal("A page info request is already in progress"));
        return promise;
    }

    m_pending_info_request = promise;
    client().async_request_internal_page_info(page_id(), type);

    return promise;
}

void ViewImplementation::did_receive_internal_page_info(Badge<WebContentClient>, PageInfoType, String const& info)
{
    VERIFY(m_pending_info_request);

    m_pending_info_request->resolve(String { info });
    m_pending_info_request = nullptr;
}

ErrorOr<LexicalPath> ViewImplementation::dump_gc_graph()
{
    auto promise = request_internal_page_info(PageInfoType::GCGraph);
    auto gc_graph_json = TRY(promise->await());

    LexicalPath path { Core::StandardPaths::tempfile_directory() };
    path = path.append(TRY(AK::UnixDateTime::now().to_string("gc-graph-%Y-%m-%d-%H-%M-%S.json"sv)));

    auto dump_file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Write));
    TRY(dump_file->write_until_depleted(gc_graph_json.bytes()));

    return path;
}

void ViewImplementation::set_user_style_sheet(String const& source)
{
    client().async_set_user_style(page_id(), source);
}

void ViewImplementation::use_native_user_style_sheet()
{
    extern String native_stylesheet_source;
    set_user_style_sheet(native_stylesheet_source);
}

void ViewImplementation::initialize_context_menus()
{
    auto& application = Application::the();

    m_navigate_back_action = Action::create("Go Back"sv, ActionID::NavigateBack, [this]() {
        traverse_the_history_by_delta(-1);
    });
    m_navigate_forward_action = Action::create("Go Forward"sv, ActionID::NavigateForward, [this]() {
        traverse_the_history_by_delta(+1);
    });
    m_navigate_back_action->set_enabled(false);
    m_navigate_forward_action->set_enabled(false);

    m_reset_zoom_action = Action::create("100%"sv, ActionID::ResetZoomViaToolbar, [this]() {
        reset_zoom();
    });
    m_reset_zoom_action->set_tooltip("Reset zoom level"sv);
    m_reset_zoom_action->set_visible(false);

    m_search_selected_text_action = Action::create("Search Selected Text"sv, ActionID::SearchSelectedText, [this]() {
        auto const& search_engine = Application::settings().search_engine();
        if (!search_engine.has_value())
            return;

        auto url_string = search_engine->format_search_query_for_navigation(*m_search_text);
        auto url = URL::Parser::basic_parse(url_string);
        VERIFY(url.has_value());

        Application::the().open_url_in_new_tab(*url, Web::HTML::ActivateTab::Yes);
    });
    m_search_selected_text_action->set_visible(false);

    auto take_and_save_screenshot = [this](auto type) {
        take_screenshot(type)
            ->when_resolved([](auto const& path) {
                Application::the().display_download_confirmation_dialog("Screenshot"sv, path);
            })
            .when_rejected([](auto const& error) {
                if (error.is_errno() && error.code() == ECANCELED)
                    return;

                auto error_message = MUST(String::formatted("{}", error));
                Application::the().display_error_dialog(error_message);
            });
    };

    m_take_visible_screenshot_action = Action::create("Take Visible Screenshot"sv, ActionID::TakeVisibleScreenshot, [take_and_save_screenshot]() {
        take_and_save_screenshot(ScreenshotType::Visible);
    });
    m_take_full_screenshot_action = Action::create("Take Full Screenshot"sv, ActionID::TakeFullScreenshot, [take_and_save_screenshot]() {
        take_and_save_screenshot(ScreenshotType::Full);
    });

    m_open_in_new_tab_action = Action::create("Open in New Tab"sv, ActionID::OpenInNewTab, [this]() {
        Application::the().open_url_in_new_tab(m_context_menu_url, Web::HTML::ActivateTab::No);
    });
    m_copy_url_action = Action::create("Copy URL"sv, ActionID::CopyURL, [this]() {
        insert_text_into_clipboard(url_text_to_copy(m_context_menu_url));
    });

    m_open_image_action = Action::create("Open Image"sv, ActionID::OpenImage, [this]() {
        load(m_context_menu_url);
    });
    m_copy_image_action = Action::create("Copy Image"sv, ActionID::CopyImage, [this]() {
        if (!m_image_context_menu_bitmap.has_value())
            return;

        auto bitmap = m_image_context_menu_bitmap.release_value();
        if (!bitmap.is_valid())
            return;

        auto encoded = Gfx::PNGWriter::encode(*bitmap.bitmap());
        if (encoded.is_error())
            return;

        if (on_insert_clipboard_entry)
            on_insert_clipboard_entry({ ByteString { encoded.value().bytes() }, "image/png"_string }, {});
    });

    m_open_audio_action = Action::create("Open Audio"sv, ActionID::OpenAudio, [this]() {
        load(m_context_menu_url);
    });
    m_open_video_action = Action::create("Open Video"sv, ActionID::OpenVideo, [this]() {
        load(m_context_menu_url);
    });
    m_media_play_action = Action::create("Play"sv, ActionID::PlayMedia, [this]() {
        client().async_toggle_media_play_state(page_id());
    });
    m_media_pause_action = Action::create("Pause"sv, ActionID::PauseMedia, [this]() {
        client().async_toggle_media_play_state(page_id());
    });
    m_media_mute_action = Action::create("Mute"sv, ActionID::MuteMedia, [this]() {
        client().async_toggle_media_mute_state(page_id());
    });
    m_media_unmute_action = Action::create("Unmute"sv, ActionID::UnmuteMedia, [this]() {
        client().async_toggle_media_mute_state(page_id());
    });
    m_media_show_controls_action = Action::create("Show Controls"sv, ActionID::ShowControls, [this]() {
        client().async_toggle_media_controls_state(page_id());
    });
    m_media_hide_controls_action = Action::create("Hide Controls"sv, ActionID::HideControls, [this]() {
        client().async_toggle_media_controls_state(page_id());
    });
    m_media_loop_action = Action::create_checkable("Loop"sv, ActionID::ToggleMediaLoopState, [this]() {
        client().async_toggle_media_loop_state(page_id());
    });

    m_page_context_menu = Menu::create("Page Context Menu"sv);
    m_page_context_menu->add_action(*m_navigate_back_action);
    m_page_context_menu->add_action(*m_navigate_forward_action);
    m_page_context_menu->add_action(application.reload_action());
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(application.copy_selection_action());
    m_page_context_menu->add_action(application.paste_action());
    m_page_context_menu->add_action(application.select_all_action());
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(*m_search_selected_text_action);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(*m_take_visible_screenshot_action);
    m_page_context_menu->add_action(*m_take_full_screenshot_action);
    m_page_context_menu->add_separator();
    m_page_context_menu->add_action(application.view_source_action());

    m_link_context_menu = Menu::create("Link Context Menu"sv);
    m_link_context_menu->add_action(*m_open_in_new_tab_action);
    m_link_context_menu->add_action(*m_copy_url_action);

    m_image_context_menu = Menu::create("Image Context Menu"sv);
    m_image_context_menu->add_action(*m_open_image_action);
    m_image_context_menu->add_action(*m_open_in_new_tab_action);
    m_image_context_menu->add_separator();
    m_image_context_menu->add_action(*m_copy_image_action);
    m_image_context_menu->add_action(*m_copy_url_action);

    m_media_context_menu = Menu::create("Media Context Menu"sv);
    m_media_context_menu->add_action(*m_media_play_action);
    m_media_context_menu->add_action(*m_media_pause_action);
    m_media_context_menu->add_action(*m_media_mute_action);
    m_media_context_menu->add_action(*m_media_unmute_action);
    m_media_context_menu->add_action(*m_media_show_controls_action);
    m_media_context_menu->add_action(*m_media_hide_controls_action);
    m_media_context_menu->add_action(*m_media_loop_action);
    m_media_context_menu->add_separator();
    m_media_context_menu->add_action(*m_open_audio_action);
    m_media_context_menu->add_action(*m_open_video_action);
    m_media_context_menu->add_action(*m_open_in_new_tab_action);
    m_media_context_menu->add_separator();
    m_media_context_menu->add_action(*m_copy_url_action);
}

void ViewImplementation::did_request_page_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position)
{
    auto const& search_engine = Application::settings().search_engine();

    auto selected_text = search_engine.has_value() ? selected_text_with_whitespace_collapsed() : OptionalNone {};
    TemporaryChange change_url { m_search_text, move(selected_text) };

    if (m_search_text.has_value()) {
        m_search_selected_text_action->set_text(search_engine->format_search_query_for_display(*m_search_text));
        m_search_selected_text_action->set_visible(true);
    } else {
        m_search_selected_text_action->set_visible(false);
    }

    if (m_page_context_menu->on_activation)
        m_page_context_menu->on_activation(to_widget_position(content_position));
}

void ViewImplementation::did_request_link_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, URL::URL url)
{
    m_context_menu_url = move(url);

    m_open_in_new_tab_action->set_text("Open in New Tab"sv);

    switch (url_type(m_context_menu_url)) {
    case URLType::Email:
        m_copy_url_action->set_text("Copy Email Address"sv);
        break;
    case URLType::Telephone:
        m_copy_url_action->set_text("Copy Phone Number"sv);
        break;
    case URLType::Other:
        m_copy_url_action->set_text("Copy Link Address"sv);
        break;
    }

    if (m_link_context_menu->on_activation)
        m_link_context_menu->on_activation(to_widget_position(content_position));
}

void ViewImplementation::did_request_image_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, URL::URL url, Optional<Gfx::ShareableBitmap> bitmap)
{
    m_context_menu_url = move(url);
    m_image_context_menu_bitmap = move(bitmap);

    m_open_in_new_tab_action->set_text("Open Image in New Tab"sv);
    m_copy_url_action->set_text("Copy Image URL"sv);

    m_copy_image_action->set_enabled(m_image_context_menu_bitmap.has_value());

    if (m_image_context_menu->on_activation)
        m_image_context_menu->on_activation(to_widget_position(content_position));
}

void ViewImplementation::did_request_media_context_menu(Badge<WebContentClient>, Gfx::IntPoint content_position, Web::Page::MediaContextMenu menu)
{
    m_context_menu_url = move(menu.media_url);

    m_open_in_new_tab_action->set_text(menu.is_video ? "Open Video in New Tab"sv : "Open Audio in new Tab"sv);
    m_copy_url_action->set_text(menu.is_video ? "Copy Video URL"sv : "Copy Audio URL"sv);

    m_open_audio_action->set_visible(!menu.is_video);
    m_open_video_action->set_visible(menu.is_video);

    m_media_play_action->set_visible(!menu.is_playing);
    m_media_pause_action->set_visible(menu.is_playing);

    m_media_mute_action->set_visible(!menu.is_muted);
    m_media_unmute_action->set_visible(menu.is_muted);

    m_media_show_controls_action->set_visible(!menu.has_user_agent_controls);
    m_media_hide_controls_action->set_visible(menu.has_user_agent_controls);

    m_media_loop_action->set_checked(menu.is_looping);

    if (m_media_context_menu->on_activation)
        m_media_context_menu->on_activation(to_widget_position(content_position));
}

}
