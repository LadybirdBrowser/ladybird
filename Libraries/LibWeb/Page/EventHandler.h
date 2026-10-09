/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibCompositing/Scrolling/WheelGestureIdentity.h>
#include <LibCompositing/Types.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/DOM/HoverEventData.h>
#include <LibWeb/DOM/NodeIdentity.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Painting/Forward.h>
#include <LibWebCommon/Gamepad/GamepadSnapshot.h>
#include <LibWebCommon/Page/EventResult.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/Page/QueuedInputEvent.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebCommon/UIEvents/KeyCode.h>

namespace Web {

struct AsyncScrollOperation {
    GC::Ptr<HTML::LocalNavigable> navigable;
    Compositing::AsyncScrollOperationID operation_id { 0 };
};

// Content another process hosts under the pointer: the event goes to that process, at this position in the
// viewport of the navigable it hosts.
struct RemoteInputEventTarget {
    HTML::CrossProcessId navigable_id;
    CSSPixelPoint position;
};

class WEB_API EventHandler {
    friend class AutoScrollHandler;

public:
    EventHandler(Badge<HTML::LocalNavigable>, HTML::LocalNavigable&);
    ~EventHandler();

    void visit_edges(JS::Cell::Visitor& visitor) const;

    EventResult handle_mousedown(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, int click_count, Optional<Web::ScrollbarDraggedByCompositor> const& = {}, Optional<RemoteInputEventTarget>* remote_target = nullptr);
    EventResult handle_mousemove(CSSPixelPoint, CSSPixelPoint screen_position, unsigned buttons, unsigned modifiers, Optional<RemoteInputEventTarget>* remote_target = nullptr);

    // Whether the event handled now is older than a pointer move whose hover the screen showed, which moves neither the
    // hover nor the pointer.
    void set_handling_outdated_input(bool handling_outdated_input) { m_handling_outdated_input = handling_outdated_input; }
    void set_handling_input_event_id(u64 input_event_id) { m_handling_input_event_id = input_event_id; }
    bool handling_outdated_input() const { return m_handling_outdated_input; }
    u64 handling_input_event_id() const { return m_handling_input_event_id; }
    EventResult handle_mouseup(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, Optional<RemoteInputEventTarget>* remote_target = nullptr);
    EventResult handle_mousewheel(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, double wheel_delta_x, double wheel_delta_y, Web::WheelDeltaPrecision = Web::WheelDeltaPrecision::Discrete, Web::ScrollGesturePhase = Web::ScrollGesturePhase::None, bool async_scroll_performed_default_action = false, Optional<AsyncScrollOperation>* async_scroll_operation = nullptr, Optional<RemoteInputEventTarget>* remote_target = nullptr);
    EventResult handle_mouseleave();
#if defined(AK_OS_MACOS)
    bool select_word_for_dictionary_lookup(CSSPixelPoint visual_viewport_position);
#endif
    void update_hover_after_scroll();
    // Hovers what is under the pointer at `visual_viewport_position`, where a hover beside the event loop shows it
    // already: the boundary events of the move fire, with no mouse move event.
    void update_hover_at(CSSPixelPoint visual_viewport_position, GC::Ptr<DOM::Node> hover_target = {});
    // Shows the cursor of what is under the pointer again, where a move resolved it before it hovered that, once a
    // rendering update has applied the style of the hover.
    void update_cursor_after_rendering_update();
    GC::Ptr<DOM::Node> target_node_for_mouse_position(CSSPixelPoint);

    EventResult handle_keydown(UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat, bool should_insert_text, bool async_scroll_performed_default_action = false);
    struct KeyboardScrollSnapshot {
        Compositing::KeyboardScrollState state;
        Vector<GC::Weak<DOM::EventTarget>> event_path;
        GC::Weak<DOM::Node> scroll_target;
    };
    KeyboardScrollSnapshot keyboard_scroll_snapshot() const;
    EventResult handle_keyup(UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);

