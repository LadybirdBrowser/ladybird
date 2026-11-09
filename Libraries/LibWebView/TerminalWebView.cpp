/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/CharacterTypes.h>
#include <AK/NeverDestroyed.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebView/Application.h>
#include <LibWebView/PageBackingStores.h>
#include <LibWebView/TerminalWebView.h>
#include <LibWebView/URL.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace WebView {

using Web::UIEvents::KeyCode;
using Web::UIEvents::KeyModifier;
using Web::UIEvents::MouseButton;

static int s_terminal_fd = -1;
static NeverDestroyed<ByteString> s_log_path;

// One wheel report from the terminal scrolls one line, as terminals send a report for each line a wheel or touchpad
// scrolls.
static constexpr double wheel_pixels_per_terminal_wheel_report = 40;

static constexpr auto double_click_interval = AK::Duration::from_milliseconds(500);
static constexpr int double_click_distance_in_device_pixels = 4;

// The Kitty keyboard protocol's flags.
static constexpr u32 keyboard_protocol_disambiguates_escape_codes = 1;
static constexpr u32 keyboard_protocol_reports_event_types = 2;
static constexpr u32 keyboard_protocol_reports_all_keys_as_escape_codes = 8;
static constexpr u32 keyboard_protocol_reports_associated_text = 16;
static constexpr u32 requested_keyboard_protocol_flags = keyboard_protocol_disambiguates_escape_codes
    | keyboard_protocol_reports_event_types
    | keyboard_protocol_reports_all_keys_as_escape_codes
    | keyboard_protocol_reports_associated_text;

static constexpr auto delete_all_images = "\x1b_Ga=d,d=A,q=2\x1b\\"sv;
static constexpr auto query_pane_pixel_size = "\x1b[14t"sv;

static constexpr u32 first_image_id = 1;
// Raw bytes per chunk of an image sent inline, which base64 makes the protocol's largest chunk of 4096 bytes.
static constexpr size_t direct_transmission_raw_chunk_size = 3072;
static constexpr size_t direct_transmission_flush_size = 256 * KiB;

static ByteString temporary_frame_path(u64 transmission_number)
{
    // The terminal only reads image files whose path names them as such, in a temporary directory.
    return ByteString::formatted("/tmp/tty-graphics-protocol-ladybird-{}-{}.rgb", getpid(), transmission_number);
}

static void append_graphics_command(StringBuilder& builder, StringView keys, StringView payload)
{
    builder.appendff("\x1b_G{};{}\x1b\\", keys, payload);
}

ErrorOr<void> TerminalWebView::take_over_terminal()
{
    if (!TRY(Core::System::isatty(STDIN_FILENO)) || !TRY(Core::System::isatty(STDOUT_FILENO)))
        return Error::from_string_literal("Terminal mode needs to run in a terminal");

    s_terminal_fd = TRY(Core::System::dup(STDOUT_FILENO));
    TRY(Core::System::fcntl(s_terminal_fd, F_SETFD, FD_CLOEXEC));

    *s_log_path = ByteString::formatted("/tmp/ladybird-terminal-{}.log", getpid());
    auto log_fd = TRY(Core::System::open(*s_log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));

    auto message = ByteString::formatted("Ladybird logs to {}\r\n", *s_log_path);
    (void)Core::System::write(s_terminal_fd, message.bytes());

    // Helper processes inherit these, so what any of them prints goes to the log rather than over the page.
    if (::dup2(log_fd, STDOUT_FILENO) < 0 || ::dup2(log_fd, STDERR_FILENO) < 0)
        return Error::from_syscall("dup2"sv, errno);
    TRY(Core::System::close(log_fd));
    return {};
}

TerminalWebView::PaneGeometry TerminalWebView::query_pane_geometry()
{
    struct winsize size {};
    if (ioctl(s_terminal_fd, TIOCGWINSZ, &size) < 0)
        return {};
    return { .rows = size.ws_row, .columns = size.ws_col, .pixel_size = { size.ws_xpixel, size.ws_ypixel } };
}

