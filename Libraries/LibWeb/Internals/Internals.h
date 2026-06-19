/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Internals/InternalAnimationTimeline.h>
#include <LibWeb/Internals/InternalsBase.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Internals {

class WEB_API Internals final : public InternalsBase {
    WEB_PLATFORM_OBJECT(Internals, InternalsBase);
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

    void signal_test_is_done(String const& text);
    void set_test_timeout(double milliseconds);
    WebIDL::ExceptionOr<void> load_reference_test_metadata();

    WebIDL::ExceptionOr<String> set_time_zone(StringView time_zone);

    void gc();
    GC::Ref<WebIDL::Promise> gc_async();
    WebIDL::ExceptionOr<void> mark_as_garbage(StringView variable_name);
    JS::Object* hit_test(double x, double y);

    void send_text(HTML::HTMLElement&, String const&, WebIDL::UnsignedShort modifiers);
    void send_key(HTML::HTMLElement&, String const&, WebIDL::UnsignedShort modifiers);
    void paste(HTML::HTMLElement& target, Utf16String const& text);
    void commit_text();

    // Low-level mouse primitives
    void mouse_down(double x, double y, WebIDL::UnsignedShort click_count, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers);
    void mouse_up(double x, double y, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers);
    void mouse_move(double x, double y, WebIDL::UnsignedShort modifiers);
    void mouse_leave();

    // High-level mouse conveniences
    void click(double x, double y, WebIDL::UnsignedShort click_count, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers);
    void click_and_hold(double x, double y, WebIDL::UnsignedShort click_count, WebIDL::UnsignedShort button, WebIDL::UnsignedShort modifiers);
    GC::Ref<WebIDL::Promise> wheel(double x, double y, double delta_x, double delta_y);
    void pinch(double x, double y, double scale_delta, WebIDL::UnsignedShort modifiers);

    String current_cursor();

    String selected_text_for_clipboard();

    void set_marked_text_from_input_method(Utf16String const& text);
    void commit_text_from_input_method(Utf16String const& text);
    void unmark_text_from_input_method();
    GC::Ptr<Geometry::DOMRect> current_caret_rect();

    WebIDL::ExceptionOr<bool> dispatch_user_activated_event(DOM::EventTarget&, DOM::Event& event);

    void spoof_current_url(String const& url);
    void load_url(String const& url);

    GC::Ref<InternalAnimationTimeline> create_internal_animation_timeline();

    void simulate_drag_start(double x, double y, String const& name, String const& contents);
    void simulate_drag_move(double x, double y);
    void simulate_drop(double x, double y);

    void expire_cookies_with_time_offset(WebIDL::LongLong seconds);
    GC::Ref<WebIDL::Promise> delete_all_cookies();

    bool set_http_memory_cache_enabled(bool enabled);
    WebIDL::ExceptionOr<void> set_content_blockers(String const& patterns);
    void set_content_blocking_enabled(bool enabled);
    void set_autoplay_policy(String const& policy);

    String get_computed_role(DOM::Element& element);
    String get_computed_label(DOM::Element& element);
    String get_computed_aria_level(DOM::Element& element);

    static u16 get_echo_server_port();
    static void set_echo_server_port(u16 port);

    void set_hsts_policy(String const& domain, u64 max_age, bool include_sub_domains);
    void ingest_hsts_header(String const& url, String const& header_value);
    bool is_known_hsts_host(String const& domain);

    void set_browser_zoom(double factor);
    void set_device_pixel_ratio(double ratio);

    bool headless();

    String dump_display_list();
    String dump_accessibility_tree();
    String dump_layout_tree(GC::Ref<DOM::Node>);
    String dump_paintable_tree(GC::Ref<DOM::Node>);
    String dump_stacking_context_tree();
    String dump_gc_graph();
    String dump_session_history();
    String dump_ui_process_session_history();
    String dump_site_isolation_process_tree();
    GC::Ref<WebIDL::Promise> flush_session_history_traversal_queue();
    void clobber_next_navigation_with_a_traversal();

    GC::Ptr<DOM::ShadowRoot> get_shadow_root(GC::Ref<DOM::Element>);

    void handle_sdl_input_events();

    GC::Ref<InternalGamepad> connect_virtual_gamepad();
    void disconnect_virtual_gamepad(GC::Ref<InternalGamepad>);

    void perform_per_test_cleanup();

    void set_highlighted_node(GC::Ptr<DOM::Node> node);

    void clear_element(HTML::HTMLElement&);
    void set_environments_top_level_url(StringView url);

    JS::Object* get_style_invalidation_counters();
    void reset_style_invalidation_counters();
    void update_style();
    void set_preferred_color_scheme(StringView color_scheme);
    String canvas_color_scheme();
    bool style_sheet_may_have_has_selectors(CSS::CSSStyleSheet&);
    WebIDL::ExceptionOr<JS::Object*> image_animation_state_for_url(String const& url);
    JS::Object* async_scrolling_state();
    bool async_scrolling_state_blocks_wheel_event_at(double x, double y);
    bool async_scrolling_state_can_wheel_scroll_at(double x, double y, double delta_x, double delta_y, bool force_stale_wheel_event_regions);
    String async_scrolling_state_wheel_routing_admission();
    String async_scrolling_state_wheel_scroll_admission_at(double x, double y, double delta_x, double delta_y, bool force_stale_wheel_event_regions);
    String async_scrolling_state_wheel_target_at(double x, double y, double delta_x, double delta_y);

private:
    explicit Internals(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    UIEvents::MouseButton button_from_unsigned_short(WebIDL::UnsignedShort button);

    Vector<GC::Ref<InternalGamepad>> m_gamepads;
};

}