    EventResult handle_drag_and_drop_event(DragEvent::Type, CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, Vector<HTML::SelectedFile> files);
    EventResult handle_pinch_event(CSSPixelPoint, unsigned modifiers, double scale_delta);
    [[nodiscard]] EventResult perform_paste_action();
    EventResult perform_paste_action(NonnullRefPtr<HTML::DragDataStore> const&);

    void handle_gamepad_connected(Gamepad::GamepadDescription const&);
    void handle_gamepad_updated(Gamepad::GamepadState const&);
    void handle_gamepad_disconnected(Gamepad::GamepadHandle);

    void process_auto_scroll();

    enum class SelectionMode : u8 {
        None,
        Character,
        Word,
        Paragraph,
    };
    bool is_handling_mouse_selection() const { return m_selection_mode != SelectionMode::None; }
    void reset_mouse_input_tracking(Badge<Page>);
    void reset_hover_for_document_replacement(Badge<HTML::LocalNavigable>);

    Optional<MiddleButtonScrollHandler&> middle_button_scroll_handler() const
    {
        if (m_middle_button_scroll_handler)
            return *m_middle_button_scroll_handler;
        return {};
    }

    void clear_per_test_input_state(Badge<Internals::Internals>);

private:
    bool should_ignore_device_input_event() const;
    GC::Ptr<DOM::Node> scroll_target_for_key_input() const;
    int page_scroll_distance_for_key_input() const;

    EventResult fire_keyboard_event(Utf16FlyString const& event_name, HTML::LocalNavigable&, UIEvents::KeyCode, unsigned modifiers, u32 code_point, bool repeat);
    [[nodiscard]] EventResult fire_text_input_event(HTML::LocalNavigable&, Utf16String const& data);
    [[nodiscard]] EventResult input_event(Utf16FlyString const& event_name, Utf16FlyString const& input_type, HTML::LocalNavigable&, Variant<u32, Utf16String> code_point_or_string);

    [[nodiscard]] EventResult perform_copy_action();
    [[nodiscard]] EventResult perform_cut_action();
    EventResult insert_pasted_content(Utf16View plain_text, Optional<Utf16View> html);

    EventResult focus_next_element();
    EventResult focus_previous_element();

    struct MouseEventCoordinates {
        CSSPixelPoint page_offset;
        CSSPixelPoint visual_viewport_position;
        CSSPixelPoint viewport_position;
        CSSPixelPoint offset;
    };
    bool fire_click_events(GC::Ref<DOM::Node>, MouseEventCoordinates const&, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, int click_count);

    MouseEventCoordinates compute_mouse_event_coordinates(CSSPixelPoint visual_viewport_position, CSSPixelPoint viewport_position, Layout::Node const& layout_node) const;
    CSSPixelPoint compute_mouse_event_page_offset(CSSPixelPoint event_client_offset) const;
    CSSPixelPoint compute_mouse_event_movement(CSSPixelPoint screen_position) const;

    struct Target {
        Compositing::RustFFI::NodeSlotId hit_node;
        NonnullRefPtr<Layout::NodeArena> arena;
        RefPtr<Painting::ChromeWidget> chrome_widget;
        DOM::NodeIdentity node;
        Optional<int> index_in_node;
        bool is_text_fragment { false };

        Layout::Node* layout_node(Layout::BegunRead const& read) const;
        GC::Ptr<DOM::Node> dom_node() const;
    };
    Optional<Target> target_for_mouse_position(CSSPixelPoint position);
    GC::Ptr<DOM::Node> focus_candidate_for_position(Layout::BegunRead const& read, CSSPixelPoint) const;

    void run_mousedown_default_actions(Layout::BegunRead const& read, DOM::Document&, CSSPixelPoint visual_viewport_position, unsigned button, unsigned modifiers, int click_count);
    void run_activation_behavior(GC::Ref<DOM::Node>, unsigned button, unsigned modifiers);