ErrorOr<NonnullOwnPtr<TerminalWebView>> TerminalWebView::create(Core::AnonymousBuffer theme, double device_pixel_ratio)
{
    if (s_terminal_fd < 0)
        return Error::from_string_literal("The terminal must be taken over before a terminal view is created");

    auto geometry = query_pane_geometry();
    auto page_pixel_size = geometry.page_pixel_size();
    if (page_pixel_size.is_empty())
        page_pixel_size = { 800, 600 };

    auto view = adopt_own(*new TerminalWebView(move(theme), page_pixel_size.to_type<Web::DevicePixels>(), device_pixel_ratio));
    view->m_pane_geometry = geometry;
    view->enter_terminal_modes();
    view->initialize_tab(Web::HTML::VisibilityState::Visible);
    view->set_presents_from_cpu_memory(true);
    view->show_notice(ByteString::formatted("logging to {}", *s_log_path));
    return view;
}

TerminalWebView::TerminalWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size, double device_pixel_ratio)
    : HeadlessWebView(move(theme), viewport_size)
{
    m_device_pixel_ratio = device_pixel_ratio;

    on_title_change = [this](auto const&) { draw_status_line(); };
    on_url_change = [this](auto const&) { draw_status_line(); };
    on_loading_state_change = [this](bool) { draw_status_line(); };
}

TerminalWebView::~TerminalWebView()
{
    leave_terminal_modes();
}

void TerminalWebView::quit()
{
    if (!m_terminal_modes_entered)
        return;
    leave_terminal_modes();
    Core::EventLoop::current().quit(0);
}

void TerminalWebView::write_to_terminal(StringView text)
{
    auto bytes = text.bytes();
    while (!bytes.is_empty()) {
        auto written = Core::System::write(s_terminal_fd, bytes);
        if (written.is_error()) {
            if (written.error().code() == EINTR || written.error().code() == EAGAIN)
                continue;
            return;
        }
        bytes = bytes.slice(written.value());
    }
}

void TerminalWebView::enter_terminal_modes()
{
    if (::tcgetattr(STDIN_FILENO, &m_original_terminal_attributes) == 0) {
        auto raw_attributes = m_original_terminal_attributes;
        cfmakeraw(&raw_attributes);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &raw_attributes);
    }
    m_terminal_modes_entered = true;

    // Alternate screen, hidden cursor, mouse reports for every motion with SGR pixel coordinates, focus reports,
    // bracketed paste and the keyboard protocol; then ask which keyboard flags took.
    write_to_terminal(ByteString::formatted(
        "\x1b[?1049h\x1b[?25l\x1b[2J"
        "\x1b[?1003h\x1b[?1006h\x1b[?1016h\x1b[?1004h\x1b[?2004h"
        "\x1b[>{}u\x1b[?u",
        requested_keyboard_protocol_flags));
    if (m_pane_geometry.pixel_size.is_empty())
        write_to_terminal(query_pane_pixel_size);

    m_input_notifier = Core::Notifier::construct(STDIN_FILENO, Core::Notifier::Type::Read);
    m_input_notifier->on_activation = [this] {
        u8 buffer[64 * KiB];
        auto read = Core::System::read(STDIN_FILENO, { buffer, sizeof(buffer) });
        if (read.is_error()) {
            if (read.error().code() != EINTR && read.error().code() != EAGAIN)
                quit();
            return;
        }
        if (read.value() == 0) {
            quit();
            return;
        }
        for (auto const& input : m_input_parser.parse({ buffer, read.value() }))
            handle_input(input);
    };

    m_signal_handler_ids.append(Core::EventLoop::register_signal(SIGWINCH, [this](int) {
        m_pane_geometry = query_pane_geometry();
        if (m_pane_geometry.pixel_size.is_empty())
            write_to_terminal(query_pane_pixel_size);
        apply_pane_geometry();
    }));
    for (auto signal_number : { SIGTERM, SIGHUP }) {
        m_signal_handler_ids.append(Core::EventLoop::register_signal(signal_number, [this](int) {
            quit();
        }));
    }
}

