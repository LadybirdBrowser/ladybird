/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Internals.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Internals/InternalAnimationTimeline.h>
#include <LibWeb/Internals/InternalsBase.h>
#include <LibWeb/Painting/Forward.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/WebIDL/Types.h>

namespace JS {

class Object;
class Realm;

}

namespace Web::Internals {

class WEB_API Internals final : public InternalsBase {
    WEB_WRAPPABLE(Internals, InternalsBase);
    GC_DECLARE_ALLOCATOR(Internals);

public:
    // Same as Internals.idl
    static constexpr unsigned short MOD_NONE = 0;
    static constexpr unsigned short MOD_ALT = 1;
    static constexpr unsigned short MOD_CTRL = 2;
    static constexpr unsigned short MOD_SHIFT = 4;
    static constexpr unsigned short MOD_SUPER = 8;
    static constexpr unsigned short MOD_KEYPAD = 16;

    static constexpr unsigned short BUTTON_LEFT = 0;
    static constexpr unsigned short BUTTON_MIDDLE = 1;
    static constexpr unsigned short BUTTON_RIGHT = 2;

    virtual ~Internals() override;

    void signal_test_is_done(Utf16String const& text);
    void set_test_timeout(double milliseconds);
    void force_incompatible_visual_context_tree_rebuild();
    void set_force_dark_enabled(bool);
    void set_show_line_box_borders(bool);
    void set_force_dark_thresholds(i32, i32);
    u64 visual_context_tree_node_count();
    u64 visual_context_pending_dirty_box_count();
    u64 accumulated_visual_context_incremental_update_count();
    u64 visual_context_tree_node_capacity();
    u64 visual_context_tree_dead_node_count();
    u64 visual_context_tree_structural_epoch();
    GC::Ref<JS::Object> visual_context_node_indices(DOM::Element&);
    void send_mismatched_visual_context_tree_update_to_compositor();
    u64 layout_tree_pre_order_label_violation_count();
    u64 layout_tree_pre_order_relabel_count();
    WebIDL::ExceptionOr<void> load_reference_test_metadata();

    WebIDL::ExceptionOr<Utf16String> set_time_zone(Utf16String const& time_zone);

    void gc();
    void gc_async(GC::Ref<WebIDL::Promise>);
    WebIDL::ExceptionOr<void> mark_as_garbage(Utf16String const& variable_name);
    bool wrapper_is_preserved(JS::Object&);
    bool has_activity_root(JS::Object&);
    WebIDL::UnsignedLongLong message_port_pending_outgoing_message_count(HTML::MessagePort&);
    WebIDL::UnsignedLongLong html_collection_cache_generation(DOM::HTMLCollection&);
    void fail_next_message_port_transfer(HTML::MessagePort&);
    bool message_ports_are_directly_entangled(HTML::MessagePort&, HTML::MessagePort&);
    GC::Ref<XHR::XMLHttpRequest> create_xml_http_request_for_document(DOM::Document&);
    Optional<Painting::HitTestResult> hit_test(double x, double y);
    GC::Ptr<JS::Object> hit_test_result(double x, double y);
    GC::Ptr<JS::Object> take_context_menu_request();

    void send_text(HTML::HTMLElement&, Utf16String const&, WebIDL::UnsignedShort modifiers);
    void send_key(HTML::HTMLElement&, Utf16String const&, WebIDL::UnsignedShort modifiers, WebIDL::UnsignedLong repeat_count);
    void paste(HTML::HTMLElement& target, Utf16String const& text);
    void paste_from_clipboard();
    void commit_text();

    // Low-level mouse primitives
    void mouse_down(double x, double y, WebIDL::UnsignedShort click_count, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers);
    void mouse_up(double x, double y, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers);
    void mouse_move(double x, double y, WebIDL::UnsignedShort modifiers);
    void mouse_leave();

    // High-level mouse conveniences
    void click(double x, double y, WebIDL::UnsignedShort click_count, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers);
    void click_and_hold(double x, double y, WebIDL::UnsignedShort click_count, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers);
    void wheel(GC::Ref<WebIDL::Promise>, double x, double y, double delta_x, double delta_y, bool precise, Bindings::ScrollGesturePhase);
    void wheel(double x, double y, double delta_x, double delta_y, bool precise, Bindings::ScrollGesturePhase, GC::Ref<WebIDL::Promise>);
    void pinch(double x, double y, double scale_delta, WebIDL::UnsignedShort modifiers);
    void reset_zoom();