    void maybe_show_context_menu(GC::Ref<DOM::Node>, MouseEventCoordinates const&, CSSPixelPoint screen_position, CSSPixelPoint viewport_position, unsigned buttons, unsigned modifiers);
    bool maybe_request_paste_for_middle_click(Layout::BegunRead const& read, DOM::Document&, CSSPixelPoint visual_viewport_position);

    Optional<Painting::CaretPosition> prepare_mouse_selection(DOM::Document&, CSSPixelPoint visual_viewport_position);
    bool initiate_character_selection(DOM::Document&, Painting::CaretPosition const&, CSS::UserSelect, bool shift_held);
    bool initiate_word_selection(Layout::BegunRead const& read, DOM::Document&, Painting::CaretPosition const&, CSS::UserSelect);
    bool initiate_paragraph_selection(DOM::Document&, Painting::CaretPosition const&, CSS::UserSelect);
    bool select_context_menu_text(Layout::BegunRead const& read, DOM::Document&, CSSPixelPoint visual_viewport_position);
    bool select_context_menu_url_token(DOM::Document&, Painting::CaretPosition const&, CSS::UserSelect);
#if defined(AK_OS_MACOS)
    bool select_word_at_position(Layout::BegunRead const& read, DOM::Document&, CSSPixelPoint visual_viewport_position);
    void start_selection_from_preserved_mousedown(Layout::BegunRead const& read, DOM::Document&);
    void finish_selection_from_preserved_mousedown(DOM::Document&, CSSPixelPoint visual_viewport_position);
#endif

    void update_mouse_selection(Layout::BegunRead const& read, CSSPixelPoint visual_viewport_position);
    void apply_mouse_selection(Layout::BegunRead const& read, CSSPixelPoint visual_viewport_position);

    void clear_mousedown_tracking();
    void stop_updating_selection();

    void update_hover_after_scroll(CSSPixelPoint visual_viewport_position, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, GC::Ptr<DOM::Node> hover_target = {});
    EventResult dispatch_wheel_event(Layout::BegunRead const& read, Layout::Node&, CSSPixelPoint visual_viewport_position, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers, double wheel_delta_x, double wheel_delta_y, bool is_cancelable);
    // Both drop the latch when what it refers to is gone from the document, so that the event is targeted afresh.
    Layout::Node* validated_wheel_scroll_latch_target_layout_node(Layout::BegunRead const& read, DOM::Document&);
    Layout::Node* validated_latched_wheel_scrolling_box();
    EventResult dispatch_synthetic_pinch_wheel_event(CSSPixelPoint visual_viewport_position, CSSPixelPoint screen_position, unsigned modifiers, double wheel_delta_y);

    enum class PointerEventType : u8 {
        PointerDown,
        PointerUp,
        PointerMove,
        PointerCancel
    };
    enum class PointerEventDispatchResult : u8 {
        RunDefaultActions,
        CancelledByPage,
        SwallowedByChromeWidget,
    };
    PointerEventDispatchResult dispatch_a_pointer_event_for_a_device_that_supports_hover(PointerEventType, GC::Ptr<DOM::Node>, RefPtr<Painting::ChromeWidget>, MouseEventCoordinates const&, CSSPixelPoint screen_position, CSSPixelPoint movement, unsigned button, unsigned buttons, unsigned modifiers, int click_count = 0);
    void track_the_effective_position_of_the_legacy_mouse_pointer(GC::Ptr<DOM::Node>, Optional<DOM::HoverEventData> = {});
    void report_hovered_node_to_client(GC::Ptr<DOM::Node>);

    bool dispatch_chrome_widget_pointer_event(RefPtr<Painting::ChromeWidget>, Utf16FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position);
    void update_hovered_chrome_widget(RefPtr<Painting::ChromeWidget>);
    void update_nested_navigable_under_pointer(GC::Ptr<HTML::LocalNavigable>);