void TerminalWebView::leave_terminal_modes()
{
    if (!m_terminal_modes_entered)
        return;
    m_terminal_modes_entered = false;

    m_input_notifier = nullptr;
    for (auto id : m_signal_handler_ids)
        Core::EventLoop::unregister_signal(id);
    m_signal_handler_ids.clear();

    write_to_terminal("\x1b[<u\x1b[?2004l\x1b[?1004l\x1b[?1016l\x1b[?1006l\x1b[?1003l"sv);
    write_to_terminal(delete_all_images);
    write_to_terminal("\x1b[?25h\x1b[?1049l"sv);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &m_original_terminal_attributes);

    // The terminal deletes each frame's file once it reads it. Remove any it had not read yet.
    for (u64 number = m_image_data_transmissions > 4 ? m_image_data_transmissions - 4 : 0; number < m_image_data_transmissions; ++number)
        (void)Core::System::unlink(temporary_frame_path(number));
}

void TerminalWebView::apply_pane_geometry()
{
    m_drawn_status_line = {};
    m_frame_needs_full_redraw = true;
    m_displayed_image_id.clear();
    write_to_terminal(delete_all_images);
    write_to_terminal("\x1b[2J"sv);
    draw_status_line();

    auto page_pixel_size = m_pane_geometry.page_pixel_size();
    if (page_pixel_size.is_empty())
        return;
    if (page_pixel_size != viewport_size().to_type<int>())
        reset_viewport_size(page_pixel_size.to_type<Web::DevicePixels>());
    schedule_frame_flush();
}

void TerminalWebView::did_accept_presented_backing_store(i32, Gfx::IntRect damage_rect)
{
    m_pending_damage = m_pending_damage.has_value() ? m_pending_damage->united(damage_rect) : damage_rect;
    schedule_frame_flush();
}

void TerminalWebView::schedule_frame_flush()
{
    if (m_frame_flush_scheduled)
        return;
    m_frame_flush_scheduled = true;
    Core::deferred_invoke([weak_this = make_weak_ptr<TerminalWebView>()] {
        if (!weak_this)
            return;
        weak_this->m_frame_flush_scheduled = false;
        weak_this->flush_frame();
    });
}

void TerminalWebView::flush_frame()
{
    if (!m_terminal_modes_entered)
        return;

    auto frame = backing_stores().displayed_frame();
    if (!frame.has_value() || !frame->shared_image_buffer)
        return;
    auto bitmap = frame->shared_image_buffer->bitmap_if_present();
    if (!bitmap) {
        show_notice("frames are not in CPU memory");
        return;
    }

    if (auto result = transmit_frame(*bitmap); result.is_error())
        show_notice(ByteString::formatted("cannot draw frame: {}", result.error()));
    m_pending_damage.clear();
    m_frame_needs_full_redraw = false;
}

static ErrorOr<void> convert_to_rgb(Gfx::Bitmap const& bitmap, Gfx::IntRect rect, ByteBuffer& pixels)
{
    TRY(pixels.try_resize(static_cast<size_t>(rect.width()) * rect.height() * 3));
    auto red_is_low_byte = bitmap.format() == Gfx::BitmapFormat::RGBA8888 || bitmap.format() == Gfx::BitmapFormat::RGBx8888;
    auto* out = pixels.data();
    for (int y = rect.y(); y < rect.y() + rect.height(); ++y) {
        auto const* scanline = bitmap.scanline(y);
        for (int x = rect.x(); x < rect.x() + rect.width(); ++x) {
            auto pixel = scanline[x];
            u8 low = pixel & 0xff;
            u8 middle = (pixel >> 8) & 0xff;
            u8 high = (pixel >> 16) & 0xff;
            *out++ = red_is_low_byte ? low : high;
            *out++ = middle;
            *out++ = red_is_low_byte ? high : low;
        }
    }
    return {};
}

