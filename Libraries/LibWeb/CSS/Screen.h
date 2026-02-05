/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Window.h>

namespace Web::CSS {

class Screen final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(Screen, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Screen);

public:
    [[nodiscard]] static GC::Ref<Screen> create(HTML::Window&);

    i32 width() const;
    i32 height() const;
    i32 avail_width() const;
    i32 avail_height() const;
    u32 color_depth() const;
    u32 pixel_depth() const;
    GC::Ref<ScreenOrientation> orientation();

    bool is_extended() const;

    void set_onchange(GC::Ptr<WebIDL::CallbackType> event_handler);
    GC::Ptr<WebIDL::CallbackType> onchange();

private:
    explicit Screen(HTML::Window&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    HTML::Window const& window() const { return *m_window; }

    GC::Ref<HTML::Window> m_window;
    GC::Ptr<ScreenOrientation> m_orientation;
};

}
