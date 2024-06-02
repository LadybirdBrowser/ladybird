/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "OutOfProcessWebView.h"
#include "WebContentClient.h"
#include <AK/ByteString.h>
#include <LibFileSystemAccessClient/Client.h>
#include <LibGUI/Application.h>
#include <LibGUI/Desktop.h>
#include <LibGUI/Painter.h>
#include <LibGUI/Scrollbar.h>
#include <LibGUI/Window.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/Worker/WebWorkerClient.h>

REGISTER_WIDGET(WebView, OutOfProcessWebView)

namespace WebView {

OutOfProcessWebView::OutOfProcessWebView()
{
    set_should_hide_unnecessary_scrollbars(true);
    set_focus_policy(GUI::FocusPolicy::StrongFocus);

    initialize_client(CreateNewClient::Yes);

    on_did_layout = [this](auto content_size) {
        set_content_size(content_size);
    };

    on_ready_to_paint = [this]() {
        update();
    };

    on_request_file = [this](auto const& path, auto request_id) {
        auto file = FileSystemAccessClient::Client::the().request_file_read_only_approved(window(), path);

        if (file.is_error())
            client().async_handle_file_return(m_client_state.page_index, file.error().code(), {}, request_id);
        else
            client().async_handle_file_return(m_client_state.page_index, 0, IPC::File::adopt_file(file.release_value().release_stream()), request_id);
    };

    on_scroll_by_delta = [this](auto x_delta, auto y_delta) {
        horizontal_scrollbar().increase_slider_by(x_delta);
        vertical_scrollbar().increase_slider_by(y_delta);
    };

    on_scroll_to_point = [this](auto position) {
        horizontal_scrollbar().set_value(position.x());
        vertical_scrollbar().set_value(position.y());
    };

    on_cursor_change = [this](auto cursor) {
        set_override_cursor(cursor);
    };

    on_enter_tooltip_area = [](auto, auto tooltip) {
        GUI::Application::the()->show_tooltip(MUST(String::from_byte_string(tooltip)), nullptr);
    };

    on_leave_tooltip_area = []() {
        GUI::Application::the()->hide_tooltip();
    };

    on_finish_handling_key_event = [this](auto const& event) {
        finish_handling_key_event(event);
    };

    on_request_worker_agent = []() {
        auto worker_client = MUST(Web::HTML::WebWorkerClient::try_create());
        return worker_client->dup_socket();
    };
}

OutOfProcessWebView::~OutOfProcessWebView() = default;

void OutOfProcessWebView::initialize_client(WebView::ViewImplementation::CreateNewClient)
{
    // FIXME: Don't create a new process when CreateNewClient is false
    //        We should create a new tab/window in the UI instead, and re-use the existing WebContentClient object.
    m_client_state = {};

    m_client_state.client = WebContentClient::try_create(*this).release_value_but_fixme_should_propagate_errors();
    m_client_state.client->on_web_content_process_crash = [this] {
        deferred_invoke([this] {
            handle_web_content_process_crash();
        });
    };

    m_client_state.client_handle = Web::Crypto::generate_random_uuid().release_value_but_fixme_should_propagate_errors();
    client().async_set_window_handle(m_client_state.page_index, m_client_state.client_handle);

    client().async_update_system_theme(m_client_state.page_index, Gfx::current_system_theme_buffer());
    client().async_update_system_fonts(m_client_state.page_index, Gfx::FontDatabase::default_font_query(), Gfx::FontDatabase::fixed_width_font_query(), Gfx::FontDatabase::window_title_font_query());

    Vector<Web::DevicePixelRect> screen_rects;
    for (auto const& screen_rect : GUI::Desktop::the().rects()) {
        screen_rects.append(screen_rect.to_type<Web::DevicePixels>());
    }
    client().async_update_screen_rects(m_client_state.page_index, screen_rects, GUI::Desktop::the().main_screen_index());
}

void OutOfProcessWebView::paint_event(GUI::PaintEvent& event)
{
    Super::paint_event(event);

    // If the available size is empty, we don't have a front or back bitmap to draw.
    if (available_size().is_empty())
        return;

    GUI::Painter painter(*this);
    painter.add_clip_rect(event.rect());

    if (auto* bitmap = m_client_state.has_usable_bitmap ? m_client_state.front_bitmap.bitmap.ptr() : m_backup_bitmap.ptr()) {
        painter.add_clip_rect(frame_inner_rect());
        painter.translate(frame_thickness(), frame_thickness());
        if (m_content_scales_to_viewport) {
            auto bitmap_rect = Gfx::IntRect {
                {},
                m_client_state.has_usable_bitmap
                    ? m_client_state.front_bitmap.last_painted_size
                    : m_backup_bitmap_size
            };
            painter.draw_scaled_bitmap(rect(), *bitmap, bitmap_rect);
        } else {
            painter.blit({ 0, 0 }, *bitmap, bitmap->rect());
        }
        return;
    }

    painter.fill_rect(frame_inner_rect(), palette().base());
}

void OutOfProcessWebView::resize_event(GUI::ResizeEvent& event)
{
    Super::resize_event(event);
    client().async_set_viewport_rect(m_client_state.page_index, Web::DevicePixelRect({ horizontal_scrollbar().value(), vertical_scrollbar().value() }, available_size()));
    handle_resize();
}

Web::DevicePixelRect OutOfProcessWebView::viewport_rect() const
{
    return visible_content_rect().to_type<Web::DevicePixels>();
}

Gfx::IntPoint OutOfProcessWebView::to_content_position(Gfx::IntPoint widget_position) const
{
    return GUI::AbstractScrollableWidget::to_content_position(widget_position);
}

Gfx::IntPoint OutOfProcessWebView::to_widget_position(Gfx::IntPoint content_position) const
{
    return GUI::AbstractScrollableWidget::to_widget_position(content_position);
}

void OutOfProcessWebView::update_zoom()
{
    client().async_set_device_pixels_per_css_pixel(m_client_state.page_index, m_device_pixel_ratio * m_zoom_level);
    // FIXME: Refactor this into separate update_viewport_rect() + request_repaint() like in Ladybird
    handle_resize();
}

void OutOfProcessWebView::keydown_event(GUI::KeyEvent& event)
{
    enqueue_native_event(Web::KeyEvent::Type::KeyDown, event);
}

void OutOfProcessWebView::keyup_event(GUI::KeyEvent& event)
{
    enqueue_native_event(Web::KeyEvent::Type::KeyUp, event);
}

void OutOfProcessWebView::mousedown_event(GUI::MouseEvent& event)
{
    enqueue_native_event(Web::MouseEvent::Type::MouseDown, event);
}

void OutOfProcessWebView::mouseup_event(GUI::MouseEvent& event)
{
    enqueue_native_event(Web::MouseEvent::Type::MouseUp, event);

    if (event.button() == GUI::MouseButton::Backward) {
        if (on_navigate_back)
            on_navigate_back();
    } else if (event.button() == GUI::MouseButton::Forward) {
        if (on_navigate_forward)
            on_navigate_forward();
    }
}

void OutOfProcessWebView::mousemove_event(GUI::MouseEvent& event)
{
    enqueue_native_event(Web::MouseEvent::Type::MouseMove, event);
}

void OutOfProcessWebView::mousewheel_event(GUI::MouseEvent& event)
{
    enqueue_native_event(Web::MouseEvent::Type::MouseWheel, event);
}

void OutOfProcessWebView::doubleclick_event(GUI::MouseEvent& event)
{
    enqueue_native_event(Web::MouseEvent::Type::DoubleClick, event);
}

void OutOfProcessWebView::theme_change_event(GUI::ThemeChangeEvent& event)
{
    Super::theme_change_event(event);
    client().async_update_system_theme(m_client_state.page_index, Gfx::current_system_theme_buffer());
}

void OutOfProcessWebView::screen_rects_change_event(GUI::ScreenRectsChangeEvent& event)
{
    Vector<Web::DevicePixelRect> screen_rects;
    for (auto const& screen_rect : event.rects()) {
        screen_rects.append(screen_rect.to_type<Web::DevicePixels>());
    }
    client().async_update_screen_rects(m_client_state.page_index, screen_rects, event.main_screen_index());
}

void OutOfProcessWebView::did_scroll()
{
    client().async_set_viewport_rect(m_client_state.page_index, visible_content_rect().to_type<Web::DevicePixels>());
}

ByteString OutOfProcessWebView::dump_layout_tree()
{
    return client().dump_layout_tree(m_client_state.page_index);
}

OrderedHashMap<String, String> OutOfProcessWebView::get_local_storage_entries()
{
    return client().get_local_storage_entries(m_client_state.page_index);
}

OrderedHashMap<String, String> OutOfProcessWebView::get_session_storage_entries()
{
    return client().get_session_storage_entries(m_client_state.page_index);
}

void OutOfProcessWebView::set_content_filters(Vector<String> filters)
{
    client().async_set_content_filters(m_client_state.page_index, move(filters));
}

void OutOfProcessWebView::set_autoplay_allowed_on_all_websites()
{
    client().async_set_autoplay_allowed_on_all_websites(m_client_state.page_index);
}

void OutOfProcessWebView::set_autoplay_allowlist(Vector<String> allowlist)
{
    client().async_set_autoplay_allowlist(m_client_state.page_index, move(allowlist));
}

void OutOfProcessWebView::set_proxy_mappings(Vector<ByteString> proxies, HashMap<ByteString, size_t> mappings)
{
    client().async_set_proxy_mappings(m_client_state.page_index, move(proxies), move(mappings));
}

void OutOfProcessWebView::connect_to_webdriver(ByteString const& webdriver_ipc_path)
{
    client().async_connect_to_webdriver(m_client_state.page_index, webdriver_ipc_path);
}

void OutOfProcessWebView::set_window_position(Gfx::IntPoint position)
{
    client().async_set_window_position(m_client_state.page_index, position.to_type<Web::DevicePixels>());
}

void OutOfProcessWebView::set_window_size(Gfx::IntSize size)
{
    client().async_set_window_size(m_client_state.page_index, size.to_type<Web::DevicePixels>());
}

void OutOfProcessWebView::focusin_event(GUI::FocusEvent&)
{
    client().async_set_has_focus(m_client_state.page_index, true);
}

void OutOfProcessWebView::focusout_event(GUI::FocusEvent&)
{
    client().async_set_has_focus(m_client_state.page_index, false);
}

void OutOfProcessWebView::set_system_visibility_state(bool visible)
{
    client().async_set_system_visibility_state(m_client_state.page_index, visible);
}

void OutOfProcessWebView::show_event(GUI::ShowEvent&)
{
    set_system_visibility_state(true);
}

void OutOfProcessWebView::hide_event(GUI::HideEvent&)
{
    set_system_visibility_state(false);
}

void OutOfProcessWebView::enqueue_native_event(Web::MouseEvent::Type type, GUI::MouseEvent const& event)
{
    auto position = to_content_position(event.position()).to_type<Web::DevicePixels>();
    auto screen_position = (event.position() + (window()->position() + relative_position())).to_type<Web::DevicePixels>();

    // FIXME: This wheel delta step size multiplier is used to remain the old scroll behaviour, in future use system step size.
    static constexpr int SCROLL_STEP_SIZE = 24;
    auto wheel_delta_x = event.wheel_delta_x() * SCROLL_STEP_SIZE;
    auto wheel_delta_y = event.wheel_delta_y() * SCROLL_STEP_SIZE;

    enqueue_input_event(Web::MouseEvent { type, position, screen_position, static_cast<Web::UIEvents::MouseButton>(to_underlying(event.button())), static_cast<Web::UIEvents::MouseButton>(event.buttons()), static_cast<KeyModifier>(event.modifiers()), wheel_delta_x, wheel_delta_y, nullptr });
}

struct KeyData : Web::ChromeInputData {
    explicit KeyData(GUI::KeyEvent const& event)
        : event(make<GUI::KeyEvent>(event))
    {
    }