ErrorOr<void> TerminalWebView::send_image_data(StringView keys, ReadonlyBytes pixels)
{
    StringBuilder command;
    if (m_graphics_transmission == GraphicsTransmission::TemporaryFile) {
        auto path = temporary_frame_path(m_image_data_transmissions++);
        auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate, 0600));
        TRY(file->write_until_depleted(pixels));
        append_graphics_command(command, ByteString::formatted("{},t=t", keys), TRY(encode_base64(path.bytes())));
        write_to_terminal(command.string_view());
        return {};
    }

    for (size_t offset = 0; offset < pixels.size(); offset += direct_transmission_raw_chunk_size) {
        auto chunk = pixels.slice(offset, min(direct_transmission_raw_chunk_size, pixels.size() - offset));
        auto has_more_chunks = offset + direct_transmission_raw_chunk_size < pixels.size();
        auto chunk_keys = offset == 0 ? ByteString::formatted("{},m={}", keys, has_more_chunks ? 1 : 0) : ByteString::formatted("m={}", has_more_chunks ? 1 : 0);
        append_graphics_command(command, chunk_keys, TRY(encode_base64(chunk)));
        if (command.length() >= direct_transmission_flush_size || !has_more_chunks) {
            write_to_terminal(command.string_view());
            command.clear();
        }
    }
    return {};
}

ErrorOr<void> TerminalWebView::transmit_frame(Gfx::Bitmap const& bitmap)
{
    // A frame laid out for a larger pane than this one is cut to fit above the status line.
    auto size = bitmap.size();
    auto page_pixel_size = m_pane_geometry.page_pixel_size();
    if (!page_pixel_size.is_empty())
        size = { min(size.width(), page_pixel_size.width()), min(size.height(), page_pixel_size.height()) };
    if (size.is_empty())
        return {};
    Gfx::IntRect frame_rect { {}, size };

    auto shows_frame_of_this_size = !m_frame_needs_full_redraw && m_displayed_image_id.has_value() && m_displayed_image_size == size;
    auto damage_rect = m_pending_damage.value_or(Gfx::IntRect {}).intersected(frame_rect);
    if (shows_frame_of_this_size && damage_rect.is_empty())
        return {};

    // Only the damaged part of a frame is sent, as an edit of the image on display.
    if (m_sends_damaged_rectangles && shows_frame_of_this_size && damage_rect != frame_rect) {
        TRY(convert_to_rgb(bitmap, damage_rect, m_frame_pixels));
        auto keys = ByteString::formatted("a=f,r=1,i={},x={},y={},s={},v={},f=24,q=1", *m_displayed_image_id, damage_rect.x(), damage_rect.y(), damage_rect.width(), damage_rect.height());
        return send_image_data(keys, m_frame_pixels);
    }

    TRY(convert_to_rgb(bitmap, frame_rect, m_frame_pixels));
    auto image_id = m_displayed_image_id == first_image_id ? first_image_id + 1 : first_image_id;
    write_to_terminal("\x1b[1;1H"sv);
    TRY(send_image_data(ByteString::formatted("a=T,f=24,s={},v={},i={},p=1,C=1,q=1", size.width(), size.height(), image_id), m_frame_pixels));
    // The new frame is placed before the old one goes, so the pane never shows neither.
    if (m_displayed_image_id.has_value())
        write_to_terminal(ByteString::formatted("\x1b_Ga=d,d=I,i={},q=2\x1b\\", *m_displayed_image_id));

    m_displayed_image_id = image_id;
    m_displayed_image_size = size;
    return {};
}

void TerminalWebView::handle_input(TerminalInput const& input)
{
    input.visit(
        [this](TerminalMouseReport const& report) {
            handle_mouse_report(report);
        },
        [this](TerminalKeyReport const& report) {
            handle_key_report(report);
        },
        [this](TerminalPaste const& paste) {
            if (m_url_prompt_text.has_value()) {
                m_url_prompt_text = ByteString::formatted("{}{}", *m_url_prompt_text, paste.text);
                draw_status_line();
                return;
            }
            commit_text_from_input_method(Utf16String::from_utf8(paste.text));
        },
        [this](TerminalFocusChange const& focus_change) {
            set_has_system_focus(focus_change.has_focus);
        },
        [this](TerminalCommand const& command) {
            handle_command(command);
        },
        [this](TerminalGraphicsReply const& reply) {
            if (reply.text.ends_with(";OK"sv))
                return;
            if (m_graphics_transmission == GraphicsTransmission::TemporaryFile) {
                // A terminal on another machine cannot read this machine's files.
                m_graphics_transmission = GraphicsTransmission::Direct;
                show_notice("sending frames inline");
                m_frame_needs_full_redraw = true;
                schedule_frame_flush();
                return;
            }
            show_notice(ByteString::formatted("graphics: {}", reply.text));
        },
        [this](TerminalKeyboardProtocolFlags const& flags) {
            m_terminal_reports_key_releases = (flags.flags & keyboard_protocol_reports_event_types) != 0;
        },
        [this](TerminalPixelSizeReport const& report) {
            if (!m_pane_geometry.pixel_size.is_empty())
                return;
            m_pane_geometry.pixel_size = report.size;
            apply_pane_geometry();
        });
}