    Utf16String current_cursor();

    Utf16String selected_text_for_clipboard();
    WebIDL::ExceptionOr<void> set_clipboard_file(Utf16String const& name, Utf16String const& mime_type, Utf16String const& data);

    void set_marked_text_from_input_method(Utf16String const& text);
    void commit_text_from_input_method(Utf16String const& text, WebIDL::Long replacement_start, WebIDL::Long replacement_length);
    void unmark_text_from_input_method();
    GC::Ptr<Geometry::DOMRect> current_caret_rect();

    WebIDL::ExceptionOr<bool> dispatch_user_activated_event(DOM::EventTarget&, DOM::Event& event);

    void spoof_current_url(Utf16String const& url);
    void load_url(Utf16String const& url);

    GC::Ref<InternalAnimationTimeline> create_internal_animation_timeline();

    WebIDL::ExceptionOr<void> simulate_drag_start(double x, double y, Utf16String const& name, Utf16String const& contents);
    void simulate_drag_move(double x, double y);
    void simulate_drop(double x, double y);

    void expire_cookies_with_time_offset(WebIDL::LongLong seconds);
    GC::Ref<WebIDL::Promise> delete_all_cookies();
    WebIDL::ExceptionOr<bool> has_cookie_for_url(Utf16String const& url, String const& name, String const& value);

    bool set_http_memory_cache_enabled(bool enabled);
    void simulate_request_server_connection_loss();
    void simulate_worker_request_server_connection_loss();
    WebIDL::ExceptionOr<void> set_content_blockers(Utf16String const& patterns);
    WebIDL::ExceptionOr<void> set_site_compatibility_data(Utf16String const& source);
    void set_content_blocking_enabled(bool enabled);
    WebIDL::UnsignedLongLong partial_layout_count();
    WebIDL::UnsignedLongLong full_layout_count();
    WebIDL::UnsignedLongLong layout_run_cache_hit_count();
    WebIDL::UnsignedLongLong table_cell_measurement_cache_miss_count();
    WebIDL::UnsignedLongLong intrinsic_measurement_count();
    WebIDL::UnsignedLongLong accumulated_visual_context_tree_build_count();
    WebIDL::UnsignedLongLong paint_cache_spliced_capture_count();
    WebIDL::UnsignedLongLong paint_cache_capture_site_visit_count();
    void set_autoplay_policy(Utf16String const& policy);

    Utf16String get_computed_role(DOM::Element& element);
    Utf16String get_computed_label(DOM::Element& element);
    Utf16String get_computed_aria_level(DOM::Element& element);

    static u16 get_echo_server_port();
    static void set_echo_server_port(u16 port);

    WebIDL::ExceptionOr<void> set_hsts_policy(Utf16String const& domain, u64 max_age, bool include_sub_domains);
    WebIDL::ExceptionOr<void> ingest_hsts_header(Utf16String const& url, Utf16String const& header_value);
    WebIDL::ExceptionOr<bool> is_known_hsts_host(Utf16String const& domain);

    void set_browser_zoom(double factor);
    void set_device_pixel_ratio(double ratio);

    bool headless();
    bool screen_wake_lock_active();

    bool needs_repaint();
    bool needs_display_list_record();

    Utf16String dump_display_list();
    Utf16String dump_accessibility_tree();
    Utf16String dump_layout_tree(GC::Ref<DOM::Node>);
    Utf16String dump_stacking_context_tree();
    Utf16String stacking_context_structure_verification_report();
    Utf16String dump_gc_graph();
    Utf16String dump_session_history();
    Utf16String dump_ui_process_session_history();
    Utf16String dump_ui_process_session_history_without_update();
    bool capture_session_history_snapshot();
    bool restore_captured_session_history_snapshot();
    bool register_session_store_tab();
    Utf16String dump_session_store_tab_state();
    Utf16String dump_site_isolation_process_tree();
    GC::Ref<WebIDL::Promise> flush_session_history_traversal_queue();
    bool has_html_parser_end_state(DOM::Document& document) { return document.has_html_parser_end_state(); }

    bool has_shadow_root(GC::Ref<DOM::Element>);
    GC::Ptr<DOM::ShadowRoot> get_shadow_root(GC::Ref<DOM::Element>);