    NonnullOwnPtr<GUI::KeyEvent> event;
};

void OutOfProcessWebView::enqueue_native_event(Web::KeyEvent::Type type, GUI::KeyEvent const& event)
{
    enqueue_input_event(Web::KeyEvent { type, event.key(), static_cast<KeyModifier>(event.modifiers()), event.code_point(), make<KeyData>(event) });
}

void OutOfProcessWebView::finish_handling_key_event(Web::KeyEvent const& key_event)
{
    // First, we give our superclass a chance to handle the event.
    //
    // If it does not, we dispatch the event to our parent widget, but limited such that it will never bubble up to the
    // Window. (Otherwise, it would then dispatch the event to us since we are the focused widget, and it would go around
    // indefinitely.)
    //
    // Finally, any unhandled KeyDown events are propagated to trigger any shortcut Actions.
    auto& chrome_data = verify_cast<KeyData>(*key_event.chrome_data);
    auto& event = *chrome_data.event;

    switch (key_event.type) {
    case Web::KeyEvent::Type::KeyDown:
        Super::keydown_event(event);
        break;
    case Web::KeyEvent::Type::KeyUp:
        Super::keyup_event(event);
        break;
    }

    if (!event.is_accepted()) {
        parent_widget()->dispatch_event(event, window());

        // NOTE: If other events can ever trigger shortcuts, propagate those here.
        if (!event.is_accepted() && event.type() == GUI::Event::Type::KeyDown)
            window()->propagate_shortcuts(static_cast<GUI::KeyEvent&>(event), this);
    }
}

void OutOfProcessWebView::set_content_scales_to_viewport(bool b)
{
    m_content_scales_to_viewport = b;
}

}