static Web::MouseEvent mouse_event(Web::MouseEvent::Type type, Gfx::IntPoint position, MouseButton button, MouseButton buttons, KeyModifier modifiers)
{
    Web::MouseEvent event {};
    event.type = type;
    event.position = position.to_type<Web::DevicePixels>();
    event.screen_position = event.position;
    event.button = button;
    event.buttons = buttons;
    event.modifiers = modifiers;
    return event;
}

void TerminalWebView::handle_mouse_report(TerminalMouseReport const& report)
{
    auto viewport = viewport_size().to_type<int>();
    auto is_in_status_line = report.position.y() >= viewport.height();
    auto position_in_page = Gfx::IntPoint {
        clamp(report.position.x(), 0, max(0, viewport.width() - 1)),
        clamp(report.position.y(), 0, max(0, viewport.height() - 1)),
    };

    switch (report.kind) {
    case TerminalMouseReport::Kind::Wheel: {
        if (is_in_status_line)
            return;
        auto event = mouse_event(Web::MouseEvent::Type::MouseWheel, position_in_page, MouseButton::None, m_held_mouse_buttons, report.modifiers);
        event.wheel_delta_x = report.wheel_steps.x() * wheel_pixels_per_terminal_wheel_report;
        event.wheel_delta_y = report.wheel_steps.y() * wheel_pixels_per_terminal_wheel_report;
        event.wheel_tick_120ths_x = Web::wheel_tick_120ths_from_pixel_delta(event.wheel_delta_x, Web::WheelDeltaPrecision::Discrete);
        event.wheel_tick_120ths_y = Web::wheel_tick_120ths_from_pixel_delta(event.wheel_delta_y, Web::WheelDeltaPrecision::Discrete);
        event.wheel_delta_precision = Web::WheelDeltaPrecision::Discrete;
        enqueue_input_event(move(event));
        return;
    }

    case TerminalMouseReport::Kind::Motion:
        // A drag that leaves the page keeps going at its edge.
        if (is_in_status_line && m_held_mouse_buttons == MouseButton::None)
            return;
        enqueue_input_event(mouse_event(Web::MouseEvent::Type::MouseMove, position_in_page, MouseButton::None, m_held_mouse_buttons, report.modifiers));
        return;

    case TerminalMouseReport::Kind::Release: {
        if (!has_flag(m_held_mouse_buttons, report.button))
            return;
        m_held_mouse_buttons &= ~report.button;
        auto event = mouse_event(Web::MouseEvent::Type::MouseUp, position_in_page, report.button, m_held_mouse_buttons, report.modifiers);
        event.click_count = m_click_count;
        enqueue_input_event(move(event));
        return;
    }

    case TerminalMouseReport::Kind::Press: {
        if (is_in_status_line) {
            open_url_prompt();
            return;
        }

        auto now = MonotonicTime::now();
        auto distance = max(abs(position_in_page.x() - m_last_press_position.x()), abs(position_in_page.y() - m_last_press_position.y()));
        if (report.button == m_last_pressed_mouse_button && now - m_last_press_time < double_click_interval && distance <= double_click_distance_in_device_pixels * m_device_pixel_ratio)
            ++m_click_count;
        else
            m_click_count = 1;
        m_last_pressed_mouse_button = report.button;
        m_last_press_position = position_in_page;
        m_last_press_time = now;

        m_held_mouse_buttons |= report.button;
        auto event = mouse_event(Web::MouseEvent::Type::MouseDown, position_in_page, report.button, m_held_mouse_buttons, report.modifiers);
        event.click_count = m_click_count;
        enqueue_input_event(move(event));
        return;
    }
    }
}

