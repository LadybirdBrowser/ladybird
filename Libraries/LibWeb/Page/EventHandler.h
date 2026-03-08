/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibUnicode/Forward.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Gamepad/SDLGamepadForward.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/UIEvents/KeyCode.h>

namespace Web {

class WEB_API EventHandler {
    friend class AutoScrollHandler;

public:
    explicit EventHandler(Badge<HTML::Navigable>, HTML::Navigable&);
    ~EventHandler();

    EventResult handle_mouseup(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);
    EventResult handle_mousedown(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);
    EventResult handle_mousemove(CSSPixelPoint, CSSPixelPoint screen_position, unsigned buttons, unsigned modifiers);
    EventResult handle_mouseleave();
    EventResult handle_mousewheel(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, int wheel_delta_x, int wheel_delta_y);
    EventResult handle_doubleclick(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);
    EventResult handle_tripleclick(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);

    EventResult handle_drag_and_drop_event(DragEvent::Type, CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, Vector<HTML::SelectedFile> files);

    EventResult handle_pinch_event(CSSPixelPoint, double scale_delta);

    EventResult handle_keydown(UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);
    EventResult handle_keyup(UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);

    void process_auto_scroll();

    EventResult handle_paste(Utf16String const& text);

    void handle_sdl_input_events();

    void visit_edges(JS::Cell::Visitor& visitor) const;

    Unicode::Segmenter& word_segmenter();

    enum class SelectionMode : u8 {
        None,
        Character,
        Word,
        Paragraph,
    };

    bool is_handling_mouse_selection() const { return m_selection_mode != SelectionMode::None; }

private:
    EventResult focus_next_element();
    EventResult focus_previous_element();

    GC::Ptr<DOM::Node> focus_candidate_for_position(CSSPixelPoint) const;

    EventResult fire_keyboard_event(FlyString const& event_name, HTML::Navigable&, UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);
    [[nodiscard]] EventResult input_event(FlyString const& event_name, FlyString const& input_type, HTML::Navigable&, Variant<u32, Utf16String> code_point_or_string);
    CSSPixelPoint compute_mouse_event_client_offset(CSSPixelPoint event_page_position) const;
    CSSPixelPoint compute_mouse_event_page_offset(CSSPixelPoint event_client_offset) const;
    CSSPixelPoint compute_mouse_event_movement(CSSPixelPoint event_client_offset) const;

    struct Target {
        GC::Ptr<Painting::Paintable> paintable;
        GC::Ptr<Painting::ChromeWidget> chrome_widget;
        Optional<int> index_in_node;
    };
    Optional<Target> target_for_mouse_position(CSSPixelPoint position);
    void update_hovered_chrome_widget(GC::Ptr<Painting::ChromeWidget>);
    void update_mouse_selection(CSSPixelPoint visual_viewport_position);
    void apply_mouse_selection(CSSPixelPoint visual_viewport_position);

    GC::Ptr<Painting::PaintableBox> paint_root();
    GC::Ptr<Painting::PaintableBox const> paint_root() const;

    bool should_ignore_device_input_event() const;

    void handle_gamepad_connected(SDL_JoystickID);
    void handle_gamepad_updated(SDL_JoystickID);
    void handle_gamepad_disconnected(SDL_JoystickID);

    GC::Ref<HTML::Navigable> m_navigable;

    SelectionMode m_selection_mode { SelectionMode::None };
    InputEventsTarget* m_mouse_selection_target { nullptr };
    GC::Ptr<DOM::Range> m_selection_origin;

    GC::Ptr<Painting::ChromeWidget> m_hovered_chrome_widget;
    GC::Ptr<Painting::ChromeWidget> m_captured_chrome_widget;

    NonnullOwnPtr<DragAndDropEventHandler> m_drag_and_drop_event_handler;

    GC::Weak<DOM::Node> m_mousedown_target;

    Optional<CSSPixelPoint> m_mousemove_previous_screen_position;

    OwnPtr<Unicode::Segmenter> m_word_segmenter;

    OwnPtr<AutoScrollHandler> m_auto_scroll_handler;
};

}
