/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Touch.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::UIEvents {

class Touch final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Touch, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Touch);

public:
    static GC::Ref<Touch> create(Bindings::TouchInit const&);
    GC::Ref<Touch> clone_with_target(GC::Ref<DOM::EventTarget>) const;
    GC::Ref<DOM::EventTarget> target() const { return m_target; }
    i32 identifier() const { return m_identifier; }
    double screen_x() const { return m_screen_x; }
    double screen_y() const { return m_screen_y; }
    double client_x() const { return m_client_x; }
    double client_y() const { return m_client_y; }
    double page_x() const { return m_page_x; }
    double page_y() const { return m_page_y; }
    float radius_x() const { return m_radius_x; }
    float radius_y() const { return m_radius_y; }
    float rotation_angle() const { return m_rotation_angle; }
    float force() const { return m_force; }

private:
    explicit Touch(Bindings::TouchInit const&);
    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<DOM::EventTarget> m_target;
    i32 m_identifier;
    double m_screen_x;
    double m_screen_y;
    double m_client_x;
    double m_client_y;
    double m_page_x;
    double m_page_y;
    float m_radius_x;
    float m_radius_y;
    float m_rotation_angle;
    float m_force;
};

}