static bool is_typed_character(u32 code_point)
{
    return code_point >= ' ' && code_point != 0x7f;
}

static bool has_shortcut_modifier(KeyModifier modifiers)
{
    return (modifiers & (KeyModifier::Mod_Ctrl | KeyModifier::Mod_Super)) != 0;
}

// The code point a key event carries, as the Qt UI takes it from the text a key types.
static u32 code_point_for_key_event(TerminalKeyReport const& report)
{
    if (!report.text_code_points.is_empty())
        return report.text_code_points.first();
    if (has_flag(report.modifiers, KeyModifier::Mod_Shift) && is_typed_character(report.code_point))
        return report.shifted_code_point.value_or(to_ascii_uppercase(report.code_point));
    return report.code_point;
}

bool TerminalWebView::handle_browser_shortcut(TerminalKeyReport const& report)
{
    auto is_key_with = [&](KeyCode key, KeyModifier modifiers) {
        return report.key == key && report.modifiers == modifiers;
    };

    if (is_key_with(KeyCode::Key_Q, KeyModifier::Mod_Ctrl)) {
        quit();
        return true;
    }
    if (is_key_with(KeyCode::Key_L, KeyModifier::Mod_Ctrl)) {
        open_url_prompt();
        return true;
    }
    if (is_key_with(KeyCode::Key_R, KeyModifier::Mod_Ctrl) || is_key_with(KeyCode::Key_F5, KeyModifier::Mod_None)) {
        reload();
        return true;
    }
    // Terminals that follow the macOS convention send Option+Left and Option+Right as Alt+B and Alt+F, the keys that
    // move a shell's cursor by words.
    if (is_key_with(KeyCode::Key_Left, KeyModifier::Mod_Alt) || is_key_with(KeyCode::Key_B, KeyModifier::Mod_Alt)) {
        traverse_the_history_by_delta(-1);
        return true;
    }
    if (is_key_with(KeyCode::Key_Right, KeyModifier::Mod_Alt) || is_key_with(KeyCode::Key_F, KeyModifier::Mod_Alt)) {
        traverse_the_history_by_delta(1);
        return true;
    }
    return false;
}

void TerminalWebView::handle_url_prompt_key(TerminalKeyReport const& report)
{
    if (report.phase == TerminalKeyReport::Phase::Release)
        return;

    switch (report.key) {
    case KeyCode::Key_Escape:
        m_url_prompt_text.clear();
        break;
    case KeyCode::Key_Return:
        open_url(m_url_prompt_text.release_value());
        break;
    case KeyCode::Key_Backspace: {
        Utf8View text { m_url_prompt_text->view() };
        if (!text.is_empty())
            m_url_prompt_text = ByteString { text.unicode_substring_view(0, text.length() - 1).as_string() };
        break;
    }
    default: {
        auto code_point = code_point_for_key_event(report);
        if (has_shortcut_modifier(report.modifiers) || !is_typed_character(code_point))
            break;
        StringBuilder builder;
        builder.append(*m_url_prompt_text);
        builder.append_code_point(code_point);
        m_url_prompt_text = builder.to_byte_string();
        break;
    }
    }
    draw_status_line();
}