    void update_cursor(Layout::BegunRead const& read, Layout::Node const*, GC::Ptr<DOM::Node> host_element, RefPtr<Painting::ChromeWidget>, bool hit_text_fragment = false);
    void record_last_known_mouse_position(CSSPixelPoint visual_viewport_position, CSSPixelPoint screen_position, unsigned buttons, unsigned modifiers);
    EventResult cancel_drag_and_drop_event(CSSPixelPoint, CSSPixelPoint screen_position, unsigned button, unsigned buttons, unsigned modifiers);

    bool has_committed_root_box() const;

    GC::Ref<HTML::LocalNavigable> m_navigable;

    SelectionMode m_selection_mode { SelectionMode::None };
    InputEventsTarget* m_mouse_selection_target { nullptr };
    GC::Ptr<DOM::Range> m_selection_origin;

    RefPtr<Painting::ChromeWidget> m_hovered_chrome_widget;
    RefPtr<Painting::ChromeWidget> m_captured_chrome_widget;

    GC::Weak<DOM::Node> m_effective_legacy_mouse_pointer_position;

    GC::Weak<DOM::Node> m_last_mousedown_target;
    GC::Weak<DOM::Node> m_mousedown_target;
    GC::Weak<HTML::LocalNavigable> m_nested_navigable_under_pointer;
    Optional<CSSPixelPoint> m_mousedown_visual_viewport_position;
    int m_mousedown_click_count { 0 };
    bool m_mousedown_target_is_drag_candidate { false };
#if defined(AK_OS_MACOS)
    bool m_mousedown_preserved_selection { false };
#endif

    // https://w3c.github.io/pointerevents/#the-pointerdown-event
    // The PREVENT MOUSE EVENT flag.
    // FIXME: This should be per-pointer, of which there can be multiple. Move it once multiple simultaneous pointer
    //        inputs are supported.
    bool m_prevent_mouse_event { false };

    Optional<CSSPixelPoint> m_mousemove_previous_screen_position;
    Optional<CSSPixelPoint> m_last_known_mouse_visual_viewport_position;
    // Whether a move resolved the cursor before it hovered what is under the pointer, since the last rendering update.
    bool m_cursor_resolved_before_hover { false };
    bool m_handling_outdated_input { false };
    // The UI process's id of the input event being handled, or 0.
    u64 m_handling_input_event_id { 0 };
    CSSPixelPoint m_last_known_mouse_screen_position;
    unsigned m_last_known_mouse_buttons { 0 };
    unsigned m_last_known_mouse_modifiers { 0 };

    OwnPtr<AutoScrollHandler> m_auto_scroll_handler;
    OwnPtr<MiddleButtonScrollHandler> m_middle_button_scroll_handler;
    NonnullOwnPtr<DragAndDropEventHandler> m_drag_and_drop_event_handler;

    Optional<UIEvents::KeyCode> m_held_scroll_key;
    OwnPtr<HTML::UserScrollGestureHold> m_scroll_key_gesture_hold;

    // The target and the scrolling box of the wheel gesture in progress: the gesture's later wheel events go to them
    // without hit testing, and the scrolling box absorbs the gesture at its edge. The compositor latches by the same
    // rules, so the two agree on the scroller of an event.
    struct WheelScrollLatch {
        Compositing::WheelGestureIdentity gesture;
        GC::Weak<DOM::Node> wheel_event_target;
        // The default action walks the boxes of this pseudo-element rather than the target's own.
        Optional<CSS::PseudoElement> wheel_event_target_pseudo_element {};
        bool gesture_handed_to_nested_navigable { false };
        // Unset until the default action of this thread has moved a box, so for as long as the compositor scrolls.
        Optional<Web::AsyncScrollNodeStableID> scrolling_box {};
    };
    Optional<WheelScrollLatch> m_wheel_scroll_latch;
};

}
