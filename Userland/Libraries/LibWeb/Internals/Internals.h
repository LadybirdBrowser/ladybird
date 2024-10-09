/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Internals/InternalAnimationTimeline.h>
#include <LibWeb/UIEvents/MouseButton.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Internals {

class Internals final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Internals, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(Internals);

public:
    virtual ~Internals() override;

    void signal_text_test_is_done(String const& text);

    void gc();
    JS::Object* hit_test(double x, double y);

    void send_text(HTML::HTMLElement&, String const&, WebIDL::UnsignedShort modifiers);
    void send_key(HTML::HTMLElement&, String const&, WebIDL::UnsignedShort modifiers);
    void commit_text();

    void click(double x, double y);
    void middle_click(double x, double y);
    void move_pointer_to(double x, double y);
    void wheel(double x, double y, double delta_x, double delta_y);

    WebIDL::ExceptionOr<bool> dispatch_user_activated_event(DOM::EventTarget&, DOM::Event& event);

    void spoof_current_url(String const& url);

    JS::NonnullGCPtr<InternalAnimationTimeline> create_internal_animation_timeline();

    void simulate_drag_start(double x, double y, String const& name, String const& contents);
    void simulate_drag_move(double x, double y);
    void simulate_drop(double x, double y);

private:
    explicit Internals(JS::Realm&);
    virtual void initialize(JS::Realm&) override;

    void click(double x, double y, UIEvents::MouseButton);

    HTML::Window& internals_window() const;
    Page& internals_page() const;
};

}