void TerminalWebView::handle_key_report(TerminalKeyReport const& report)
{
    if (m_url_prompt_text.has_value()) {
        handle_url_prompt_key(report);
        return;
    }

    if (report.phase != TerminalKeyReport::Phase::Release && handle_browser_shortcut(report))
        return;

    auto code_point = code_point_for_key_event(report);
    auto types_text = code_point != 0 && (!report.text_code_points.is_empty() || !has_shortcut_modifier(report.modifiers));

    auto key_event = [&](Web::KeyEvent::Type type, bool repeat) {
        Web::KeyEvent event {};
        event.type = type;
        event.key = report.key;
        event.modifiers = report.modifiers;
        event.code_point = code_point;
        event.repeat = repeat;
        event.should_insert_text = type == Web::KeyEvent::Type::KeyDown && types_text;
        return event;
    };

    // A release goes to the page only after its press did, which a shortcut, the URL prompt, or a terminal that turns
    // the press into another key keeps from it.
    auto& is_down_in_page = m_key_is_down_in_page[to_underlying(report.key)];
    switch (report.phase) {
    case TerminalKeyReport::Phase::Press:
        enqueue_input_event(key_event(Web::KeyEvent::Type::KeyDown, false));
        if (report.is_legacy_encoding || !m_terminal_reports_key_releases)
            enqueue_input_event(key_event(Web::KeyEvent::Type::KeyUp, false));
        else
            is_down_in_page = true;
        break;
    case TerminalKeyReport::Phase::Repeat:
        enqueue_input_event(key_event(Web::KeyEvent::Type::KeyDown, true));
        break;
    case TerminalKeyReport::Phase::Release:
        if (!exchange(is_down_in_page, false))
            break;
        enqueue_input_event(key_event(Web::KeyEvent::Type::KeyUp, false));
        break;
    }
}

void TerminalWebView::handle_command(TerminalCommand const& command)
{
    if (command.name == "open"sv) {
        open_url(command.argument);
    } else if (command.name == "back"sv) {
        traverse_the_history_by_delta(-1);
    } else if (command.name == "forward"sv) {
        traverse_the_history_by_delta(1);
    } else if (command.name == "reload"sv) {
        reload();
    } else if (command.name == "quit"sv) {
        quit();
    } else if (command.name == "frames"sv) {
        m_sends_damaged_rectangles = command.argument == "damage"sv;
        m_frame_needs_full_redraw = true;
        schedule_frame_flush();
        show_notice(m_sends_damaged_rectangles ? "sending damaged rectangles" : "sending whole frames");
    } else {
        show_notice(ByteString::formatted("unknown command: {}", command.name));
    }
}

void TerminalWebView::open_url_prompt()
{
    m_url_prompt_text = displayed_url().serialize().to_byte_string();
    draw_status_line();
}

void TerminalWebView::open_url(StringView text)
{
    auto url = sanitize_url(text, Application::settings().search_engine_settings().engine);
    if (!url.has_value()) {
        show_notice(ByteString::formatted("not a URL: {}", text));
        return;
    }
    load(*url);
}

void TerminalWebView::show_notice(ByteString notice)
{
    m_notice = move(notice);
    draw_status_line();

    if (!m_notice_timer) {
        m_notice_timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(5), [this] {
            m_notice = {};
            draw_status_line();
        });
    }
    m_notice_timer->restart();
}

void TerminalWebView::draw_status_line()
{
    if (!m_terminal_modes_entered || m_pane_geometry.rows <= 0 || m_pane_geometry.columns <= 0)
        return;

    StringBuilder text;
    if (m_url_prompt_text.has_value()) {
        text.appendff(" Go to: {}█", *m_url_prompt_text);
    } else {
        auto url = displayed_url().serialize();
        auto heading = title().is_empty() ? url.to_byte_string() : title().to_byte_string();
        text.appendff(" {} — {}", heading, url);
        text.append("   ctrl-l url · ctrl-r reload · alt-←/→ history · ctrl-q quit"sv);
        if (!m_notice.is_empty())
            text.appendff(" · {}", m_notice);
    }
    Utf8View text_view { text.string_view() };
    auto visible_length = min(text_view.length(), static_cast<size_t>(max(0, m_pane_geometry.columns - 2)));
    auto visible_text = text_view.unicode_substring_view(0, visible_length);

    StringBuilder line;
    line.appendff("\x1b[{};1H\x1b[2K", m_pane_geometry.rows);
    if (!m_url_prompt_text.has_value())
        line.appendff("\x1b[{}m●\x1b[0m", is_loading() ? 33 : 32);
    line.appendff("\x1b[2m{}\x1b[0m", visible_text.as_string());

    auto status_line = line.to_byte_string();
    if (status_line == m_drawn_status_line)
        return;
    m_drawn_status_line = status_line;
    write_to_terminal(status_line);
}

}
