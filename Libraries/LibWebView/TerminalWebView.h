/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibGfx/Rect.h>
#include <LibWebCommon/UIEvents/MouseButton.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/TerminalInputParser.h>

#include <termios.h>

namespace WebView {

// A view that shows its page in the terminal it was started from: frames as Kitty graphics, and the terminal's mouse
// and keyboard reports as input. The last row of the terminal is a status line.
class WEBVIEW_API TerminalWebView final : public HeadlessWebView {
public:
    // Keeps the terminal for drawing, and sends what this process and every helper it starts would print there to a
    // log file instead. Must run before any helper process is started.
    static ErrorOr<void> take_over_terminal();

    static ErrorOr<NonnullOwnPtr<TerminalWebView>> create(Core::AnonymousBuffer theme, double device_pixel_ratio);
    virtual ~TerminalWebView() override;

private:
    struct PaneGeometry {
        int rows { 0 };
        int columns { 0 };
        Gfx::IntSize pixel_size;

        int cell_width() const { return columns > 0 ? pixel_size.width() / columns : 0; }
        int cell_height() const { return rows > 0 ? pixel_size.height() / rows : 0; }
        // Everything but the last row, which holds the status line.
        Gfx::IntSize page_pixel_size() const { return { columns * cell_width(), (rows - 1) * cell_height() }; }
    };

    enum class GraphicsTransmission : u8 {
        TemporaryFile,
        Direct,
    };

    TerminalWebView(Core::AnonymousBuffer theme, Web::DevicePixelSize viewport_size, double device_pixel_ratio);

    static PaneGeometry query_pane_geometry();

    // ^ViewImplementation
    virtual void did_accept_presented_backing_store(i32 bitmap_id, Gfx::IntRect damage_rect) override;

    void enter_terminal_modes();
    void leave_terminal_modes();
    void quit();
    void write_to_terminal(StringView);

    void apply_pane_geometry();

    void schedule_frame_flush();
    void flush_frame();
    ErrorOr<void> transmit_frame(Gfx::Bitmap const&);
    ErrorOr<void> send_image_data(StringView keys, ReadonlyBytes pixels);

    void handle_input(TerminalInput const&);
    void handle_mouse_report(TerminalMouseReport const&);
    void handle_key_report(TerminalKeyReport const&);
    bool handle_browser_shortcut(TerminalKeyReport const&);
    void handle_url_prompt_key(TerminalKeyReport const&);
    void handle_command(TerminalCommand const&);

    void open_url_prompt();
    void open_url(StringView);
    void show_notice(ByteString);
    void draw_status_line();

    struct termios m_original_terminal_attributes {};
    bool m_terminal_modes_entered { false };
    RefPtr<Core::Notifier> m_input_notifier;
    Vector<int> m_signal_handler_ids;
    TerminalInputParser m_input_parser;
    bool m_terminal_reports_key_releases { false };
    Array<bool, 256> m_key_is_down_in_page {};

    PaneGeometry m_pane_geometry;

    bool m_frame_flush_scheduled { false };
    bool m_frame_needs_full_redraw { true };
    Optional<Gfx::IntRect> m_pending_damage;
    ByteBuffer m_frame_pixels;
    u64 m_image_data_transmissions { 0 };
    GraphicsTransmission m_graphics_transmission { GraphicsTransmission::TemporaryFile };
    bool m_sends_damaged_rectangles { false };
    Optional<u32> m_displayed_image_id;
    Gfx::IntSize m_displayed_image_size;

    Web::UIEvents::MouseButton m_held_mouse_buttons { Web::UIEvents::MouseButton::None };
    Web::UIEvents::MouseButton m_last_pressed_mouse_button { Web::UIEvents::MouseButton::None };
    Gfx::IntPoint m_last_press_position;
    MonotonicTime m_last_press_time { MonotonicTime::now_coarse() };
    int m_click_count { 0 };

    Optional<ByteString> m_url_prompt_text;
    ByteString m_notice;
    RefPtr<Core::Timer> m_notice_timer;
    ByteString m_drawn_status_line;
};

}