    void handle_sdl_input_events();

    GC::Ref<InternalGamepad> connect_virtual_gamepad();
    void disconnect_virtual_gamepad(GC::Ref<InternalGamepad>);

    void perform_per_test_cleanup();

    void set_highlighted_node(GC::Ptr<DOM::Node> node);

    void clear_element(HTML::HTMLElement&);
    void set_environments_top_level_url(Utf16String const& url);
    void set_geolocation_emulated_position(double latitude, double longitude, double accuracy);

    u64 parser_non_append_insertions();
    DOM::Document::StyleInvalidationCounters const& style_invalidation_counters() const;
    GC::Ref<JS::Object> style_invalidation_counters_object() const;
    void reset_style_invalidation_counters();
    GC::Ref<JS::Object> get_rendering_scheduler_counters() const;
    void reset_rendering_scheduler_counters();
    void set_manual_rendering_opportunities(bool enabled);
    void inject_rendering_opportunity(double frame_time_ms);
    void update_compositor_animations();
    bool run_empty_animation_style_update_for_testing();
    void arm_compositor_animation_timers_for_testing();
    void fire_compositor_animation_wakeup_for_testing(double frame_time_ms);
    void request_reentrant_animation_style_flush_for_testing(GC::Ref<DOM::Node>);
    GC::Ref<JS::Object> layout_tree_build_stats();
    GC::Ref<JS::Object> compare_layout_tree_with_full_rebuild();
    GC::Ref<JS::Object> computed_values_stats();
    GC::Ref<JS::Object> style_ffi_counters();
    GC::Ref<JS::Object> style_engine_counters();
    u64 style_record_identity(DOM::Element&);
    u64 layout_style_record_identity(DOM::Element&);
    u64 before_style_record_identity(DOM::Element&);
    u64 before_layout_style_record_identity(DOM::Element&);
    u64 paint_style_record_identity(DOM::Element&);
    u64 layout_node_identity(DOM::Node&);
    u64 layout_arena_live_slot_count();
    u64 layout_arena_shell_count();
    double style_engine_match_document();
    Utf16String style_engine_matched_rules();
    GC::Ref<JS::Object> style_engine_transaction_reactions();
    void reset_style_ffi_counters();
    GC::Ref<JS::Object> style_group_sharing_info(DOM::Element&);
    void update_style();
    void set_user_style(Utf16String const& source);
    void set_preferred_color_scheme(Utf16String const& color_scheme);
    void set_page_focus(bool has_focus);
    void set_system_visibility_state(Utf16String const& state);
    Utf16String canvas_color_scheme();
    WebIDL::ExceptionOr<GC::Ref<JS::Object>> image_animation_state_for_url(Utf16String const& url);
    bool media_element_is_fetching(HTML::HTMLMediaElement&);
    bool media_element_is_playing_audio(HTML::HTMLMediaElement&);
    bool media_element_video_sink_is_ticking(HTML::HTMLMediaElement&);
    void set_media_element_ready_state(HTML::HTMLMediaElement&, u16 ready_state);
    void set_media_element_paused(HTML::HTMLMediaElement&, bool paused);
    void set_media_element_seeking(HTML::HTMLMediaElement&, bool seeking);
    void set_page_muted(bool muted);
    WebIDL::UnsignedLongLong active_image_style_value_animation_count();
    Compositor::AsyncScrollingState async_scrolling_state();
    GC::Ref<JS::Object> async_scrolling_state_object();
    bool async_scrolling_state_blocks_wheel_event_at(double x, double y);
    bool async_scrolling_state_can_wheel_scroll_at(double x, double y, double delta_x, double delta_y, bool precise, bool force_stale_wheel_event_regions);
    Utf16String async_scrolling_state_wheel_routing_admission();
    Utf16String async_scrolling_state_wheel_scroll_admission_at(double x, double y, double delta_x, double delta_y, bool precise, bool force_stale_wheel_event_regions);
    Utf16String async_scrolling_state_wheel_target_at(double x, double y, double delta_x, double delta_y, bool precise, Bindings::ScrollGesturePhase);
    String viewport_overflow_x();

private:
    explicit Internals(HTML::Window&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    UIEvents::MouseButton button_from_unsigned_short(WebIDL::UnsignedShort button);

    Vector<GC::Ref<InternalGamepad>> m_gamepads;
};

}
